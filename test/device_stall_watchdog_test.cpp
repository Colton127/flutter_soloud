// Standalone native regression tests for the output-device callback stall
// watchdog.
//
// The failure it reports: the OS stops calling the output device's data
// callback while the device keeps reporting `ma_device_state_started`. Nothing
// is mixed, voices stay valid and unpaused but never advance, and without the
// watchdog nothing tells the app. The test hook
// soloud_test::setDeviceCallbackStalled() reproduces exactly that on a real
// device: the device keeps running and reporting started, while its callback
// returns silence without reaching the engine.
//
// What these tests pin down:
//
//   * a stall is reported within the threshold plus a sample interval, not
//     before the threshold, and exactly once however long it lasts;
//   * the report re-arms once callbacks resume, and once the device has been
//     stopped and restarted;
//   * the device callback is what is watched, not the mixer: with the
//     render-ahead ring the mixer also runs on API threads (retroactive
//     re-mix), and that must not hide a dead device;
//   * nothing is reported during normal playback, while the device is
//     stopped, after an idle stop, during an OS interruption, or once the
//     engine is gone;
//   * a watchdog frozen by a process suspension does not report on resume
//     before the callback had a chance to run, and still needs a full
//     threshold of fresh observation to report a stall that outlives it;
//   * teardown joins the watchdog promptly, even mid-stall.
//
// The threshold is injected (soloud_test::setDeviceStallThresholdMs()) so the
// whole run takes seconds rather than minutes.
//
// Build and run from the flutter_soloud repository root with:
//
//   ./test/run_device_stall_watchdog_test.sh

#include "bindings.h"
#include "device_lifecycle_test_hooks.h"
#include "enums.h"
#include "soloud/include/soloud_internal.h"

#include <atomic>
#include <chrono>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <thread>

namespace
{

using soloud_test::DeviceBarrier;
using Clock = std::chrono::steady_clock;

constexpr int64_t kEngineId = 3001;

// Injected stall threshold. The watchdog samples four times per threshold.
constexpr long long kThresholdMs = 400;
constexpr long long kSampleIntervalMs = kThresholdMs / 4;

// Slack on top of threshold + one sample interval before a missing report
// counts as a failure. Generous on purpose, for ThreadSanitizer builds and
// loaded CI machines: what it excludes is "never reported".
constexpr long long kDetectionMarginMs = 1500;

// How early a report may arrive relative to the threshold. The stall starts
// when the hook is set, and a callback already in flight at that instant can
// still count once, so allow a callback period or so.
constexpr long long kEarlyToleranceMs = 60;

// Long enough that the idle scheduler never stops the device during a test.
constexpr int64_t kQuietIdleTimeoutMs = 600000;

// A dispose() mid-stall must join the watchdog promptly. The number excluded
// is a watchdog stuck behind a lock, not an ordinary device stop.
constexpr long long kTeardownBudgetMs = 3000;

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

const Clock::time_point gEpoch = Clock::now();

long long nowMs()
{
    return std::chrono::duration_cast<std::chrono::milliseconds>(
               Clock::now() - gEpoch)
        .count();
}

void sleepMs(long long ms)
{
    std::this_thread::sleep_for(std::chrono::milliseconds(ms));
}

std::atomic<int> gStallEvents{0};
std::atomic<long long> gLastStallAtMs{-1};

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
    if (state == nullptr)
        return;
    if (*state == PlayerStateEvents::event_audio_device_stalled)
    {
        gLastStallAtMs.store(nowMs(), std::memory_order_release);
        gStallEvents.fetch_add(1, std::memory_order_acq_rel);
    }
    // Ownership is handed over exactly as it is to Dart.
    std::free(state);
}

void resetStallEvents()
{
    gStallEvents.store(0, std::memory_order_release);
    gLastStallAtMs.store(-1, std::memory_order_release);
}

int stallEvents() { return gStallEvents.load(std::memory_order_acquire); }

/// Wait for at least [count] stall reports, up to [timeoutMs].
bool waitForStallEvents(int count, long long timeoutMs)
{
    const long long deadline = nowMs() + timeoutMs;
    while (stallEvents() < count && nowMs() < deadline)
        sleepMs(5);
    return stallEvents() >= count;
}

/// The device data callback heartbeat the watchdog reads, or 0 while the
/// backend is busy with a device operation.
unsigned int callbackHeartbeat()
{
    unsigned int state = 0, count = 0, generation = 0;
    if (!SoLoud::miniaudio_sampleDataCallbackLiveness(&state, &count,
                                                       &generation))
        return 0;
    return count;
}

/// Wait until the device reports started and its callback demonstrably runs.
bool waitForRunningCallbacks(long long timeoutMs = 3000)
{
    const long long deadline = nowMs() + timeoutMs;
    while (nowMs() < deadline)
    {
        if (getAudioDeviceState() == audioDeviceStarted)
        {
            const unsigned int before = callbackHeartbeat();
            sleepMs(50);
            const unsigned int after = callbackHeartbeat();
            if (before != 0 && after != 0 && after != before)
                return true;
        }
        else
        {
            sleepMs(10);
        }
    }
    return false;
}

bool waitForDeviceState(AudioDeviceState expected, long long timeoutMs)
{
    const long long deadline = nowMs() + timeoutMs;
    while (getAudioDeviceState() != expected && nowMs() < deadline)
        sleepMs(5);
    return getAudioDeviceState() == expected;
}

/// Bring an engine up. Returns false when the environment has no usable output
/// device, which every caller treats as "skip" rather than "pass".
bool bringUpEngine(unsigned int renderAheadFrames = 0)
{
    soloud_test::setDeviceCallbackStalled(false);
    setAudioDeviceIdleTimeout(kQuietIdleTimeoutMs);
    prepareEngineInit(kEngineId);
    const PlayerErrors err =
        initEngine(-1, 44100, 2048, 2, 1, renderAheadFrames != 0 ? 512 : 0,
                   renderAheadFrames);
    if (err != PlayerErrors::noError)
        return false;
    setDartEventCallback(onVoiceEnded, onFileLoaded, onStateChanged, kEngineId);
    resetStallEvents();
    return isInited() != 0;
}

void tearDownEngine()
{
    soloud_test::setDeviceCallbackStalled(false);
    if (isInited())
        dispose();
    setAudioDeviceIdleTimeout(kQuietIdleTimeoutMs);
}

/// A waveform needs no asset and no decoder: the cheapest real voice.
unsigned int loadTestWaveform()
{
    unsigned int hash = 0;
    const PlayerErrors err = loadWaveform(0, false, 1.0f, 1.0f, &hash);
    return err == PlayerErrors::noError ? hash : 0;
}

unsigned int playLooping(unsigned int hash)
{
    unsigned int handle = 0;
    play(hash, 0, 0.05f, 0.0f, /*paused*/ false, /*looping*/ true, 0.0, 0.0,
         -1, -1, 1.0f, &handle);
    return handle;
}

/// Bring up an engine with one looping voice and a device whose callback is
/// demonstrably running. Returns 0, with the failure recorded, when it cannot.
unsigned int bringUpPlaying(unsigned int renderAheadFrames = 0)
{
    if (!bringUpEngine(renderAheadFrames))
    {
        // main() already proved an engine can initialize here.
        EXPECT(false, "the engine should initialize");
        tearDownEngine();
        return 0;
    }
    const unsigned int hash = loadTestWaveform();
    EXPECT(hash != 0, "the test waveform should load");
    const unsigned int handle = hash != 0 ? playLooping(hash) : 0;
    EXPECT(handle != 0, "playback should start");
    const bool running = waitForRunningCallbacks();
    EXPECT(running, "the device callback should be running before the test");
    if (handle == 0 || !running)
    {
        tearDownEngine();
        return 0;
    }
    // Let the watchdog take a baseline of the healthy device.
    sleepMs(2 * kSampleIntervalMs);
    resetStallEvents();
    return handle;
}

// ---------------------------------------------------------------------------

/// A healthy device is never reported.
void testNoReportDuringNormalPlayback()
{
    std::printf("no report during normal playback\n");
    const unsigned int handle = bringUpPlaying();
    if (handle == 0)
        return;

    const double before = getPosition(handle);
    sleepMs(5 * kThresholdMs);
    EXPECT(stallEvents() == 0,
           "a healthy device was reported as stalled (%d reports)",
           stallEvents());
    EXPECT(getPosition(handle) > before,
           "the voice should advance on a healthy device");

    tearDownEngine();
    std::printf("  ok\n");
}

/// The basic contract: reported within the threshold, not before it, once
/// per stall, and again after callbacks have resumed and then stall anew.
void testStallIsReportedOncePerEpisode()
{
    std::printf("stall is reported once per episode, and re-arms on resume\n");
    const unsigned int handle = bringUpPlaying();
    if (handle == 0)
        return;

    const long long stalledAt = nowMs();
    soloud_test::setDeviceCallbackStalled(true);
    // Let a callback already in flight finish before sampling the position.
    sleepMs(50);
    const double positionWhileStalled = getPosition(handle);

    const bool reported =
        waitForStallEvents(1, kThresholdMs + kSampleIntervalMs +
                                  kDetectionMarginMs);
    EXPECT(reported, "a stalled device callback was never reported");
    if (reported)
    {
        const long long latency =
            gLastStallAtMs.load(std::memory_order_acquire) - stalledAt;
        EXPECT(latency >= kThresholdMs - kEarlyToleranceMs,
               "reported %lldms after the stall began, before the %lldms "
               "threshold",
               latency, kThresholdMs);
        EXPECT(latency <= kThresholdMs + kSampleIntervalMs + kDetectionMarginMs,
               "reported only %lldms after the stall began", latency);
        std::printf("  reported %lldms after the stall began\n", latency);
    }

    // The simulated failure is the field one: the backend still says started,
    // the voice is valid and unpaused, and it does not move.
    EXPECT(getAudioDeviceState() == audioDeviceStarted,
           "the stalled device should still report started");
    EXPECT(getPause(handle) == 0, "the voice should still be unpaused");
    EXPECT(getPosition(handle) == positionWhileStalled,
           "the voice advanced while the device callback was stalled");

    // However long it lasts, one stall is one report.
    sleepMs(4 * kThresholdMs);
    EXPECT(stallEvents() == 1,
           "one stall should be reported exactly once, got %d reports",
           stallEvents());

    // Callbacks resume: the episode is over, and the next one is reported.
    soloud_test::setDeviceCallbackStalled(false);
    EXPECT(waitForRunningCallbacks(), "callbacks should resume");
    EXPECT(getPosition(handle) > positionWhileStalled,
           "the voice should advance again once callbacks resume");
    // Give the watchdog several samples of the resumed heartbeat.
    sleepMs(kThresholdMs);
    EXPECT(stallEvents() == 1, "resuming must not itself be reported");

    soloud_test::setDeviceCallbackStalled(true);
    EXPECT(waitForStallEvents(2, kThresholdMs + kSampleIntervalMs +
                                     kDetectionMarginMs),
           "a second stall after callbacks resumed was not reported");
    sleepMs(2 * kThresholdMs);
    EXPECT(stallEvents() == 2, "expected exactly two reports, got %d",
           stallEvents());

    tearDownEngine();
    std::printf("  ok\n");
}

/// With the render-ahead ring, the mixer also runs on API threads: play()
/// re-mixes retroactively on the calling thread. A heartbeat taken from the
/// mixer would keep moving here; the device callback's does not.
void testRenderAheadWatchesTheDeviceCallback()
{
    std::printf("render-ahead: the device callback is watched, not the mixer\n");
    const unsigned int handle = bringUpPlaying(/*renderAheadFrames*/ 8192);
    if (handle == 0)
        return;
    const unsigned int hash = loadTestWaveform();

    const long long stalledAt = nowMs();
    soloud_test::setDeviceCallbackStalled(true);

    // Keep the mixer busy on this thread while the device is dead.
    const long long deadline =
        stalledAt + kThresholdMs + kSampleIntervalMs + kDetectionMarginMs;
    while (stallEvents() == 0 && nowMs() < deadline)
    {
        const unsigned int extra = playLooping(hash);
        sleepMs(20);
        stop(extra);
        sleepMs(20);
    }
    EXPECT(stallEvents() == 1,
           "a dead device callback in render-ahead mode was not reported "
           "exactly once (%d reports)",
           stallEvents());
    if (stallEvents() > 0)
        std::printf("  reported %lldms after the stall began\n",
                    gLastStallAtMs.load(std::memory_order_acquire) - stalledAt);

    soloud_test::setDeviceCallbackStalled(false);
    EXPECT(waitForRunningCallbacks(), "callbacks should resume");
    tearDownEngine();
    std::printf("  ok\n");
}

/// A stopped device is silent by design. And the watchdog parked on it must
/// wake when the device is started again: a start into a still-dead callback
/// is reported.
void testNoReportWhileStoppedAndRearmOnRestart()
{
    std::printf("no report while stopped; re-armed by a restart\n");
    const unsigned int handle = bringUpPlaying();
    if (handle == 0)
        return;

    EXPECT(stopAudioDevice(/*force*/ 1) == PlayerErrors::noError,
           "the forced stop should succeed");
    EXPECT(waitForDeviceState(audioDeviceStopped, 2000),
           "the device should be stopped");
    soloud_test::setDeviceCallbackStalled(true);
    sleepMs(4 * kThresholdMs);
    EXPECT(stallEvents() == 0,
           "a stopped device was reported as stalled (%d reports)",
           stallEvents());

    // Restarting into the same dead callback is a new episode.
    EXPECT(startAudioDevice() == PlayerErrors::noError,
           "the explicit start should succeed");
    EXPECT(getAudioDeviceState() == audioDeviceStarted,
           "the device should report started");
    EXPECT(waitForStallEvents(1, kThresholdMs + kSampleIntervalMs +
                                     kDetectionMarginMs),
           "a restarted device whose callback never runs was not reported");

    // A stall already reported is reported again after a stop and restart,
    // even when the watchdog never got to see the stopped state.
    EXPECT(stopAudioDevice(1) == PlayerErrors::noError, "stop should succeed");
    EXPECT(startAudioDevice() == PlayerErrors::noError, "start should succeed");
    EXPECT(waitForStallEvents(2, kThresholdMs + kSampleIntervalMs +
                                     kDetectionMarginMs),
           "a stall after a stop and restart was not reported again");

    tearDownEngine();
    std::printf("  ok\n");
}

/// After the idle timeout has stopped the device, nothing is reported however
/// long it stays silent.
void testNoReportAfterIdleStop()
{
    std::printf("no report after an idle stop\n");
    const unsigned int handle = bringUpPlaying();
    if (handle == 0)
        return;

    stop(handle);
    setAudioDeviceIdleTimeout(50);
    const bool idleStopped = waitForDeviceState(audioDeviceStopped, 3000);
    EXPECT(idleStopped, "the idle timeout should stop the device");
    soloud_test::setDeviceCallbackStalled(true);
    sleepMs(4 * kThresholdMs);
    EXPECT(stallEvents() == 0,
           "an idle-stopped device was reported as stalled (%d reports)",
           stallEvents());

    tearDownEngine();
    std::printf("  ok\n");
}

/// An OS interruption silences the device on purpose. Once it ends and the
/// device is started again, a dead callback is reported as usual.
void testNoReportDuringInterruption()
{
    std::printf("no report during an OS interruption\n");
    const unsigned int handle = bringUpPlaying();
    if (handle == 0)
        return;

    soloud_test::setDeviceCallbackStalled(true);
    debugTriggerAudioInterruption(1);
    sleepMs(4 * kThresholdMs);
    EXPECT(stallEvents() == 0,
           "a device silenced by an interruption was reported (%d reports)",
           stallEvents());

    // The interruption ends with a voice still playing, so the device is
    // started again -- into the same dead callback.
    debugTriggerAudioInterruption(0);
    EXPECT(waitForDeviceState(audioDeviceStarted, 3000),
           "the interruption end should restart the device");
    EXPECT(waitForStallEvents(1, kThresholdMs + kSampleIntervalMs +
                                     kDetectionMarginMs),
           "a dead callback after the interruption ended was not reported");

    tearDownEngine();
    std::printf("  ok\n");
}

/// A process suspension freezes the audio callback and the watchdog together,
/// and on resume the watchdog can run first. The OS also tends to stop the
/// audio IO a little before it suspends the app, so the watchdog may already
/// have seen the callback go quiet -- for less than a threshold -- when it is
/// frozen. Parking the watchdog on a barrier while the callback is held stalled
/// reproduces that deterministically.
void freezeWatchdogShortlyAfterCallbackStops()
{
    soloud_test::setDeviceCallbackStalled(true);
    // At least one sample sees the callback quiet before the freeze, so the
    // watchdog's last view of it is "silent for a while", not "progressing".
    // Without that the first sample after the thaw sees the progress made
    // before the freeze, and the scenario proves nothing.
    sleepMs(kSampleIntervalMs * 3 / 2);
    soloud_test::armBarrier(DeviceBarrier::stallWatchdogWoke);
    soloud_test::waitBarrierReached(DeviceBarrier::stallWatchdogWoke);
    EXPECT(stallEvents() == 0,
           "reported before the freeze; the scenario needs the freeze to land "
           "within the threshold");
    sleepMs(3 * kThresholdMs);
}

void testSuspendedWatchdogDoesNotReportOnResume()
{
    std::printf("a watchdog frozen by a suspension does not report on resume\n");
    const unsigned int handle = bringUpPlaying();
    if (handle == 0)
        return;

    // Thaw the watchdog first, and the callback a little later, but well
    // within one threshold. Counting the frozen time as silence would report
    // here, before the callback had a chance to run.
    freezeWatchdogShortlyAfterCallbackStops();
    soloud_test::releaseBarrier(DeviceBarrier::stallWatchdogWoke);
    sleepMs(kThresholdMs / 2);
    soloud_test::setDeviceCallbackStalled(false);
    sleepMs(3 * kThresholdMs);
    EXPECT(stallEvents() == 0,
           "the watchdog reported a stall on resuming from a freeze (%d "
           "reports)",
           stallEvents());

    // The same freeze, but the callback stays dead after the thaw: it is
    // reported, and only after a full threshold of observation since resume.
    EXPECT(waitForRunningCallbacks(), "callbacks should be running again");
    sleepMs(2 * kSampleIntervalMs);
    resetStallEvents();
    freezeWatchdogShortlyAfterCallbackStops();
    const long long resumedAt = nowMs();
    soloud_test::releaseBarrier(DeviceBarrier::stallWatchdogWoke);
    const bool reported =
        waitForStallEvents(1, kThresholdMs + kSampleIntervalMs +
                                  kDetectionMarginMs);
    EXPECT(reported, "a stall that outlived the freeze was not reported");
    if (reported)
    {
        const long long sinceResume =
            gLastStallAtMs.load(std::memory_order_acquire) - resumedAt;
        EXPECT(sinceResume >= kThresholdMs - kEarlyToleranceMs,
               "reported %lldms after resume: the frozen time was counted",
               sinceResume);
        std::printf("  reported %lldms after resume\n", sinceResume);
    }

    tearDownEngine();
    std::printf("  ok\n");
}

/// Teardown joins the watchdog promptly mid-stall, and nothing is reported
/// once the engine is gone.
void testTeardownJoinsWatchdog()
{
    std::printf("teardown joins the watchdog; nothing after deinit\n");
    const unsigned int handle = bringUpPlaying();
    if (handle == 0)
        return;

    soloud_test::setDeviceCallbackStalled(true);
    sleepMs(kThresholdMs / 2);
    const long long start = nowMs();
    dispose();
    const long long elapsed = nowMs() - start;
    EXPECT(elapsed < kTeardownBudgetMs,
           "dispose() mid-stall took %lldms", elapsed);
    EXPECT(isInited() == 0, "the engine should be gone");

    resetStallEvents();
    sleepMs(3 * kThresholdMs);
    EXPECT(stallEvents() == 0,
           "a stall was reported after the engine was torn down (%d)",
           stallEvents());

    tearDownEngine();
    std::printf("  ok: disposed in %lldms\n", elapsed);
}

/// Abort a wedged run instead of hanging CI: a watchdog that cannot be joined
/// is itself one of the bugs this suite looks for.
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

} // namespace

int main()
{
    startHangGuard(180);
    soloud_test::setDeviceStallThresholdMs(kThresholdMs);

    // Every scenario needs a device whose callback really runs. miniaudio
    // opens its null backend on a machine with no audio hardware, so a failure
    // here means a broken environment, not merely a headless one.
    const bool canInit = bringUpEngine();
    tearDownEngine();
    if (!canInit)
    {
        std::fprintf(stderr,
                     "FATAL: no output device could be opened, not even a null "
                     "backend; the stall watchdog scenarios cannot run.\n"
                     "Set SOLOUD_LIFECYCLE_TEST_ALLOW_NO_DEVICE=1 to downgrade "
                     "this to a skip.\n");
        return std::getenv("SOLOUD_LIFECYCLE_TEST_ALLOW_NO_DEVICE") ? 0 : 1;
    }

    testNoReportDuringNormalPlayback();
    testStallIsReportedOncePerEpisode();
    testRenderAheadWatchesTheDeviceCallback();
    testNoReportWhileStoppedAndRearmOnRestart();
    testNoReportAfterIdleStop();
    testNoReportDuringInterruption();
    testSuspendedWatchdogDoesNotReportOnResume();
    testTeardownJoinsWatchdog();

    soloud_test::setDeviceStallThresholdMs(0);
    std::printf("\n%d assertions, %d failures\n", gAssertions, gFailures);
    return gFailures == 0 ? 0 : 1;
}
