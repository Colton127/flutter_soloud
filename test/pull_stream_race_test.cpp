// Standalone native tests for the locking of a PullBufferStream's decode
// state.
//
// A PullBufferStream is used from three kinds of thread at once:
//
//   * a feeder delivering data (Player::addPullBufferDataStream(), under
//     sounds_mutex), which queues the bytes and decodes them;
//   * the mixer (getAudio(), with SoLoud's audio mutex held), which drains the
//     decoded audio, decodes queued bytes itself when no delivery is doing so
//     (tryDecodePendingData()), asks for more data, and applies seeks and
//     rewinds: SoLoud runs those on the mixer, at the start of a block;
//   * Dart's thread, which resets the stream (resetPullBufferStream()) and
//     reads its buffered range (getPullBufferTimeRange()).
//
// The decoder, the queued bytes, the buffers, the offsets and the pending seek
// are shared between them, and the stream's decode mutex guards them. Seeks,
// rewinds and resets used to change them without it: a seek the mixer applied
// while a feeder was decoding discarded the queue and the buffer under the
// feeder (and, for a compressed stream, replaced the decoder it was using),
// and a reset did the same to the mixer's own decoding.
//
// What these tests pin down:
//
//   * a delivery made while the mixer is in the middle of a seek out of the
//     buffered range, or of a seek to the start (a rewind), parked right
//     before it discards the buffer (the pullSeekDiscarding barrier), does
//     not get to decode (pullDecoding) until the seek is released. It then
//     lands after the seek, as the data at the target;
//   * a reset while the mixer is parked mid-decode does not finish until the
//     mixer is released, and the stream takes data from the start again;
//   * a reset while the mixer is parked between taking samples out of the
//     circular buffer and counting them (pullSamplesRead) waits for it too,
//     and leaves an empty stream with an empty buffered range;
//   * the duration and metadata callbacks, which the web's Dart receives
//     synchronously and may answer by calling back into the stream, are made
//     with the decode mutex released: a callback that reads the stream's
//     buffered range on the delivering thread does not deadlock it;
//   * a raw PCM stream and an MP3 stream, played while a feeder answers their
//     requests for data and other threads seek, reset, pause and read them,
//     come through intact. Under ThreadSanitizer this reports any access to
//     the shared state that the decode mutex does not order, and under
//     AddressSanitizer a decoder or a buffer used after it was replaced.
//
// Without the locking the first two fail their assertion, and the run ends
// there: releasing the parked threads would let the seek or the reset clear
// what the other is decoding into. With the getters locked but the callbacks
// still made under the lock, the third deadlocks (the run ends after a
// bounded wait) or aborts. Without the locking the last reports data races
// under ThreadSanitizer.
//
// Build and run from the flutter_soloud repository root with:
//
//   ./test/run_pull_stream_race_test.sh            (plain)
//   ./test/run_pull_stream_race_test.sh --asan     (AddressSanitizer)
//   ./test/run_pull_stream_race_test.sh --tsan     (ThreadSanitizer)
//
// A scenario name after the flag (seek, rewind, reset, read-reset, callbacks
// or stress) runs only that scenario.

#include "bindings.h"
#include "device_lifecycle_test_hooks.h"
#include "enums.h"

#include <atomic>
#include <chrono>
#include <cmath>
#include <condition_variable>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <fstream>
#include <functional>
#include <memory>
#include <mutex>
#include <random>
#include <thread>
#include <vector>

extern "C"
{
    // Test-only export, compiled in with SOLOUD_LIFECYCLE_TEST_HOOKS.
    int soloudTestSoundCount();
}

namespace
{

using soloud_test::DeviceBarrier;

constexpr int64_t kEngineId = 4002;

// How long a thread is shown to be held off while another is parked holding
// the decode mutex. Without the locking it gets through in well under a
// millisecond.
constexpr long long kParkMs = 300;

// Long enough that the idle policy never stops the device during a test.
constexpr int64_t kQuietIdleTimeoutMs = 600000;

// Bound on waits for work that needs no lock the test holds back. Generous,
// since passing runs take milliseconds; only a deadlocked run waits this long.
constexpr long long kProgressMs = 5000;

// Every stream here is 32-bit float stereo at 44.1 kHz, or decodes to it.
constexpr unsigned int kSampleRate = 44100;
constexpr unsigned int kChannels = 2;
constexpr unsigned int kFrameBytes = kChannels * sizeof(float);

int gFailures = 0;
int gAssertions = 0;

#define EXPECT(condition, format, ...)                                    \
    do                                                                    \
    {                                                                     \
        ++gAssertions;                                                    \
        if (!(condition))                                                 \
        {                                                                 \
            ++gFailures;                                                  \
            std::fprintf(stderr, "  FAIL [%s:%d] " format "\n", __FILE__, \
                         __LINE__, ##__VA_ARGS__);                        \
        }                                                                 \
    } while (0)

// ---------------------------------------------------------------------------
// Bounded waits
// ---------------------------------------------------------------------------

void printSummary()
{
    std::printf("\n%d assertions, %d failures\n", gAssertions, gFailures);
    std::fflush(stdout);
    std::fflush(stderr);
}

/// End the run now: a thread is parked or wedged where releasing it, or
/// waiting for it, cannot end well, and static destruction must not run
/// under it.
[[noreturn]] void endRun(const char *why)
{
    ++gFailures;
    std::fprintf(stderr, "  FAIL: %s\n  Ending the run here.\n", why);
    printSummary();
    std::_Exit(1);
}

/// soloud_test::waitBarrierReached() with a bound, which it does not have: a
/// helper thread waits for the barrier and reports back. The helper stays
/// blocked until the barrier is reached, so after a false return the run
/// either reaches it later or ends (endRun()).
bool waitBarrierReachedFor(DeviceBarrier barrier, long long ms)
{
    struct State
    {
        std::mutex mutex;
        std::condition_variable cv;
        bool reached = false;
    };
    auto state = std::make_shared<State>();
    std::thread(
        [barrier, state]
        {
            soloud_test::waitBarrierReached(barrier);
            {
                std::lock_guard<std::mutex> lock(state->mutex);
                state->reached = true;
            }
            state->cv.notify_all();
        })
        .detach();
    std::unique_lock<std::mutex> lock(state->mutex);
    return state->cv.wait_for(lock, std::chrono::milliseconds(ms),
                              [&] { return state->reached; });
}

/// Runs a call on its own thread, so the test can tell, with a bound, whether
/// it has returned without blocking on whatever the call waits for.
class BackgroundCall
{
public:
    explicit BackgroundCall(std::function<void()> call)
        : mThread(
              [this, call = std::move(call)]
              {
                  call();
                  {
                      std::lock_guard<std::mutex> lock(mMutex);
                      mDone = true;
                  }
                  mCv.notify_all();
              })
    {
    }

    ~BackgroundCall() { join(); }

    bool finishedWithin(long long ms)
    {
        std::unique_lock<std::mutex> lock(mMutex);
        return mCv.wait_for(lock, std::chrono::milliseconds(ms),
                            [&] { return mDone; });
    }

    void join()
    {
        if (mThread.joinable())
            mThread.join();
    }

private:
    // Declared before the thread, which uses them from its first instruction.
    std::mutex mMutex;
    std::condition_variable mCv;
    bool mDone = false;
    std::thread mThread;
};

/// Abort a wedged run instead of hanging. macOS has no `timeout` command.
void startHangGuard(int limitSeconds)
{
    std::thread(
        [limitSeconds]
        {
            std::this_thread::sleep_for(std::chrono::seconds(limitSeconds));
            std::fprintf(stderr,
                         "\n  FAIL: hang guard fired after %ds -- the suite is "
                         "wedged, which is itself the bug.\n",
                         limitSeconds);
            std::fflush(stderr);
            std::fflush(stdout);
            std::_Exit(1);
        })
        .detach();
}

// ---------------------------------------------------------------------------
// Engine
// ---------------------------------------------------------------------------

void onVoiceEnded(unsigned int *handle) { std::free(handle); }

void onFileLoaded(enum PlayerErrors *e, char *name, unsigned int *hash,
                  uint64_t *counter)
{
    std::free(e);
    std::free(name);
    std::free(hash);
    std::free(counter);
}

void onStateChanged(enum PlayerStateEvents *state) { std::free(state); }

bool bringUpEngine()
{
    setAudioDeviceIdleTimeout(kQuietIdleTimeoutMs);
    prepareEngineInit(kEngineId);
    const PlayerErrors err = initEngine(-1, kSampleRate, 2048, kChannels, 1,
                                        0, 0);
    if (err != PlayerErrors::noError)
        return false;
    setDartEventCallback(onVoiceEnded, onFileLoaded, onStateChanged, kEngineId);
    return isInited() != 0;
}

void tearDownEngine()
{
    if (isInited())
        dispose();
}

/// No voice alive, no handle registered, no sound loaded.
void expectNothingLeft(const char *what)
{
    EXPECT(getVoiceCount() == 0, "%s: %u voice(s) still alive", what,
           getVoiceCount());
    EXPECT(getActiveVoiceCount() == 0, "%s: %u handle(s) still registered",
           what, getActiveVoiceCount());
    EXPECT(soloudTestSoundCount() == 0, "%s: %d sound(s) still loaded", what,
           soloudTestSoundCount());
}

// ---------------------------------------------------------------------------
// Streams
// ---------------------------------------------------------------------------

/// [seconds] of raw PCM, in bytes.
unsigned int pcmBytes(double seconds)
{
    return static_cast<unsigned int>(seconds * kSampleRate) * kFrameBytes;
}

/// A pull stream with a buffer of [bufferBytes], declared [audioBytes] long:
/// raw 32-bit float PCM when [format] is PCM_F32LE (no decoder), compressed
/// when it is AUTO.
unsigned int createStream(const char *what, BufferType format,
                          unsigned int bufferBytes, uint64_t audioBytes,
                          dartOnMoreDataIsNeededCallback_t onMoreData = nullptr)
{
    unsigned int hash = 0;
    const PlayerErrors err = setPullBufferStream(
        &hash, bufferBytes, 0.5, kSampleRate, kChannels,
        static_cast<int>(format), audioBytes, nullptr, nullptr, onMoreData,
        nullptr);
    EXPECT(err == PlayerErrors::noError && hash != 0,
           "%s: the pull stream should be created (error %d)", what,
           static_cast<int>(err));
    return err == PlayerErrors::noError ? hash : 0;
}

unsigned int playStream(const char *what, unsigned int hash, bool looping)
{
    unsigned int voice = 0;
    const PlayerErrors err =
        play(hash, 0, 0.05f, 0.0f, /*paused*/ false, looping, 0.0, 0.0, -1, -1,
             1.0f, &voice);
    EXPECT(err == PlayerErrors::noError && voice != 0,
           "%s: the stream should play (error %d)", what,
           static_cast<int>(err));
    return err == PlayerErrors::noError ? voice : 0;
}

/// The whole of [path], relative to the repository root.
std::vector<unsigned char> readFile(const char *path)
{
    std::ifstream in(path, std::ios::binary | std::ios::ate);
    std::vector<unsigned char> data;
    if (!in)
        return data;
    data.resize(static_cast<size_t>(in.tellg()));
    in.seekg(0);
    in.read(reinterpret_cast<char *>(data.data()),
            static_cast<std::streamsize>(data.size()));
    data.resize(static_cast<size_t>(in.gcount()));
    return data;
}

// ---------------------------------------------------------------------------
// Scenarios
// ---------------------------------------------------------------------------

/// The mixer applies a seek to [target] while a feeder delivers data to the
/// same stream. The seek discards what the delivery decodes into (the queued
/// bytes, the buffer window and the counters, and for a compressed stream the
/// decoder), so the two must not overlap. [target] 0 is a rewind.
///
/// The mixer is parked inside the seek, right before it discards the buffer
/// (pullSeekDiscarding), and a feeder then delivers raw PCM. The delivery must
/// not get to decode (pullDecoding) while the seek is parked, and must once
/// the seek is released. It then lands after the seek: for a seek, as the data
/// at the target; for a rewind, as the data at the start.
void seekKeepsADeliveryOut(double target)
{
    const bool rewinds = target <= 0.0;
    const char *what = rewinds ? "delivery during a rewind"
                               : "delivery during a seek out of the buffer";
    std::printf("%s\n", what);

    const unsigned int hash =
        createStream(what, PCM_F32LE, /*bufferBytes*/ 1u << 20,
                     /*audioBytes*/ 64u << 20);
    if (hash == 0)
        return;
    const unsigned int voice = playStream(what, hash, /*looping*/ false);
    if (voice == 0)
    {
        disposeSound(hash);
        expectNothingLeft(what);
        return;
    }

    // Park the mixer inside the seek. The seek call only queues it, but gets
    // its own thread anyway: with no mixer running it would apply the seek
    // itself, and park.
    soloud_test::armBarrier(DeviceBarrier::pullSeekDiscarding);
    PlayerErrors seekResult = PlayerErrors::noError;
    BackgroundCall seeker(
        [&] { seekResult = seek(voice, static_cast<float>(target)); });
    if (!waitBarrierReachedFor(DeviceBarrier::pullSeekDiscarding, kProgressMs))
        endRun("the stream's seek never got to discarding its buffer");

    // A quarter of a second of silence, at the next sequential offset: the
    // seek target, or the start after a rewind.
    constexpr double kDelivered = 0.25;
    const std::vector<float> pcm(pcmBytes(kDelivered) / sizeof(float), 0.0f);
    soloud_test::armBarrier(DeviceBarrier::pullDecoding);
    PlayerErrors feedResult = PlayerErrors::noError;
    BackgroundCall feeder(
        [&]
        {
            feedResult = addPullBufferDataStream(
                hash, reinterpret_cast<const unsigned char *>(pcm.data()),
                static_cast<unsigned int>(pcm.size() * sizeof(float)), 0);
        });

    // Bounded on purpose: with the locking, the delivery waits for the parked
    // seek.
    const bool decodedDuringSeek =
        waitBarrierReachedFor(DeviceBarrier::pullDecoding, kParkMs);
    EXPECT(!decodedDuringSeek,
           "%s: the delivery decoded while the mixer was in the middle of the "
           "seek, without the decode mutex",
           what);
    if (decodedDuringSeek)
        endRun("releasing the seek now would discard the buffer and the queue "
               "under the delivery");

    soloud_test::releaseBarrier(DeviceBarrier::pullSeekDiscarding);
    if (!waitBarrierReachedFor(DeviceBarrier::pullDecoding, kProgressMs))
        endRun("the delivery never decoded once the seek was done");
    soloud_test::releaseBarrier(DeviceBarrier::pullDecoding);
    feeder.join();
    seeker.join();
    EXPECT(feedResult == PlayerErrors::noError,
           "%s: the delivery should be added (error %d)", what,
           static_cast<int>(feedResult));
    EXPECT(seekResult == PlayerErrors::noError,
           "%s: the seek should be accepted (error %d)", what,
           static_cast<int>(seekResult));

    // The delivery came after the seek: the buffered range is the delivered
    // audio, from the target. The voice playing it does not move the range.
    double start = -1.0;
    double end = -1.0;
    const PlayerErrors rangeResult = getPullBufferTimeRange(hash, &start, &end);
    EXPECT(rangeResult == PlayerErrors::noError &&
               std::fabs(start - target) < 1e-6 &&
               std::fabs(end - (target + kDelivered)) < 1e-6,
           "%s: the buffered range should be [%.3f, %.3f] s, got [%.6f, %.6f] "
           "s (error %d)",
           what, target, target + kDelivered, start, end,
           static_cast<int>(rangeResult));
    const double position = getPosition(voice);
    EXPECT(position >= target - 1e-3,
           "%s: the voice should play from %.3f s (position %.3f s)", what,
           target, position);

    disposeSound(hash);
    expectNothingLeft(what);
}

/// resetPullBufferStream() starts the stream over: it clears the queued bytes,
/// the buffers and the counters, and for a compressed stream replaces the
/// decoder. The mixer decodes queued bytes itself (getAudio() ->
/// tryDecodePendingData()), so a reset must wait for it.
///
/// The stream's buffer holds a tenth of a second, and a delivery queues two
/// seconds, so the mixer keeps decoding as it plays. It parks mid-decode,
/// holding the decode mutex (pullDecoding). A reset must not finish while it
/// is parked, and must once it is released.
void resetWaitsForTheMixerDecoding()
{
    const char *what = "reset during the mixer's decode";
    std::printf("%s\n", what);

    const unsigned int hash = createStream(
        what, PCM_F32LE, pcmBytes(0.1), /*audioBytes*/ 64u << 20);
    if (hash == 0)
        return;
    const unsigned int voice = playStream(what, hash, /*looping*/ false);
    if (voice == 0)
    {
        disposeSound(hash);
        expectNothingLeft(what);
        return;
    }

    // The delivery decodes what fits in the buffer and queues the rest, which
    // the mixer decodes as the voice plays.
    const std::vector<float> pcm(kSampleRate * 2 * kChannels, 0.0f);
    const auto *bytes = reinterpret_cast<const unsigned char *>(pcm.data());
    PlayerErrors err = addPullBufferDataStream(
        hash, bytes, static_cast<unsigned int>(pcm.size() * sizeof(float)), 0);
    EXPECT(err == PlayerErrors::noError,
           "%s: the delivery should be added (error %d)", what,
           static_cast<int>(err));

    soloud_test::armBarrier(DeviceBarrier::pullDecoding);
    if (!waitBarrierReachedFor(DeviceBarrier::pullDecoding, kProgressMs))
        endRun("the mixer never decoded the queued data");

    PlayerErrors resetResult = PlayerErrors::noError;
    BackgroundCall resetter([&] { resetResult = resetPullBufferStream(hash); });
    const bool resetDuringDecode = resetter.finishedWithin(kParkMs);
    EXPECT(!resetDuringDecode,
           "%s: the reset finished while the mixer was decoding, without the "
           "decode mutex",
           what);
    if (resetDuringDecode)
        endRun("releasing the mixer now would decode into the stream the "
               "reset has just cleared");

    soloud_test::releaseBarrier(DeviceBarrier::pullDecoding);
    if (!resetter.finishedWithin(kProgressMs))
        endRun("the reset never finished once the mixer had decoded");
    resetter.join();
    EXPECT(resetResult == PlayerErrors::noError,
           "%s: the reset should succeed (error %d)", what,
           static_cast<int>(resetResult));

    // The stream starts over and takes data from the start again.
    err = addPullBufferDataStream(hash, bytes, pcmBytes(0.1), 0);
    EXPECT(err == PlayerErrors::noError,
           "%s: a delivery after the reset should be added (error %d)", what,
           static_cast<int>(err));

    disposeSound(hash);
    expectNothingLeft(what);
}

// Reset must also exclude consumption, after decoding has released its lock.
// Park a read between removing samples and accounting for them; an empty
// reset stream must not inherit that old read's counter increment.
void resetWaitsForTheMixerReading()
{
    const char *what = "reset during the mixer's circular read";
    const auto hash = createStream(what, PCM_F32LE, pcmBytes(1), 64u << 20);
    const auto voice = playStream(what, hash, false);
    soloud_test::armBarrier(DeviceBarrier::pullSamplesRead);
    const std::vector<float> pcm(kSampleRate * kChannels, 0.125f);
    EXPECT(addPullBufferDataStream(hash, reinterpret_cast<const unsigned char *>(pcm.data()),
               pcm.size() * sizeof(float), 0) == noError, "read/reset: feed PCM");
    if (!waitBarrierReachedFor(DeviceBarrier::pullSamplesRead, kProgressMs))
        endRun("mixer never reached its circular read");
    PlayerErrors result = unknownError;
    BackgroundCall resetter([&] { result = resetPullBufferStream(hash); });
    EXPECT(!resetter.finishedWithin(kParkMs),
           "read/reset: reset must not finish between the circular read and its accounting");
    soloud_test::releaseBarrier(DeviceBarrier::pullSamplesRead);
    resetter.join();
    // Wait for the parked mix to finish without delivering any new data.
    getEngineTime();
    double start = -1, end = -1;
    EXPECT(result == noError && getPullBufferTimeRange(hash, &start, &end) == noError,
           "read/reset: reset and range query should succeed");
    EXPECT(start == 0 && end == 0,
           "read/reset: empty reset range must be [0,0], got [%.9f,%.9f]", start, end);
    disposeSound(hash);
    expectNothingLeft(what);
    (void)voice;
}

// Reading the range must not depend on which half of a mixer's read/count
// update it observes. It must also return while the mixer is parked, without
// taking the audio mutex merely to obtain a consistent range.
void rangeStaysStableDuringAnUnaccountedRead()
{
    const char *what = "buffered range during an unaccounted read";
    const auto hash = createStream(what, PCM_F32LE, pcmBytes(1), 64u << 20);
    playStream(what, hash, false);
    soloud_test::armBarrier(DeviceBarrier::pullSamplesRead);
    const std::vector<float> pcm(kSampleRate * kChannels, 0.125f);
    EXPECT(addPullBufferDataStream(hash,
               reinterpret_cast<const unsigned char *>(pcm.data()),
               pcm.size() * sizeof(float), 0) == noError, "range: feed PCM");
    if (!waitBarrierReachedFor(DeviceBarrier::pullSamplesRead, kProgressMs))
        endRun("mixer never reached the unaccounted circular read");
    double start = -1, end = -1;
    const auto result = getPullBufferTimeRange(hash, &start, &end);
    EXPECT(result == noError && std::fabs(start) < 1e-9 && std::fabs(end - 1) < 1e-9,
           "range: an unaccounted read must keep [0,1], got [%.9f,%.9f]", start, end);
    soloud_test::releaseBarrier(DeviceBarrier::pullSamplesRead);
    disposeSound(hash);
    expectNothingLeft(what);
}

/// Set on the thread a test delivers from, so the callbacks below re-enter
/// the stream only there (see callbacksCanReenterTheStream()).
thread_local bool tReentrantDelivery = false;
std::atomic<unsigned int> gReentryHash{0};
std::atomic<int> gDurationReports{0};
std::atomic<int> gReentries{0};
std::atomic<int> gFailedReentries{0};

/// What a web app's Dart callback may do, synchronously, on the thread that
/// made the callback: read the stream's buffered range.
void reenterTheStream()
{
    if (!tReentrantDelivery)
        return;
    double start = 0.0;
    double end = 0.0;
    if (getPullBufferTimeRange(gReentryHash.load(), &start, &end) !=
        PlayerErrors::noError)
        gFailedReentries.fetch_add(1);
    gReentries.fetch_add(1);
}

void onDurationReentering(double)
{
    gDurationReports.fetch_add(1);
    reenterTheStream();
}

void onMetadataReentering(struct AudioMetadataFFI) { reenterTheStream(); }

/// On the web the stream's Dart callbacks run synchronously, on the thread
/// that makes them, and Dart may call straight back into the stream: read its
/// buffered range, reset it, seek it. Those take the decode mutex, so no
/// callback may be made while it is held. On native the callbacks are
/// listeners that only post; here they re-enter the stream themselves,
/// synchronously, as the web's Dart would.
///
/// The first delivery to an MP3 stream detects the format and reports the
/// duration (and any metadata), from work done under the decode mutex. Each
/// callback made on the delivering thread reads the buffered range. Made
/// with the mutex held, that read would deadlock the delivery, or abort it.
void callbacksCanReenterTheStream()
{
    const char *what = "callbacks re-entering the stream";
    std::printf("%s\n", what);

    const std::vector<unsigned char> mp3 =
        readFile("example/assets/audio/sample-MP3.mp3");
    EXPECT(mp3.size() > 64 * 1024,
           "%s: example/assets/audio/sample-MP3.mp3 should be readable (run "
           "from the repository root)",
           what);
    if (mp3.size() <= 64 * 1024)
        return;

    unsigned int hash = 0;
    PlayerErrors err = setPullBufferStream(
        &hash, pcmBytes(1.0), 0.5, kSampleRate, kChannels,
        static_cast<int>(AUTO), mp3.size(), nullptr, onMetadataReentering,
        nullptr, onDurationReentering);
    EXPECT(err == PlayerErrors::noError && hash != 0,
           "%s: the pull stream should be created (error %d)", what,
           static_cast<int>(err));
    if (err != PlayerErrors::noError || hash == 0)
        return;
    gReentryHash.store(hash);
    gDurationReports.store(0);
    gReentries.store(0);
    gFailedReentries.store(0);

    // Not played: no mixer decodes for it, so every callback is made on the
    // delivering thread.
    PlayerErrors feedResult = PlayerErrors::noError;
    BackgroundCall feeder(
        [&]
        {
            tReentrantDelivery = true;
            feedResult =
                addPullBufferDataStream(hash, mp3.data(), 64 * 1024, 0);
            tReentrantDelivery = false;
        });
    if (!feeder.finishedWithin(kProgressMs))
        endRun("the delivery never returned: a callback it made could not "
               "read the stream, which deadlocks it on the web");
    feeder.join();

    EXPECT(feedResult == PlayerErrors::noError,
           "%s: the delivery should be added (error %d)", what,
           static_cast<int>(feedResult));
    EXPECT(gDurationReports.load() >= 1,
           "%s: the first delivery should report the duration", what);
    EXPECT(gReentries.load() >= 1 && gFailedReentries.load() == 0,
           "%s: every callback should read the stream (%d reads, %d failed)",
           what, gReentries.load(), gFailedReentries.load());

    gReentryHash.store(0);
    disposeSound(hash);
    expectNothingLeft(what);
}

/// The latest offset the stream under test asked for. The callback runs on
/// the mixer or on a delivering thread, so it only records the request; the
/// feeder answers it, as Dart does.
std::atomic<uint64_t> gRequestedOffset{0};
std::atomic<bool> gRequestPending{false};

void onMoreDataNeeded(uint64_t offset)
{
    gRequestedOffset.store(offset, std::memory_order_relaxed);
    gRequestPending.store(true, std::memory_order_release);
}

enum class StreamKind
{
    pcm,
    mp3,
};

/// A stream played while a feeder answers its requests for data and other
/// threads seek it (out of the buffer, and to the start), reset it, pause and
/// unpause its voice and read its buffered range, for a second and a half.
/// The mixer meanwhile drains it, decodes for it and applies the seeks, and
/// rewinds it when it loops.
///
/// Nothing here is timed to collide: the threads simply overlap, many times.
/// It must come through without a crash or an error, and under the
/// sanitizers without a report.
void concurrentUseComesThroughIntact(StreamKind kind)
{
    const char *what = kind == StreamKind::pcm
                           ? "PCM pull stream under concurrent use"
                           : "MP3 pull stream under concurrent use";
    std::printf("%s\n", what);

    // The source the feeder serves the stream from.
    std::vector<unsigned char> source;
    double sourceSeconds = 0.0;
    if (kind == StreamKind::pcm)
    {
        // Four seconds of a quiet ramp.
        source.resize(pcmBytes(4.0));
        auto *samples = reinterpret_cast<float *>(source.data());
        const size_t count = source.size() / sizeof(float);
        for (size_t i = 0; i < count; ++i)
            samples[i] = static_cast<float>(i % 512) / 512.0f * 0.01f;
        sourceSeconds = 4.0;
    }
    else
    {
        source = readFile("example/assets/audio/sample-MP3.mp3");
        EXPECT(!source.empty(),
               "%s: example/assets/audio/sample-MP3.mp3 should be readable "
               "(run from the repository root)",
               what);
        if (source.empty())
            return;
        // At most this long at 128 kbps; seeks past the end are clamped or
        // stay pending, which is fine here.
        sourceSeconds = static_cast<double>(source.size()) * 8.0 / 128000.0;
    }

    gRequestPending.store(false);
    const unsigned int hash =
        createStream(what, kind == StreamKind::pcm ? PCM_F32LE : AUTO,
                     pcmBytes(0.25), source.size(), onMoreDataNeeded);
    if (hash == 0)
        return;
    // Looping, so the voice never ends: the mixer rewinds it instead.
    const unsigned int voice = playStream(what, hash, /*looping*/ true);
    if (voice == 0)
    {
        disposeSound(hash);
        expectNothingLeft(what);
        return;
    }

    std::atomic<bool> stop{false};
    std::atomic<int> deliveries{0};
    std::atomic<int> badDeliveries{0};
    std::atomic<int> seeks{0};
    std::atomic<int> resets{0};
    std::atomic<int> badResets{0};
    std::atomic<int> rangeReads{0};
    std::atomic<int> badRangeReads{0};

    std::vector<std::thread> threads;

    // The feeder: up to 16 KiB from each requested offset.
    threads.emplace_back(
        [&]
        {
            constexpr uint64_t kChunk = 16 * 1024;
            while (!stop.load())
            {
                if (!gRequestPending.exchange(false, std::memory_order_acquire))
                {
                    std::this_thread::sleep_for(std::chrono::milliseconds(1));
                    continue;
                }
                const uint64_t offset =
                    gRequestedOffset.load(std::memory_order_relaxed);
                if (offset >= source.size())
                    continue;
                const uint64_t length =
                    std::min<uint64_t>(kChunk, source.size() - offset);
                const PlayerErrors r = addPullBufferDataStream(
                    hash, source.data() + offset,
                    static_cast<unsigned int>(length), offset);
                deliveries.fetch_add(1);
                if (r != PlayerErrors::noError &&
                    r != PlayerErrors::streamEndedAlready)
                {
                    if (badDeliveries.fetch_add(1) == 0)
                        std::fprintf(stderr,
                                     "  %s: a delivery at %llu failed with "
                                     "error %d\n",
                                     what,
                                     static_cast<unsigned long long>(offset),
                                     static_cast<int>(r));
                }
            }
        });

    // The seeker: anywhere in the stream, and every fourth time to the start.
    threads.emplace_back(
        [&]
        {
            std::mt19937 rng(kind == StreamKind::pcm ? 1u : 2u);
            std::uniform_real_distribution<double> anywhere(
                0.1, sourceSeconds * 0.9);
            int n = 0;
            while (!stop.load())
            {
                std::this_thread::sleep_for(std::chrono::milliseconds(15));
                const double t = (++n % 4 == 0) ? 0.0 : anywhere(rng);
                seek(voice, static_cast<float>(t));
                seeks.fetch_add(1);
            }
        });

    // The resetter.
    threads.emplace_back(
        [&]
        {
            while (!stop.load())
            {
                std::this_thread::sleep_for(std::chrono::milliseconds(150));
                if (resetPullBufferStream(hash) != PlayerErrors::noError)
                    badResets.fetch_add(1);
                resets.fetch_add(1);
            }
        });

    // The pauser: the user pausing and unpausing the voice, on top of the
    // stream pausing it automatically for buffering.
    threads.emplace_back(
        [&]
        {
            bool paused = false;
            while (!stop.load())
            {
                std::this_thread::sleep_for(std::chrono::milliseconds(40));
                paused = !paused;
                setPause(voice, paused);
            }
            setPause(voice, false);
        });

    // The reader: what Dart polls to draw the stream's progress.
    threads.emplace_back(
        [&]
        {
            while (!stop.load())
            {
                double start = 0.0;
                double end = 0.0;
                if (getPullBufferTimeRange(hash, &start, &end) !=
                    PlayerErrors::noError)
                    badRangeReads.fetch_add(1);
                rangeReads.fetch_add(1);
                (void)getPosition(voice);
                (void)getPause(voice);
                std::this_thread::sleep_for(std::chrono::milliseconds(1));
            }
        });

    std::this_thread::sleep_for(std::chrono::milliseconds(1500));
    stop.store(true);
    for (std::thread &t : threads)
        t.join();

    std::printf("  %d deliveries, %d seeks, %d resets, %d range reads\n",
                deliveries.load(), seeks.load(), resets.load(),
                rangeReads.load());
    EXPECT(deliveries.load() > 0,
           "%s: the stream should have asked for data and been fed", what);
    EXPECT(seeks.load() > 0 && resets.load() > 0 && rangeReads.load() > 0,
           "%s: every thread should have run", what);
    EXPECT(badDeliveries.load() == 0,
           "%s: %d deliveries failed", what, badDeliveries.load());
    EXPECT(badResets.load() == 0, "%s: %d resets failed", what,
           badResets.load());
    EXPECT(badRangeReads.load() == 0, "%s: %d range reads failed", what,
           badRangeReads.load());
    EXPECT(getIsValidVoiceHandle(voice) != 0,
           "%s: the looping voice should still be playing", what);

    disposeSound(hash);
    expectNothingLeft(what);
}

} // namespace

int main(int argc, char **argv)
{
    startHangGuard(120);

    const char *only = argc > 1 ? argv[1] : nullptr;
    const char *const kScenarios[] = {"seek", "rewind", "reset",
                                       "callbacks", "stress", "read-reset", "range"};
    if (only != nullptr)
    {
        bool known = false;
        for (const char *name : kScenarios)
            known = known || std::strcmp(only, name) == 0;
        if (!known)
        {
            std::fprintf(stderr,
                         "Unknown scenario '%s': use seek, rewind, reset, "
                         "callbacks, read-reset, range or stress.\n",
                         only);
            return 2;
        }
    }
    auto wanted = [&](const char *name)
    { return only == nullptr || std::strcmp(only, name) == 0; };

    // miniaudio opens its null backend on a machine with no audio hardware,
    // so a failure here means a broken environment, not merely a headless
    // one.
    if (!bringUpEngine())
    {
        tearDownEngine();
        std::fprintf(stderr,
                     "FATAL: no output device could be opened, not even a null "
                     "backend; the scenarios cannot run.\n"
                     "Set SOLOUD_LIFECYCLE_TEST_ALLOW_NO_DEVICE=1 to downgrade "
                     "this to a skip.\n");
        return std::getenv("SOLOUD_LIFECYCLE_TEST_ALLOW_NO_DEVICE") ? 0 : 1;
    }

    if (wanted("range"))
        rangeStaysStableDuringAnUnaccountedRead();
    if (wanted("seek"))
        seekKeepsADeliveryOut(10.0);
    if (wanted("rewind"))
        seekKeepsADeliveryOut(0.0);
    if (wanted("read-reset"))
        resetWaitsForTheMixerReading();
    if (wanted("reset"))
        resetWaitsForTheMixerDecoding();
    if (wanted("callbacks"))
        callbacksCanReenterTheStream();
    if (wanted("stress"))
    {
        concurrentUseComesThroughIntact(StreamKind::pcm);
        concurrentUseComesThroughIntact(StreamKind::mp3);
    }

    tearDownEngine();

    printSummary();
    return gFailures == 0 ? 0 : 1;
}
