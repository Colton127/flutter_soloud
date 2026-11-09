// Standalone native tests for the mixer-output capture ring (MixerOutput):
// what it keeps when the reader falls behind, what a single large write does,
// how fixed-size chunks are emitted, and how long the data a notification
// points at stays valid.
//
// The audio thread's side is driven by hand: each test calls onAudioData()
// with frames whose samples count up (0, 1, 2, ...), so the bytes the ring
// holds can be checked for order and for which ones were kept. The
// notification thread is the real one.
//
//   * an exact fill keeps every byte, and an overrun keeps the newest bytes,
//     in order, rather than discarding the ring;
//   * a single write larger than the whole ring keeps its newest bytes and
//     writes nothing outside the ring (AddressSanitizer reports it otherwise);
//   * in fixed-chunk mode every complete chunk is delivered, in order, also
//     when several are buffered before the notification thread looks;
//   * the data a notification hands over stays readable and unchanged when
//     its consumer reads it later, as Dart's NativeCallable.listener does:
//     after the next fixed chunk, and after the capture is stopped and
//     started again with a larger ring.
//
// Build and run from the flutter_soloud repository root with:
//
//   ./test/run_mixer_output_test.sh            (plain)
//   ./test/run_mixer_output_test.sh --asan     (AddressSanitizer)
//   ./test/run_mixer_output_test.sh --tsan     (ThreadSanitizer)
//
// A scenario name after the flag (fill, overrun, oversized, chunks, deferred,
// allocation or surround) runs only that scenario.

#include "mixeroutput/mixer_output.h"
#include "mixeroutput/dart_port.h"
#include "device_lifecycle_test_hooks.h"

#include <atomic>
#include <chrono>
#include <cstdio>
#include <cstring>
#include <cstdlib>
#include <mutex>
#include <thread>
#include <vector>

namespace {

int gFailures = 0;
int gAssertions = 0;

#define EXPECT(condition, format, ...) do { \
    ++gAssertions; \
    if (!(condition)) { \
        ++gFailures; \
        std::fprintf(stderr, "  FAIL [%s:%d] " format "\n", \
                     __FILE__, __LINE__, ##__VA_ARGS__); \
    } \
} while (0)

constexpr int kChannels = 2;
constexpr int kSampleRate = 48000;
constexpr size_t kBytesPerFrame = kChannels * sizeof(float);

MixerOutput &capture() { return MixerOutput::instance(); }

/// Feeds [frames] frames whose samples continue the count from [next].
void feed(unsigned int frames, float &next)
{
    std::vector<float> block(static_cast<size_t>(frames) * kChannels);
    for (auto &sample : block)
        sample = next++;
    capture().onAudioData(block.data(), frames);
}

/// The unread bytes of the ring, in order, as floats.
std::vector<float> unread()
{
    const size_t available = capture().getAvailableBytes();
    const size_t size = capture().getBufferSize();
    const size_t read = capture().getReadOffset();
    const uint8_t *ring = capture().getBufferPointer();
    std::vector<uint8_t> bytes(available);
    for (size_t i = 0; i < available; ++i)
        bytes[i] = ring[(read + i) % size];
    std::vector<float> floats(available / sizeof(float));
    std::memcpy(floats.data(), bytes.data(), floats.size() * sizeof(float));
    return floats;
}

/// True when [samples] are exactly the last samples.size() values counted
/// before [next].
bool isNewestRun(const std::vector<float> &samples, float next)
{
    for (size_t i = 0; i < samples.size(); ++i)
        if (samples[i] != next - static_cast<float>(samples.size() - i))
            return false;
    return true;
}

template <typename Predicate>
bool waitFor(Predicate predicate, int timeoutMs)
{
    const auto deadline = std::chrono::steady_clock::now() +
                          std::chrono::milliseconds(timeoutMs);
    while (!predicate())
    {
        if (std::chrono::steady_clock::now() > deadline)
            return false;
        std::this_thread::sleep_for(std::chrono::milliseconds(1));
    }
    return true;
}

PlayerErrors startPcm(size_t ringBytes, size_t thresholdBytes, int chunkFrames)
{
    return capture().start(MIXER_OUTPUT_PCM_F32LE, kSampleRate, kChannels,
                           ringBytes, thresholdBytes, chunkFrames);
}

// No consumer: nothing advances the read position. A frame-sized slack is
// allowed for a ring that keeps one frame free to tell full from empty.
void exactFillKeepsEveryByte()
{
    std::printf("an exact fill keeps the ring's bytes\n");
    capture().setDataCallback(nullptr);
    EXPECT(startPcm(4096, 4096, -1) == noError, "fill: start");
    float next = 0;
    feed(256, next); // 2048 bytes
    EXPECT(capture().getAvailableBytes() == 2048,
           "fill: half full holds 2048 bytes, got %zu", capture().getAvailableBytes());
    feed(256, next); // 4096 bytes: exactly the ring
    const size_t available = capture().getAvailableBytes();
    EXPECT(available + kBytesPerFrame >= 4096 && available <= 4096,
           "fill: an exactly full ring must hold its data, %zu of 4096 bytes readable",
           available);
    EXPECT(isNewestRun(unread(), next), "fill: the kept bytes must be the newest, in order");
    capture().stop();
}

void overrunKeepsTheNewestBytes()
{
    std::printf("an overrun drops only the oldest bytes\n");
    capture().setDataCallback(nullptr);
    EXPECT(startPcm(4096, 4096, -1) == noError, "overrun: start");
    float next = 0;
    feed(128, next); // 1024 bytes
    feed(448, next); // 3584 more: 512 past the ring
    size_t available = capture().getAvailableBytes();
    EXPECT(available + kBytesPerFrame >= 4096 && available <= 4096,
           "overrun: a ring overrun by 512 bytes must stay full, %zu of 4096 bytes readable",
           available);
    EXPECT(isNewestRun(unread(), next), "overrun: the kept bytes must be the newest, in order");
    for (int block = 0; block < 5; ++block)
        feed(300, next); // keeps overrunning by uneven amounts
    available = capture().getAvailableBytes();
    EXPECT(available + kBytesPerFrame >= 4096 && available <= 4096,
           "overrun: repeated overruns must leave the ring full, %zu of 4096 bytes readable",
           available);
    EXPECT(isNewestRun(unread(), next), "overrun: repeated overruns must keep the newest bytes");
    capture().stop();
}

// Under AddressSanitizer a write outside the ring aborts the run here.
void oversizedWriteStaysInsideTheRing()
{
    std::printf("a write larger than the ring keeps its newest bytes\n");
    capture().setDataCallback(nullptr);
    EXPECT(startPcm(4096, 4096, -1) == noError, "oversized: start");
    float next = 0;
    feed(16, next);   // move the write position off 0
    feed(2048, next); // 16384 bytes into a 4096-byte ring
    const size_t available = capture().getAvailableBytes();
    EXPECT(available + kBytesPerFrame >= 4096 && available <= 4096,
           "oversized: the ring must be full after the write, %zu of 4096 bytes readable",
           available);
    EXPECT(isNewestRun(unread(), next), "oversized: the kept bytes must be the write's last ones");
    capture().stop();
}

std::mutex gChunkMutex;
std::vector<std::vector<float>> gChunks; // copied when the callback runs

void copyChunk(uint8_t *data, size_t length)
{
    uint32_t captureId = 0;
    data = MixerOutput::takePacket(data, captureId);
    std::vector<float> chunk(length / sizeof(float));
    std::memcpy(chunk.data(), data, chunk.size() * sizeof(float));
    std::free(data);
    std::lock_guard<std::mutex> lock(gChunkMutex);
    gChunks.push_back(std::move(chunk));
}

size_t chunkCount()
{
    std::lock_guard<std::mutex> lock(gChunkMutex);
    return gChunks.size();
}

void everyBufferedChunkIsDelivered()
{
    std::printf("fixed chunks: every buffered chunk is delivered, in order\n");
    constexpr int kChunkFrames = 2048;
    constexpr size_t kChunkFloats = kChunkFrames * kChannels;
    {
        std::lock_guard<std::mutex> lock(gChunkMutex);
        gChunks.clear();
    }
    capture().setDataCallback(copyChunk);
    EXPECT(startPcm(1 << 20, 4096, kChunkFrames) == noError, "chunks: start");
    float next = 0;
    // Three chunks in one callback: a device period of 6144 frames, or a
    // notification thread that ran late. All three are buffered at once.
    feed(3 * kChunkFrames, next);
    EXPECT(waitFor([] { return chunkCount() >= 3; }, 2000),
           "chunks: all 3 buffered chunks must be delivered, got %zu", chunkCount());
    // And the stream keeps going afterwards.
    feed(kChunkFrames, next);
    EXPECT(waitFor([] { return chunkCount() >= 4; }, 2000),
           "chunks: a 4th chunk must follow, got %zu", chunkCount());
    capture().setDataCallback(nullptr);
    capture().stop();
    std::lock_guard<std::mutex> lock(gChunkMutex);
    for (size_t c = 0; c < gChunks.size(); ++c)
    {
        bool inOrder = gChunks[c].size() == kChunkFloats;
        for (size_t i = 0; inOrder && i < kChunkFloats; ++i)
            inOrder = gChunks[c][i] == static_cast<float>(c * kChunkFloats + i);
        EXPECT(inOrder, "chunks: chunk %zu must hold samples %zu.. in order", c, c * kChunkFloats);
    }
}

// Dart receives notifications through a NativeCallable.listener: the native
// call only queues them, and Dart reads the bytes later. These callbacks keep
// the pointer and read it after the capture has moved on.
struct Held
{
    uint8_t *data;
    size_t length;
};
std::mutex gHeldMutex;
std::vector<Held> gHeld;

void holdPointer(uint8_t *data, size_t length)
{
    std::lock_guard<std::mutex> lock(gHeldMutex);
    gHeld.push_back({data, length});
}

size_t heldCount()
{
    std::lock_guard<std::mutex> lock(gHeldMutex);
    return gHeld.size();
}

/// The first [count] floats a held notification points at, read now.
std::vector<float> readHeld(size_t index, size_t count)
{
    std::lock_guard<std::mutex> lock(gHeldMutex);
    std::vector<float> floats(count);
    uint32_t captureId = 0;
    auto *bytes = MixerOutput::takePacket(gHeld[index].data, captureId);
    EXPECT(bytes != nullptr, "held packet must remain claimable");
    if (bytes != nullptr) std::memcpy(floats.data(), bytes, count * sizeof(float));
    std::free(bytes);
    return floats;
}

void heldNotificationsStayValid()
{
    std::printf("held notifications: data stays valid and unchanged\n");
    {
        std::lock_guard<std::mutex> lock(gHeldMutex);
        gHeld.clear();
    }

    // Fixed chunks: the first chunk's data must not become the second's.
    constexpr int kChunkFrames = 2048;
    constexpr size_t kChunkFloats = kChunkFrames * kChannels;
    capture().setDataCallback(holdPointer);
    EXPECT(startPcm(1 << 20, 4096, kChunkFrames) == noError, "held chunks: start");
    float next = 0;
    feed(kChunkFrames, next);
    EXPECT(waitFor([] { return heldCount() >= 1; }, 2000), "held chunks: first chunk");
    // Let the notification thread see the ring drained before the next chunk,
    // so this does not depend on the backlog handling the chunks scenario
    // checks.
    std::this_thread::sleep_for(std::chrono::milliseconds(50));
    feed(kChunkFrames, next);
    EXPECT(waitFor([] { return heldCount() >= 2; }, 2000), "held chunks: second chunk");
    if (heldCount() >= 2)
    {
        const auto first = readHeld(0, 4);
        EXPECT(first[0] == 0 && first[3] == 3,
               "held chunks: the first chunk read late must still hold samples 0.., got %g..",
               first[0]);
        const auto second = readHeld(1, 4);
        EXPECT(second[0] == static_cast<float>(kChunkFloats),
               "held chunks: the second chunk must hold samples %zu.., got %g..",
               kChunkFloats, second[0]);
    }
    capture().setDataCallback(nullptr);
    capture().stop();

    // A pending chunk also survives a restart that grows the packet pool's
    // slot capacity. The replacement capture must use independent storage.
    capture().setDataCallback(holdPointer);
    EXPECT(startPcm(1 << 20, 4096, kChunkFrames) == noError, "held resize: start");
    const size_t oldIndex = heldCount();
    next = 0;
    feed(kChunkFrames, next);
    EXPECT(waitFor([&] { return heldCount() > oldIndex; }, 2000), "held resize: packet");
    capture().stop();
    EXPECT(startPcm(1 << 20, 4096, kChunkFrames * 2) == noError, "held resize: restart");
    next = 90000;
    feed(kChunkFrames * 2, next);
    if (heldCount() > oldIndex) {
        const auto first = readHeld(oldIndex, 4);
        EXPECT(first[0] == 0 && first[3] == 3,
               "held resize: growing a slot must preserve the old payload");
    }
    capture().setDataCallback(nullptr);
    capture().stop();
    MixerOutput::discardPackets();

    // Threshold mode: a notification pending when the capture is restarted
    // with a larger ring must not read the old, freed ring.
    {
        std::lock_guard<std::mutex> lock(gHeldMutex);
        gHeld.clear();
    }
    capture().setDataCallback(holdPointer);
    EXPECT(startPcm(256 * 1024, 4096, -1) == noError, "held ring: start");
    next = 0;
    feed(1024, next); // 8192 bytes, past the threshold
    EXPECT(waitFor([] { return heldCount() >= 1; }, 2000), "held ring: notification");
    capture().setDataCallback(nullptr);
    capture().stop();
    EXPECT(startPcm(4 * 1024 * 1024, 4096, -1) == noError, "held ring: restart larger");
    // Overwrite whatever might reuse the old storage.
    std::vector<uint8_t> scribble(256 * 1024, 0xAB);
    if (heldCount() >= 1)
    {
        // Under AddressSanitizer a read of the freed ring aborts the run here.
        const auto first = readHeld(0, 4);
        EXPECT(first[0] == 0 && first[3] == 3,
               "held ring: data read after a restart must still be the first samples, got %g..",
               first[0]);
    }
    capture().stop();
}

// Retirement owns unclaimed packets, including events a closed listener can
// never receive. Tokens cannot become valid again if an allocation is reused.
void retiredPacketsCannotBeClaimedAgain()
{
    const uint8_t bytes[] = {1, 2, 3, 4};
    auto *old = MixerOutput::copyPacket(bytes, sizeof(bytes), 1);
    MixerOutput::discardPackets();
    uint32_t id = 0;
    EXPECT(MixerOutput::takePacket(old, id) == nullptr,
           "retired packet must be inert");
    auto *current = MixerOutput::copyPacket(bytes, sizeof(bytes), 2);
    EXPECT(current != old, "packet tokens must not be reused");
    EXPECT(MixerOutput::takePacket(old, id) == nullptr,
           "old token must not claim a replacement allocation");
    auto *copy = MixerOutput::takePacket(current, id);
    EXPECT(copy != nullptr && id == 2 && std::memcmp(copy, bytes, sizeof(bytes)) == 0,
           "current packet remains readable");
    std::free(copy);
    EXPECT(MixerOutput::takePacket(current, id) == nullptr,
           "packet can only be claimed once");
    size_t queued = 0;
    for (int i = 0; i < 128; ++i)
        if (MixerOutput::copyPacket(bytes, sizeof(bytes), 2) != nullptr) ++queued;
    EXPECT(queued > 0 && queued <= 64,
           "a stalled consumer must have a bounded pending packet queue");
    MixerOutput::discardPackets();
}

#ifndef __EMSCRIPTEN__
void allocationDoesNotHoldTheRingLock()
{
    capture().setDataCallback(holdPointer);
    EXPECT(startPcm(256 * 1024, 4096, -1) == noError, "allocation: start");
    soloud_test::armBarrier(soloud_test::DeviceBarrier::mixerPacketStorageAllocated);
    float next = 0;
    feed(1024, next);
    soloud_test::waitBarrierReached(soloud_test::DeviceBarrier::mixerPacketStorageAllocated);
    std::atomic<bool> completed{false};
    std::thread device([&] {
        float deviceNext = 8192;
        feed(256, deviceNext);
        completed.store(true);
    });
    EXPECT(waitFor([&] { return completed.load(); }, 1000),
           "allocation: audio callback waited for packet allocation");
    soloud_test::releaseBarrier(soloud_test::DeviceBarrier::mixerPacketStorageAllocated);
    device.join();
    capture().setDataCallback(nullptr);
    capture().stop();
    MixerOutput::discardPackets();
}
#endif

void surroundDefaultKeepsWholeFrames()
{
    constexpr int channels = 6;
    constexpr size_t requestedBytes = 1024 * 1024;
    constexpr size_t frameBytes = channels * sizeof(float);
    capture().setDataCallback(nullptr);
    const auto result = capture().start(MIXER_OUTPUT_PCM_F32LE, kSampleRate,
                                        channels, requestedBytes, 4096, -1);
    EXPECT(result == noError, "surround: default 1 MiB buffer must start");
    if (result != noError) return;
    const size_t capacity = capture().getBufferSize();
    EXPECT(capacity == requestedBytes - requestedBytes % frameBytes,
           "surround: buffer must round down to whole frames, got %zu", capacity);
    const unsigned int frames = static_cast<unsigned int>(capacity / frameBytes);
    std::vector<float> data(static_cast<size_t>(frames) * channels);
    float next = 0;
    for (auto &sample : data) sample = next++;
    capture().onAudioData(data.data(), frames);
    EXPECT(capture().getAvailableBytes() == capacity,
           "surround: exact frame fill must keep the whole ring");
    EXPECT(isNewestRun(unread(), next), "surround: fill must preserve frame order");
    data.resize(13 * channels);
    for (auto &sample : data) sample = next++;
    capture().onAudioData(data.data(), 13);
    EXPECT(capture().getAvailableBytes() == capacity,
           "surround: overrun must keep a full ring");
    EXPECT(capture().getReadOffset() % frameBytes == 0,
           "surround: overrun read head must stay on a frame boundary");
    EXPECT(isNewestRun(unread(), next), "surround: overrun must keep newest frames in order");
    capture().stop();
    EXPECT(capture().start(MIXER_OUTPUT_PCM_F32LE, kSampleRate, channels,
                           frameBytes - 1, 1, -1) == invalidParameter,
           "surround: refuse less than one frame");
    EXPECT(capture().start(MIXER_OUTPUT_PCM_F32LE, kSampleRate, channels,
                           requestedBytes, requestedBytes, -1) == invalidParameter,
           "surround: threshold must fit after rounding");
    EXPECT(capture().start(MIXER_OUTPUT_PCM_F32LE, kSampleRate, channels,
                           2048 * frameBytes - 1, 4096, 2048) == invalidParameter,
           "surround: chunk must fit after rounding");
    for (int count : {3, 6}) {
        for (auto format : {MIXER_OUTPUT_PCM_S8, MIXER_OUTPUT_PCM_S16LE}) {
            const size_t bytesPerFrame = count *
                (format == MIXER_OUTPUT_PCM_S8 ? 1 : 2);
            EXPECT(capture().start(format, kSampleRate, count, requestedBytes,
                                   4096, -1) == noError,
                   "surround: integer PCM default must start (%d channels)", count);
            EXPECT(capture().getBufferSize() ==
                       requestedBytes - requestedBytes % bytesPerFrame,
                   "surround: integer PCM buffer must round to whole frames");
            capture().stop();
        }
    }
}

void stoppedCaptureDrainsPendingPackets() {
    {
        std::lock_guard<std::mutex> lock(gHeldMutex);
        gHeld.clear();
    }
    capture().setDataCallback(holdPointer);
    EXPECT(startPcm(1 << 20, 4096, 2048) == noError, "drain: start");
    float next = 0;
    feed(4096 + 8, next);
    EXPECT(waitFor([] { return heldCount() >= 2; }, 2000), "drain: publish two chunks");
    const uint32_t id = capture().captureId();
    capture().stop();
    size_t length = 0;
    uint8_t *bytes = nullptr;
#ifdef SOLOUD_CAPTURE_PACKET_DRAIN
    bytes = MixerOutput::takeStoppedPackets(id, length);
#endif
    std::vector<float> all(length / sizeof(float));
    if (length != 0) std::memcpy(all.data(), bytes, length);
    std::free(bytes);
    const auto tail = unread();
    all.insert(all.end(), tail.begin(), tail.end());
    EXPECT(length == 4096 * kBytesPerFrame, "drain: complete chunks missing (%zu bytes)", length);
    EXPECT(all.size() == static_cast<size_t>(next) && isNewestRun(all, next),
           "drain: packet bytes must precede the ring tail, in order");
    {
        std::lock_guard<std::mutex> lock(gHeldMutex);
        for (const auto &held : gHeld) {
            uint32_t packetId = 0;
            auto *late = MixerOutput::takePacket(held.data, packetId);
            EXPECT(late == nullptr, "drain: delayed notifications must be retired");
            std::free(late);
        }
    }
#ifdef SOLOUD_CAPTURE_PACKET_DRAIN
    bytes = MixerOutput::takeStoppedPackets(id, length);
    EXPECT(length == 0, "drain: second drain must be empty");
    std::free(bytes);
#endif
    capture().setDataCallback(nullptr);
    MixerOutput::discardPackets();
}

} // namespace

static bool portOpen = true;
static int64_t receivedToken = 0;
static bool fakePostInteger(int64_t port, int64_t token) {
    if (!portOpen) return false;
    EXPECT(port == 17, "port: receiver id");
    receivedToken = token;
    return true;
}

static void nativePortPacketsHaveIndependentLifetime() {
    auto &capture = MixerOutput::instance();
    EXPECT(capture.start(MIXER_OUTPUT_PCM_F32LE, 44100, 2, 4096, 4096, -1) == noError,
           "port: prepare capture packet storage");
    capture.stop();
    struct Entry { const char *name; void (*function)(); };
    struct Api { int major; int minor; const Entry *functions; };
    Entry entries[] = {{"Dart_PostInteger", reinterpret_cast<void (*)()>(fakePostInteger)}, {nullptr, nullptr}};
    Api api{2, 0, entries};
    auto post = mixer_output_port::resolve(&api);
    EXPECT(post != nullptr, "port: resolve the versioned VM entry");
    api.major = 3;
    EXPECT(mixer_output_port::resolve(&api) == nullptr, "port: reject incompatible VM ABI");
    const uint8_t payload[] = {1, 2, 3, 4};
    auto *token = MixerOutput::copyPacket(payload, sizeof(payload), 71);
    portOpen = true;
    mixer_output_port::send(post, 17, token);
    EXPECT(receivedToken == static_cast<int64_t>(reinterpret_cast<uintptr_t>(token)), "port: post the token");
    uint32_t id = 0;
    size_t length = 99;
    auto *bytes = MixerOutput::takePacket(token, id, &length);
    EXPECT(bytes != nullptr && id == 71 && length == sizeof(payload), "port: claim packet and length together");
    if (bytes != nullptr) EXPECT(std::memcmp(bytes, payload, sizeof(payload)) == 0, "port: owned bytes");
    std::free(bytes);
    length = 99;
    EXPECT(MixerOutput::takePacket(token, id, &length) == nullptr && length == 0, "port: no duplicate claim");
    token = MixerOutput::copyPacket(payload, sizeof(payload), 72);
    portOpen = false;
    mixer_output_port::send(post, 17, token);
    EXPECT(MixerOutput::takePacket(token, id) == nullptr, "port: failed post retires its packet");
    portOpen = true;
}

int main(int argc, char **argv)
{
    const char *only = argc > 1 ? argv[1] : nullptr;
    const char *const kScenarios[] = {"fill", "overrun", "oversized", "chunks", "deferred", "allocation", "surround", "drain", "port"};
    if (only != nullptr)
    {
        bool known = false;
        for (const char *name : kScenarios)
            known = known || std::strcmp(only, name) == 0;
        if (!known)
        {
            std::fprintf(stderr,
                         "Unknown scenario '%s': use fill, overrun, oversized, chunks, deferred, allocation, surround, drain or port.\n",
                         only);
            return 2;
        }
    }
    auto wanted = [&](const char *name) { return only == nullptr || std::strcmp(only, name) == 0; };

    if (wanted("port"))
        nativePortPacketsHaveIndependentLifetime();
    if (wanted("fill"))
        exactFillKeepsEveryByte();
    if (wanted("overrun"))
        overrunKeepsTheNewestBytes();
    if (wanted("oversized"))
        oversizedWriteStaysInsideTheRing();
    if (wanted("chunks"))
        everyBufferedChunkIsDelivered();
    if (wanted("deferred"))
        heldNotificationsStayValid();
#ifndef __EMSCRIPTEN__
    if (wanted("allocation"))
        allocationDoesNotHoldTheRingLock();
#endif
    if (wanted("surround"))
        surroundDefaultKeepsWholeFrames();
    if (wanted("drain")) stoppedCaptureDrainsPendingPackets();
    retiredPacketsCannotBeClaimedAgain();

    if (gFailures == 0)
        std::printf("All %d assertions passed.\n", gAssertions);
    else
        std::printf("%d assertions, %d failures\n", gAssertions, gFailures);
    return gFailures == 0 ? 0 : 1;
}
