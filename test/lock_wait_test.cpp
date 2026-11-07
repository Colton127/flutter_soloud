// Standalone native tests for waits on the locks that the mixer, the
// voice-ended dispatch and an OS interruption's handler share with the
// threads that control playback.
//
// The mixer runs on the audio thread and must not wait for a lock that a
// control thread can hold for long: while it waits, every voice stops
// advancing, and on the web's AudioWorklet thread a wait on a contended lock
// aborts the program. What these tests pin down:
//
//   * after a block in which a voice stopped or paused, the mixer asks for a
//     device-idle evaluation. That takes Player::mInterruptionMutex, which an
//     interruption's handler and every lifecycle post hold. The mixer only
//     tries it: while an interruption's handler is parked holding it
//     (audioInterruptionLocked), the mixer's clock keeps advancing, and once
//     it is released the evaluation the mixer could not make still happens.
//   * a voice the mixer ends is reported on the mixer's thread, and the
//     report takes the voice's handle out of its sound's handle list, which
//     Player::sounds_mutex guards. While a play is parked holding that
//     (playSoundLookedUp), the mixer's clock keeps advancing and the end is
//     reported; the handle is gone from the list once the play is released.
//   * an interruption's end is handled by counting the active voices, which
//     waits for SoLoud's audio mutex. The handler must not hold
//     Player::mInterruptionMutex meanwhile, since every lifecycle request
//     takes it: while the mixer is parked holding the audio mutex inside a
//     stream's seek (pullSeekDiscarding) and the handler waits to count, an
//     unpause, which asks for the device to start, still returns. And since
//     the count is made without the lock, a voice that stops after it and
//     asks for an idle stop before the handler's start (which supersedes
//     it) must not leave the device running with nothing to play.
//
// Build and run from the flutter_soloud repository root with:
//
//   ./test/run_lock_wait_test.sh            (plain)
//   ./test/run_lock_wait_test.sh --asan     (AddressSanitizer)
//   ./test/run_lock_wait_test.sh --tsan     (ThreadSanitizer)
//
// A scenario name after the flag (idle, ended, interruption or recount) runs
// only that scenario.

#include "bindings.h"
#include "device_lifecycle_test_hooks.h"
#include "enums.h"

#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <functional>
#include <memory>
#include <mutex>
#include <thread>
#include <vector>

extern "C"
{
    // Test-only exports, compiled in with SOLOUD_LIFECYCLE_TEST_HOOKS.
    int soloudTestSoundCount();
    int soloudTestSoundHandleCount(unsigned int hash);
}

namespace
{

using soloud_test::DeviceBarrier;

constexpr int64_t kEngineId = 4003;

// How long a lock is held while the test watches the mixer. Without the fix
// the mixer does not advance at all in that time.
constexpr long long kParkMs = 300;

// Long enough that the idle policy never stops the device unless a scenario
// shortens it.
constexpr int64_t kQuietIdleTimeoutMs = 600000;
// Short enough that a scenario sees the device go idle in a bounded wait.
constexpr int64_t kShortIdleTimeoutMs = 200;

// Bound on waits for work that needs no lock the test holds back. Generous,
// since passing runs take milliseconds; only a deadlocked run waits this long.
constexpr long long kProgressMs = 5000;

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

/// End the run now: a thread is parked or wedged where waiting for it cannot
/// end, and static destruction must not run under it.
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

/// Wait, at most [ms], for the output device to be in [state].
bool waitForDeviceState(AudioDeviceState state, long long ms)
{
    const auto deadline =
        std::chrono::steady_clock::now() + std::chrono::milliseconds(ms);
    while (getAudioDeviceState() != state)
    {
        if (std::chrono::steady_clock::now() >= deadline)
            return false;
        std::this_thread::sleep_for(std::chrono::milliseconds(5));
    }
    return true;
}

/// How far the mixer's clock moves in [ms]. It advances at the start of every
/// block, so it stands still while the mixer is stuck between blocks.
double engineTimeAdvanceOver(long long ms)
{
    const double before = getEngineTime();
    std::this_thread::sleep_for(std::chrono::milliseconds(ms));
    return getEngineTime() - before;
}

// ---------------------------------------------------------------------------
// Engine
// ---------------------------------------------------------------------------

std::mutex gEndedMutex;
std::condition_variable gEndedCv;
std::vector<unsigned int> gEndedVoices;

void onVoiceEnded(unsigned int *handle)
{
    {
        std::lock_guard<std::mutex> lock(gEndedMutex);
        gEndedVoices.push_back(*handle);
    }
    gEndedCv.notify_all();
    std::free(handle);
}

/// Wait, at most [ms], for the end of [voice] to be reported to Dart.
bool waitForVoiceEnded(unsigned int voice, long long ms)
{
    std::unique_lock<std::mutex> lock(gEndedMutex);
    return gEndedCv.wait_for(
        lock, std::chrono::milliseconds(ms),
        [&]
        {
            for (const unsigned int ended : gEndedVoices)
                if (ended == voice)
                    return true;
            return false;
        });
}

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
    const PlayerErrors err = initEngine(-1, 44100, 2048, 2, 1, 0, 0);
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

/// A waveform needs no asset and no decoder: the cheapest real voice.
unsigned int loadTestWaveform()
{
    unsigned int hash = 0;
    const PlayerErrors err = loadWaveform(0, false, 1.0f, 1.0f, &hash);
    return err == PlayerErrors::noError ? hash : 0;
}

unsigned int playQuietly(unsigned int hash)
{
    unsigned int voice = 0;
    const PlayerErrors err = play(hash, 0, 0.05f, 0.0f, /*paused*/ false,
                                  /*looping*/ false, 0.0, 0.0, -1, -1, 1.0f,
                                  &voice);
    return err == PlayerErrors::noError ? voice : 0;
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
// Scenarios
// ---------------------------------------------------------------------------

/// After a block in which a voice stopped or paused, the mixer asks for a
/// device-idle evaluation (SoLoud's voice-inactive callback). That takes
/// mInterruptionMutex, which an OS interruption's handler holds while it
/// handles the interruption and a lifecycle post holds while it queues its
/// request. The mixer must not wait for it. It tries the lock, and when
/// another thread holds it, asks again after its next block, so the
/// evaluation is not lost.
///
/// An interruption's handler parks holding the lock (audioInterruptionLocked),
/// and the only voice is then paused by the mixer itself (schedulePause()), so
/// the mixer asks. Its clock must keep advancing while the handler is parked.
/// Once the handler is released, the evaluation the mixer could not make must
/// still happen: with a short idle timeout, the device goes idle. Nothing else
/// asks for one here.
void mixerDoesNotWaitForTheLifecycleLock()
{
    const char *what = "the mixer's idle request vs a held lifecycle lock";
    std::printf("%s\n", what);

    setAudioDeviceIdleTimeout(kShortIdleTimeoutMs);
    const unsigned int hash = loadTestWaveform();
    const unsigned int voice = hash != 0 ? playQuietly(hash) : 0;
    EXPECT(hash != 0 && voice != 0, "%s: a waveform should load and play",
           what);
    if (voice == 0)
    {
        setAudioDeviceIdleTimeout(kQuietIdleTimeoutMs);
        if (hash != 0)
            disposeSound(hash);
        return;
    }
    // Playing restarts a device the short timeout may have stopped already.
    if (!waitForDeviceState(audioDeviceStarted, kProgressMs))
        endRun("the device never started for the voice");

    soloud_test::armBarrier(DeviceBarrier::audioInterruptionLocked);
    BackgroundCall interruption([] { debugTriggerAudioInterruption(0); });
    if (!waitBarrierReachedFor(DeviceBarrier::audioInterruptionLocked,
                               kProgressMs))
        endRun("the interruption never reached its handler");

    // Paused by the mixer at its next block, which then asks for an idle
    // evaluation.
    schedulePause(voice, 0.0f);
    std::this_thread::sleep_for(std::chrono::milliseconds(100));
    const double advanced = engineTimeAdvanceOver(kParkMs);
    EXPECT(advanced >= kParkMs / 3000.0,
           "%s: the mixer stopped mixing while an interruption held the "
           "lifecycle lock (its clock moved %.3f s in %.3f s)",
           what, advanced, kParkMs / 1000.0);

    soloud_test::releaseBarrier(DeviceBarrier::audioInterruptionLocked);
    interruption.join();

    EXPECT(waitForDeviceState(audioDeviceStopped, kProgressMs),
           "%s: the device should go idle once the lock is free (state %d): "
           "the idle request the mixer could not make was lost",
           what, static_cast<int>(getAudioDeviceState()));

    setAudioDeviceIdleTimeout(kQuietIdleTimeoutMs);
    disposeSound(hash);
    expectNothingLeft(what);
    EXPECT(startAudioDevice() == PlayerErrors::noError &&
               waitForDeviceState(audioDeviceStarted, kProgressMs),
           "%s: the device should start again", what);
}

/// SoLoud reports a voice ended on the thread that ended it, after it has
/// released its audio mutex: for a voice the mixer ends, on the mixer's
/// thread. The report takes the voice's handle out of its sound's handle
/// list, which sounds_mutex guards, and a play, a disposal or a stream's
/// delivery holds that for as long as it takes. The mixer must not wait for
/// it: the removal is left to the next thread that takes the mutex, which
/// applies it before anything else.
///
/// A play parks holding sounds_mutex (playSoundLookedUp), and the mixer ends
/// another voice of the same sound (scheduleStop()). The mixer's clock must
/// keep advancing while the play is parked, and the end must be reported.
/// Once the play is released, the sound must have the new voice's handle and
/// not the ended one's.
void mixerDoesNotWaitForTheSoundsLock()
{
    const char *what = "the mixer's voice-ended report vs a held sounds lock";
    std::printf("%s\n", what);

    const unsigned int hash = loadTestWaveform();
    const unsigned int ending = hash != 0 ? playQuietly(hash) : 0;
    EXPECT(hash != 0 && ending != 0, "%s: a waveform should load and play",
           what);
    if (ending == 0)
    {
        if (hash != 0)
            disposeSound(hash);
        return;
    }

    soloud_test::armBarrier(DeviceBarrier::playSoundLookedUp);
    unsigned int played = 0;
    BackgroundCall play([&] { played = playQuietly(hash); });
    if (!waitBarrierReachedFor(DeviceBarrier::playSoundLookedUp, kProgressMs))
        endRun("the play never reached its lookup");

    // Ended by the mixer once 50 ms of it have played, and reported there.
    scheduleStop(ending, 0.05f);
    std::this_thread::sleep_for(std::chrono::milliseconds(100));
    const double advanced = engineTimeAdvanceOver(kParkMs);
    EXPECT(advanced >= kParkMs / 3000.0,
           "%s: the mixer stopped mixing while a play held the sounds lock "
           "(its clock moved %.3f s in %.3f s)",
           what, advanced, kParkMs / 1000.0);
    EXPECT(waitForVoiceEnded(ending, kParkMs),
           "%s: the voice's end should be reported while the play holds the "
           "sounds lock",
           what);

    soloud_test::releaseBarrier(DeviceBarrier::playSoundLookedUp);
    play.join();

    EXPECT(played != 0, "%s: the parked play should succeed", what);
    EXPECT(waitForVoiceEnded(ending, kProgressMs),
           "%s: the voice's end should be reported", what);
    const int handles = soloudTestSoundHandleCount(hash);
    EXPECT(handles == 1,
           "%s: the sound should have only the new voice's handle, not the "
           "ended one's (it has %d)",
           what, handles);

    disposeSound(hash);
    expectNothingLeft(what);
}

/// When an OS interruption ends and the idle timeout is finite, the handler
/// restarts the device only if voices are active. Counting them waits for
/// SoLoud's audio mutex: for the mixer's block, for a stream's seek inside
/// it, or with render-ahead for a re-mix. The handler must not hold
/// mInterruptionMutex while it waits, since every lifecycle request takes it:
/// an unpause or a play on the UI thread asks for the device to start.
///
/// The mixer parks holding the audio mutex, inside a pull stream's seek out
/// of its buffer (pullSeekDiscarding). The interruption's end is then
/// handled up to its count (audioInterruptionCounting), and released into
/// it: it now waits for the parked mixer. An unpause of another voice must
/// still return.
void unpauseDoesNotWaitForAnInterruptionsCount()
{
    const char *what = "an unpause vs an interruption's voice count";
    std::printf("%s\n", what);

    unsigned int stream = 0;
    const PlayerErrors created =
        setPullBufferStream(&stream, /*maxBufferSize*/ 1u << 20,
                            /*bufferTriggerPosition*/ 0.5, 44100, 2,
                            static_cast<int>(PCM_F32LE),
                            /*audioSizeBytes*/ 64u << 20, nullptr, nullptr,
                            nullptr, nullptr);
    const unsigned int hash = loadTestWaveform();
    const unsigned int streamVoice =
        created == PlayerErrors::noError && stream != 0 ? playQuietly(stream)
                                                        : 0;
    unsigned int paused = 0;
    const PlayerErrors playedPaused =
        hash != 0 ? play(hash, 0, 0.05f, 0.0f, /*paused*/ true,
                         /*looping*/ false, 0.0, 0.0, -1, -1, 1.0f, &paused)
                  : PlayerErrors::unknownError;
    EXPECT(streamVoice != 0 && playedPaused == PlayerErrors::noError &&
               paused != 0,
           "%s: a pull stream and a paused waveform should play", what);
    if (streamVoice == 0 || paused == 0)
    {
        if (stream != 0)
            disposeSound(stream);
        if (hash != 0)
            disposeSound(hash);
        return;
    }

    // The mixer parks inside the stream's seek, holding the audio mutex. The
    // seek call gets its own thread: with no mixer running it would apply
    // the seek itself, and park.
    soloud_test::armBarrier(DeviceBarrier::pullSeekDiscarding);
    BackgroundCall seeker([&] { seek(streamVoice, 10.0f); });
    if (!waitBarrierReachedFor(DeviceBarrier::pullSeekDiscarding, kProgressMs))
        endRun("the stream's seek never got to discarding its buffer");

    soloud_test::armBarrier(DeviceBarrier::audioInterruptionCounting);
    BackgroundCall interruption([] { debugTriggerAudioInterruption(0); });
    if (!waitBarrierReachedFor(DeviceBarrier::audioInterruptionCounting,
                               kProgressMs))
        endRun("the interruption's end never got to counting the voices");
    soloud_test::releaseBarrier(DeviceBarrier::audioInterruptionCounting);
    EXPECT(!interruption.finishedWithin(kParkMs / 3),
           "%s: the count should wait for the parked mixer", what);

    BackgroundCall unpause([&] { setPause(paused, false); });
    EXPECT(unpause.finishedWithin(kParkMs),
           "%s: the unpause waited for the interruption's handler, which "
           "held the lifecycle lock while it waited for the mixer",
           what);

    soloud_test::releaseBarrier(DeviceBarrier::pullSeekDiscarding);
    EXPECT(interruption.finishedWithin(kProgressMs) &&
               unpause.finishedWithin(kProgressMs) &&
               seeker.finishedWithin(kProgressMs),
           "%s: everything should finish once the mixer is released", what);
    interruption.join();
    unpause.join();
    seeker.join();

    EXPECT(waitForDeviceState(audioDeviceStarted, kProgressMs),
           "%s: the device should be running (state %d)", what,
           static_cast<int>(getAudioDeviceState()));

    disposeSound(stream);
    disposeSound(hash);
    expectNothingLeft(what);
}

/// The handler counts the active voices without mInterruptionMutex, then
/// takes it again to ask for the device to start. A voice may stop in
/// between and ask for an idle stop, which that start then supersedes (a
/// start supersedes all idle work). The handler must ask for an idle
/// evaluation again after its start, or the device keeps running with
/// nothing to play.
///
/// With a short idle timeout, the handler parks after counting the only
/// voice (audioInterruptionCounted), and the mixer ends that voice and asks
/// for an idle evaluation. Once the handler is released, the device must go
/// idle.
void deviceGoesIdleWhenAVoiceStopsAfterTheCount()
{
    const char *what = "a voice stopping after an interruption's count";
    std::printf("%s\n", what);

    setAudioDeviceIdleTimeout(kShortIdleTimeoutMs);
    const unsigned int hash = loadTestWaveform();
    const unsigned int voice = hash != 0 ? playQuietly(hash) : 0;
    EXPECT(hash != 0 && voice != 0, "%s: a waveform should load and play",
           what);
    if (voice == 0)
    {
        setAudioDeviceIdleTimeout(kQuietIdleTimeoutMs);
        if (hash != 0)
            disposeSound(hash);
        return;
    }
    if (!waitForDeviceState(audioDeviceStarted, kProgressMs))
        endRun("the device never started for the voice");

    soloud_test::armBarrier(DeviceBarrier::audioInterruptionCounted);
    BackgroundCall interruption([] { debugTriggerAudioInterruption(0); });
    if (!waitBarrierReachedFor(DeviceBarrier::audioInterruptionCounted,
                               kProgressMs))
        endRun("the interruption's end never counted the voice");

    // Ended by the mixer, which then asks for an idle evaluation, right after
    // it reports the end on the same thread.
    scheduleStop(voice, 0.02f);
    EXPECT(waitForVoiceEnded(voice, kProgressMs),
           "%s: the voice's end should be reported", what);
    std::this_thread::sleep_for(std::chrono::milliseconds(100));

    soloud_test::releaseBarrier(DeviceBarrier::audioInterruptionCounted);
    interruption.join();

    EXPECT(waitForDeviceState(audioDeviceStopped, kProgressMs),
           "%s: the device should go idle (state %d): the handler's start "
           "superseded the idle stop the ended voice asked for",
           what, static_cast<int>(getAudioDeviceState()));

    setAudioDeviceIdleTimeout(kQuietIdleTimeoutMs);
    disposeSound(hash);
    expectNothingLeft(what);
    EXPECT(startAudioDevice() == PlayerErrors::noError &&
               waitForDeviceState(audioDeviceStarted, kProgressMs),
           "%s: the device should start again", what);
}

} // namespace

int main(int argc, char **argv)
{
    startHangGuard(120);

    const char *only = argc > 1 ? argv[1] : nullptr;
    const char *const kScenarios[] = {"idle", "ended", "interruption",
                                      "recount"};
    if (only != nullptr)
    {
        bool known = false;
        for (const char *name : kScenarios)
            known = known || std::strcmp(only, name) == 0;
        if (!known)
        {
            std::fprintf(stderr,
                         "Unknown scenario '%s': use idle, ended, "
                         "interruption or recount.\n",
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

    if (wanted("idle"))
        mixerDoesNotWaitForTheLifecycleLock();
    if (wanted("ended"))
        mixerDoesNotWaitForTheSoundsLock();
    if (wanted("interruption"))
        unpauseDoesNotWaitForAnInterruptionsCount();
    if (wanted("recount"))
        deviceGoesIdleWhenAVoiceStopsAfterTheCount();

    tearDownEngine();

    printSummary();
    return gFailures == 0 ? 0 : 1;
}
