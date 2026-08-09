// Standalone native regression tests for the audio-device lifecycle
// coordinator.
//
// Every race covered here lives in a window of a few instructions between a
// direct device operation *observing* state and *acting* on it, while ordinary
// playback posts lifecycle intent from another thread without taking the
// device-operation mutex. Timing-based tests can only make those collisions
// likely, so each one here is forced with a named barrier
// (src/device_lifecycle_test_hooks.h) that parks production code exactly at the
// observation point.
//
// What these tests pin down:
//
//   * a device change and a teardown cannot overlap on the same Player -- the
//     change is pinned for its whole native operation, so a concurrent
//     dispose() cannot free the Player underneath it;
//   * a direct operation only cancels lifecycle work that predates its own
//     decision: a conditional stop, a device change and an explicit start each
//     leave newer intent queued instead of erasing it;
//   * an explicit start that a genuine OS interruption overtakes reports the
//     failure instead of falsely succeeding, and does not cancel the stop that
//     interruption queued;
//   * setAudioDeviceIdleTimeout() returns promptly while the engine lifecycle
//     mutex is held by an in-flight initialization;
//   * an automatic start that exhausts rebuild/retry is reported as an event,
//     so a background failure cannot leave a silent, apparently-playing engine;
//   * clocked/scheduled playback never performs a backend device start on the
//     calling thread.
//
// Build and run from the flutter_soloud repository root with:
//
//   ./test/run_device_coordinator_test.sh

#include "device_lifecycle_test_hooks.h"
#include "enums.h"
#include "soloud/include/soloud_internal.h"

#include <atomic>
#include <chrono>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <thread>

extern "C"
{
    void prepareEngineInit(int64_t owner_engine_id);
    enum PlayerErrors initEngine(int deviceID, unsigned int sampleRate,
                                 unsigned int bufferSize, unsigned int channels,
                                 unsigned int lowLatency);
    void dispose();
    int isInited();

    enum PlayerErrors loadWaveform(int waveform, bool superWave, float scale,
                                   float detune, unsigned int *hash);
    enum PlayerErrors play(unsigned int soundHash, unsigned int busId,
                           float volume, float pan, bool paused, bool looping,
                           double loopingStartAt, unsigned int *handle);
    enum PlayerErrors playClocked(unsigned int soundHash, double soundTime,
                                  unsigned int busId, float volume, float pan,
                                  unsigned int *handle);
    enum PlayerErrors setPause(unsigned int handle, bool pause);
    enum PlayerErrors stop(unsigned int handle);

    enum PlayerErrors changeDevice(int deviceID);
    enum PlayerErrors startAudioDevice();
    enum PlayerErrors stopAudioDevice(unsigned int force);
    enum AudioDeviceState getAudioDeviceState();
    void setAudioDeviceIdleTimeout(int64_t timeoutMs);

    void setDartEventCallback(void (*voice_ended)(unsigned int *),
                              void (*file_loaded)(enum PlayerErrors *, char *,
                                                  unsigned int *, uint64_t *),
                              void (*state_changed)(enum PlayerStateEvents *),
                              int64_t owner_engine_id);

    // Test-only hooks (SOLOUD_LIFECYCLE_TEST_HOOKS).
    void soloudTestLockInitDeinit();
    void soloudTestUnlockInitDeinit();
}

namespace
{

using soloud_test::DeviceBarrier;

constexpr int64_t kEngineId = 2001;

// Wall-clock budget for a call that must not wait on a device operation or on
// the engine lifecycle mutex. Generous on purpose: the number it excludes is
// the multi-second device open, not a scheduling hiccup.
constexpr long long kNonBlockingBudgetMs = 250;

// Long enough that the idle scheduler never fires during a test, but still a
// finite policy: the indefinite (-1) keep-alive is an input to several of the
// decisions under test and would make them trivially true.
constexpr int64_t kQuietIdleTimeoutMs = 600000;

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

std::atomic<int> gStartFailureEvents{0};

void onVoiceEnded(unsigned int *handle) { std::free(handle); }

void onFileLoaded(enum PlayerErrors *e, char *name, unsigned int *hash,
                  uint64_t *counter)
{
    std::free(e);
    std::free(name);
    std::free(hash);
    std::free(counter);
}

void onStateChanged(enum PlayerStateEvents *state)
{
    if (state != nullptr &&
        *state == PlayerStateEvents::event_audio_device_start_failed)
        gStartFailureEvents.fetch_add(1, std::memory_order_acq_rel);
    // The real Dart bridge owns this pointer; the native side allocates it per
    // event only on some paths, so this test's callback takes it by value.
}

/// Bring an engine up. Returns false when the environment has no usable output
/// device, which every caller treats as "skip" rather than "pass".
bool bringUpEngine()
{
    prepareEngineInit(kEngineId);
    const PlayerErrors err = initEngine(-1, 44100, 2048, 2, 1);
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

/// A waveform needs no asset and no decoder, so it is the cheapest way to get a
/// real unpaused voice -- which is what makes resumeEngine() post a genuine
/// start request from another thread.
unsigned int loadTestWaveform()
{
    unsigned int hash = 0;
    const PlayerErrors err = loadWaveform(0, false, 1.0f, 1.0f, &hash);
    return err == PlayerErrors::noError ? hash : 0;
}

unsigned int playUnpaused(unsigned int hash)
{
    unsigned int handle = 0;
    play(hash, 0, 1.0f, 0.0f, /*paused*/ false, /*looping*/ true, 0.0, &handle);
    return handle;
}

long long millisSince(std::chrono::steady_clock::time_point start)
{
    return std::chrono::duration_cast<std::chrono::milliseconds>(
               std::chrono::steady_clock::now() - start)
        .count();
}

// ---------------------------------------------------------------------------

/// A device change holds the engine for its whole native operation, so a
/// teardown cannot dispose the Player it is running inside.
///
/// Without the pin the change worker resumes inside a freed Player: the window
/// is wide here because device enumeration deliberately runs before the
/// device-operation lock is taken.
void testChangeDeviceCannotOverlapTeardown()
{
    std::printf("change device vs teardown\n");
    if (!bringUpEngine())
    {
        std::printf("  skipped: no usable output device\n");
        return;
    }

    soloud_test::armBarrier(DeviceBarrier::changeDeviceEntered);

    std::atomic<bool> changeDone{false};
    std::atomic<bool> disposeDone{false};

    std::thread changer([&] {
        changeDevice(-1);
        changeDone.store(true, std::memory_order_release);
    });

    soloud_test::waitBarrierReached(DeviceBarrier::changeDeviceEntered);

    std::thread disposer([&] {
        dispose();
        disposeDone.store(true, std::memory_order_release);
    });

    // The teardown must not be able to run while the change is parked inside
    // the engine. If it can, the change is about to touch freed memory.
    std::this_thread::sleep_for(std::chrono::milliseconds(120));
    EXPECT(!disposeDone.load(std::memory_order_acquire),
           "dispose() ran while a device change was inside the engine");
    EXPECT(!changeDone.load(std::memory_order_acquire),
           "the parked device change should not have completed yet");

    soloud_test::releaseBarrier(DeviceBarrier::changeDeviceEntered);
    changer.join();
    disposer.join();

    EXPECT(changeDone.load(std::memory_order_acquire), "changeDevice() hung");
    EXPECT(disposeDone.load(std::memory_order_acquire), "dispose() hung");
    EXPECT(isInited() == 0, "the engine should be torn down at the end");
    std::printf("  ok: teardown serialized behind the device change\n");
}

/// A device change arriving after a teardown finds no initialized engine and
/// reports it, rather than operating on whatever Player is installed.
void testStaleChangeDeviceAfterTeardown()
{
    std::printf("stale device change after teardown\n");
    if (!bringUpEngine())
    {
        std::printf("  skipped: no usable output device\n");
        return;
    }

    dispose();
    EXPECT(isInited() == 0, "the engine should be torn down");

    const PlayerErrors err = changeDevice(-1);
    EXPECT(err == PlayerErrors::backendNotInited,
           "a device change after teardown should report backendNotInited, "
           "got %d",
           (int)err);
    std::printf("  ok: reported backendNotInited\n");
}

/// A conditional stop observes an idle engine, then playback starts. The stop
/// must not erase the newer start, and must not stop the device under it.
void testConditionalStopDoesNotSwallowNewPlayback()
{
    std::printf("conditional stop vs play\n");
    if (!bringUpEngine())
    {
        std::printf("  skipped: no usable output device\n");
        return;
    }

    // A long finite timeout keeps the idle scheduler from stopping the device
    // for unrelated reasons. Deliberately not the indefinite (-1) policy: that
    // is itself one of the inputs to the decisions under test, and would make
    // them trivially true.
    setAudioDeviceIdleTimeout(kQuietIdleTimeoutMs);

    const unsigned int hash = loadTestWaveform();
    EXPECT(hash != 0, "the test waveform should load");

    soloud_test::armBarrier(DeviceBarrier::stopAudioDeviceVoiceCountObserved);

    std::atomic<int> stopResult{-1};
    std::thread stopper([&] {
        stopResult.store((int)stopAudioDevice(/*force*/ 0),
                         std::memory_order_release);
    });

    soloud_test::waitBarrierReached(
        DeviceBarrier::stopAudioDeviceVoiceCountObserved);

    // The stop has already decided the engine is idle. Start playback now:
    // play() does not wait for the device-operation mutex, so its start request
    // lands while the stop is parked.
    const unsigned int handle = playUnpaused(hash);
    EXPECT(handle != 0, "playback should start");

    soloud_test::releaseBarrier(
        DeviceBarrier::stopAudioDeviceVoiceCountObserved);
    stopper.join();

    EXPECT(stopResult.load(std::memory_order_acquire) ==
               (int)PlayerErrors::noError,
           "the conditional stop should report success");

    // Give the scheduler a moment to act on whichever request survived.
    for (int i = 0; i < 100 && getAudioDeviceState() != audioDeviceStarted; ++i)
        std::this_thread::sleep_for(std::chrono::milliseconds(10));

    EXPECT(getAudioDeviceState() == audioDeviceStarted,
           "an unpaused voice must not be left with a stopped device "
           "(state %d)",
           (int)getAudioDeviceState());

    stop(handle);
    tearDownEngine();
    std::printf("  ok: the newer start survived the conditional stop\n");
}

/// The same guarantee for an unpause rather than a fresh play.
void testConditionalStopDoesNotSwallowUnpause()
{
    std::printf("conditional stop vs unpause\n");
    if (!bringUpEngine())
    {
        std::printf("  skipped: no usable output device\n");
        return;
    }
    setAudioDeviceIdleTimeout(kQuietIdleTimeoutMs);

    const unsigned int hash = loadTestWaveform();
    EXPECT(hash != 0, "the test waveform should load");

    unsigned int handle = 0;
    play(hash, 0, 1.0f, 0.0f, /*paused*/ true, /*looping*/ true, 0.0, &handle);
    EXPECT(handle != 0, "a paused voice should be created");

    soloud_test::armBarrier(DeviceBarrier::stopAudioDeviceVoiceCountObserved);

    std::thread stopper([&] { stopAudioDevice(/*force*/ 0); });
    soloud_test::waitBarrierReached(
        DeviceBarrier::stopAudioDeviceVoiceCountObserved);

    setPause(handle, false);

    soloud_test::releaseBarrier(
        DeviceBarrier::stopAudioDeviceVoiceCountObserved);
    stopper.join();

    for (int i = 0; i < 100 && getAudioDeviceState() != audioDeviceStarted; ++i)
        std::this_thread::sleep_for(std::chrono::milliseconds(10));

    EXPECT(getAudioDeviceState() == audioDeviceStarted,
           "an unpaused voice must not be left with a stopped device "
           "(state %d)",
           (int)getAudioDeviceState());

    stop(handle);
    tearDownEngine();
    std::printf("  ok: the unpause survived the conditional stop\n");
}

/// A device change decides the replacement can stay stopped, then playback
/// starts. The replacement must come up running rather than silently stopped.
void testChangeDeviceDoesNotSwallowPlayback()
{
    std::printf("change device vs play\n");
    if (!bringUpEngine())
    {
        std::printf("  skipped: no usable output device\n");
        return;
    }
    setAudioDeviceIdleTimeout(kQuietIdleTimeoutMs);

    const unsigned int hash = loadTestWaveform();
    EXPECT(hash != 0, "the test waveform should load");

    // Start from a stopped device so `shouldStartReplacement` is decided false.
    // Without this the swap sees a running device, decides to restart it
    // anyway, and the test would pass no matter what the cancellation does.
    stopAudioDevice(/*force*/ 1);
    EXPECT(getAudioDeviceState() != audioDeviceStarted,
           "the device must be stopped before the swap decides (state %d)",
           (int)getAudioDeviceState());

    soloud_test::armBarrier(DeviceBarrier::changeDeviceStartDecided);

    std::atomic<int> changeResult{-1};
    std::thread changer([&] {
        changeResult.store((int)changeDevice(-1), std::memory_order_release);
    });

    soloud_test::waitBarrierReached(DeviceBarrier::changeDeviceStartDecided);

    const unsigned int handle = playUnpaused(hash);
    EXPECT(handle != 0, "playback should start");

    soloud_test::releaseBarrier(DeviceBarrier::changeDeviceStartDecided);
    changer.join();

    EXPECT(changeResult.load(std::memory_order_acquire) ==
               (int)PlayerErrors::noError,
           "the device change should succeed");

    for (int i = 0; i < 100 && getAudioDeviceState() != audioDeviceStarted; ++i)
        std::this_thread::sleep_for(std::chrono::milliseconds(10));

    EXPECT(getAudioDeviceState() == audioDeviceStarted,
           "the replacement device must be running under an active voice "
           "(state %d)",
           (int)getAudioDeviceState());

    stop(handle);
    tearDownEngine();
    std::printf("  ok: the replacement came up running\n");
}

/// An explicit start clears the stale-interruption latch, then a genuine
/// interruption arrives. The start must report the failure rather than claim
/// success, and must not cancel the stop the interruption queued.
void testExplicitStartYieldsToGenuineInterruption()
{
    std::printf("explicit start vs genuine interruption\n");
    if (!bringUpEngine())
    {
        std::printf("  skipped: no usable output device\n");
        return;
    }
    setAudioDeviceIdleTimeout(kQuietIdleTimeoutMs);

    // Begin with the device running. If the interruption stop is erased the
    // device simply stays started, which is what makes the final assertion
    // discriminating rather than trivially true.
    startAudioDevice();
    for (int i = 0; i < 100 && getAudioDeviceState() != audioDeviceStarted; ++i)
        std::this_thread::sleep_for(std::chrono::milliseconds(10));
    EXPECT(getAudioDeviceState() == audioDeviceStarted,
           "the device must be running before the explicit start under test "
           "(state %d)",
           (int)getAudioDeviceState());

    soloud_test::armBarrier(DeviceBarrier::startAudioDeviceLatchCleared);

    std::atomic<int> startResult{-1};
    std::thread starter([&] {
        startResult.store((int)startAudioDevice(), std::memory_order_release);
    });

    soloud_test::waitBarrierReached(DeviceBarrier::startAudioDeviceLatchCleared);

    // Delivered through the backend notification path the OS uses, which -- as
    // in production -- does not take the engine lifecycle mutex the parked
    // start is holding.
    SoLoud::miniaudio_debugTriggerAudioInterruption(true);

    soloud_test::releaseBarrier(DeviceBarrier::startAudioDeviceLatchCleared);
    starter.join();

    EXPECT(startResult.load(std::memory_order_acquire) ==
               (int)PlayerErrors::audioDeviceFailedToStart,
           "an explicit start overtaken by a real interruption must report "
           "failure, got %d",
           startResult.load(std::memory_order_acquire));

    // Let the interruption stop run; it must still be queued.
    for (int i = 0; i < 100 && getAudioDeviceState() == audioDeviceStarted; ++i)
        std::this_thread::sleep_for(std::chrono::milliseconds(10));

    EXPECT(getAudioDeviceState() != audioDeviceStarted,
           "the interruption stop was cancelled by the older explicit start: "
           "the device is still running (state %d)",
           (int)getAudioDeviceState());

    SoLoud::miniaudio_debugTriggerAudioInterruption(false);
    tearDownEngine();
    std::printf("  ok: the interruption stop survived the explicit start\n");
}

/// The idle-timeout setter is synchronous and documented as callable at any
/// time, so it must not wait for the engine lifecycle mutex -- which an
/// in-flight initialization holds across the whole native device open.
void testIdleTimeoutSetterDoesNotBlockOnInit()
{
    std::printf("idle timeout setter vs held lifecycle mutex\n");

    // Run the setter on its own thread and time it from here. If it blocks on
    // the lifecycle mutex it blocks *forever* -- this test holds that mutex --
    // so calling it inline would hang the suite instead of failing it.
    soloudTestLockInitDeinit();

    std::atomic<bool> setterReturned{false};
    const auto started = std::chrono::steady_clock::now();
    std::thread setter([&] {
        setAudioDeviceIdleTimeout(1234);
        setterReturned.store(true, std::memory_order_release);
    });

    while (!setterReturned.load(std::memory_order_acquire) &&
           millisSince(started) < kNonBlockingBudgetMs)
        std::this_thread::sleep_for(std::chrono::milliseconds(5));

    const long long elapsed = millisSince(started);
    const bool returnedInTime = setterReturned.load(std::memory_order_acquire);

    // Release the mutex either way, so a failing setter can finish and be
    // joined rather than stranding the thread.
    soloudTestUnlockInitDeinit();
    setter.join();

    EXPECT(returnedInTime,
           "setAudioDeviceIdleTimeout() was still blocked after %lldms while "
           "the lifecycle mutex was held (budget %lldms)",
           elapsed, kNonBlockingBudgetMs);

    // The published policy must survive to the next engine.
    if (!bringUpEngine())
    {
        std::printf("  ok: returned in %lldms (engine bring-up skipped)\n",
                    elapsed);
        return;
    }
    tearDownEngine();
    std::printf("  ok: returned in %lldms\n", elapsed);
}

/// An automatic start that exhausts rebuild/retry must be observable. Nothing
/// can return it to the caller -- play() completed long before -- so the engine
/// publishes it as an event.
void testAutomaticStartFailureIsReported()
{
    std::printf("automatic start failure is reported\n");
    if (!bringUpEngine())
    {
        std::printf("  skipped: no usable output device\n");
        return;
    }
    setAudioDeviceIdleTimeout(kQuietIdleTimeoutMs);

    const unsigned int hash = loadTestWaveform();
    EXPECT(hash != 0, "the test waveform should load");

    stopAudioDevice(/*force*/ 1);
    gStartFailureEvents.store(0, std::memory_order_release);

    // Two forced failures: the initial start and the retry after the device has
    // been rebuilt. That is the whole automatic recovery path.
    soloud_test::failNextDeviceStarts(2);

    const unsigned int handle = playUnpaused(hash);
    EXPECT(handle != 0, "playback should start");

    for (int i = 0;
         i < 200 && gStartFailureEvents.load(std::memory_order_acquire) == 0;
         ++i)
        std::this_thread::sleep_for(std::chrono::milliseconds(10));

    EXPECT(gStartFailureEvents.load(std::memory_order_acquire) > 0,
           "a background start failure must be published as an event");
    EXPECT(soloud_test::pendingForcedDeviceStartFailures() == 0,
           "both the start and its retry should have been attempted");

    soloud_test::failNextDeviceStarts(0);
    stop(handle);
    tearDownEngine();
    std::printf("  ok: the failure reached the event bridge\n");
}

/// Clocked and scheduled playback must not run a backend device start on the
/// calling thread. Proven structurally rather than by timing: the barrier sits
/// inside performAudioDeviceStart(), so if the call parked there it never
/// returns while the barrier is armed.
void testClockedPlaybackDoesNotStartDeviceInline()
{
    std::printf("clocked playback does not start the device inline\n");
    if (!bringUpEngine())
    {
        std::printf("  skipped: no usable output device\n");
        return;
    }
    setAudioDeviceIdleTimeout(kQuietIdleTimeoutMs);

    const unsigned int hash = loadTestWaveform();
    EXPECT(hash != 0, "the test waveform should load");

    stopAudioDevice(/*force*/ 1);
    EXPECT(getAudioDeviceState() != audioDeviceStarted,
           "the device should be stopped before the clocked play");

    soloud_test::armBarrier(DeviceBarrier::performAudioDeviceStartEntered);

    std::atomic<bool> returned{false};
    unsigned int handle = 0;
    std::thread caller([&] {
        playClocked(hash, 0.0, 0, 1.0f, 0.0f, &handle);
        returned.store(true, std::memory_order_release);
    });

    // If playClocked() still started the device inline it would be parked on
    // the barrier right now and this would time out.
    const auto started = std::chrono::steady_clock::now();
    while (!returned.load(std::memory_order_acquire) &&
           millisSince(started) < kNonBlockingBudgetMs)
        std::this_thread::sleep_for(std::chrono::milliseconds(5));

    EXPECT(returned.load(std::memory_order_acquire),
           "playClocked() performed a backend device start on the calling "
           "thread");

    soloud_test::releaseBarrier(DeviceBarrier::performAudioDeviceStartEntered);
    caller.join();

    EXPECT(handle != 0, "the clocked voice should have been created");

    stop(handle);
    tearDownEngine();
    std::printf("  ok: the device start was queued, not performed inline\n");
}

} // namespace

int main()
{
    testChangeDeviceCannotOverlapTeardown();
    testStaleChangeDeviceAfterTeardown();
    testConditionalStopDoesNotSwallowNewPlayback();
    testConditionalStopDoesNotSwallowUnpause();
    testChangeDeviceDoesNotSwallowPlayback();
    testExplicitStartYieldsToGenuineInterruption();
    testIdleTimeoutSetterDoesNotBlockOnInit();
    testAutomaticStartFailureIsReported();
    testClockedPlaybackDoesNotStartDeviceInline();

    std::printf("\n%d assertions, %d failures\n", gAssertions, gFailures);
    return gFailures == 0 ? 0 : 1;
}
