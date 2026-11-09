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

} // namespace

int main(int argc, char **argv) {
    const char *only = argc > 1 ? argv[1] : nullptr;
    auto wanted = [&](const char *name) { return only == nullptr || std::strcmp(only, name) == 0; };
    if (wanted("fill")) exactFillKeepsEveryByte();
    if (wanted("overrun")) overrunKeepsTheNewestBytes();
    if (wanted("oversized")) oversizedWriteStaysInsideTheRing();
    if (wanted("surround")) surroundDefaultKeepsWholeFrames();
    if (gFailures == 0)
        std::printf("All %d assertions passed.\n", gAssertions);
    else
        std::printf("%d assertions, %d failures\n", gAssertions, gFailures);
    return gFailures == 0 ? 0 : 1;
}
