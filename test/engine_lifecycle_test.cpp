// Standalone native regression tests for FlutterEngine lifecycle ownership.
//
// The native engine is process-global: one Player, one output device, one
// lifecycle scheduler, one set of Dart callback pointers. The Dart isolate that
// drives it belongs to a single FlutterEngine, which can go away while the
// process keeps running -- a cached engine behind audio_service, an add-to-app
// host destroying an engine, or a hot restart swapping the isolate underneath a
// live engine. The Android plugin bridges those transitions into the two
// entry points exercised here:
//
//   clearDartCallbackRegistrationsForEngine()  (hot restart, and detach)
//   requestEngineTeardownForEngine()           (engine destroy)
//
// What these tests pin down:
//
//   * a retired callable is never invoked again;
//   * callback ownership and lifecycle ownership are separate, so an engine
//     that owns the callables but not the claim retires its own callables and
//     still cannot dispose the engine that replaced it;
//   * a teardown queued by a destroyed engine cannot dispose or disturb the
//     engine that claimed after it;
//   * both hooks return promptly even while init_deinit_mutex is held by an
//     unrelated operation -- they run on Android's platform thread, where
//     waiting for a device operation is an ANR;
//   * duplicate destroy/detach notifications tear down exactly once.
//
// Build and run from the flutter_soloud repository root with:
//
//   ./test/run_engine_lifecycle_test.sh

#include "enums.h"

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
    void setDartEventCallback(void (*voice_ended)(unsigned int *),
                              void (*file_loaded)(enum PlayerErrors *, char *,
                                                  unsigned int *, uint64_t *),
                              void (*state_changed)(enum PlayerStateEvents *),
                              int64_t owner_engine_id);
    bool clearDartCallbackRegistrationsForEngine(int64_t engine_id);
    bool requestEngineTeardownForEngine(int64_t engine_id);

    // Test-only hooks (SOLOUD_LIFECYCLE_TEST_HOOKS).
    void soloudTestLockInitDeinit();
    void soloudTestUnlockInitDeinit();
    void soloudTestInvokeStateChanged(unsigned int state);
    int soloudTestPlayerIsInited();
}

namespace
{

constexpr int64_t kEngineA = 1001;
constexpr int64_t kEngineB = 1002;
constexpr int64_t kNoEngineId = -1;

// A lifecycle hook does a handful of atomic stores and spawns a detached
// worker. The number this excludes is the one that matters: the seconds a
// device stop or a scheduler join can take while init_deinit_mutex is held.
constexpr long long kPlatformThreadBudgetMs = 100;

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

std::atomic<int> gVoiceEndedCalls{0};
std::atomic<int> gFileLoadedCalls{0};
std::atomic<int> gStateChangedCalls{0};

// Stand-ins for the Dart trampolines. Native code owns the pointers it hands
// over, exactly as the real callables do.
void onVoiceEnded(unsigned int *handle)
{
    std::free(handle);
    ++gVoiceEndedCalls;
}

void onFileLoaded(enum PlayerErrors *error, char *name, unsigned int *hash,
                  uint64_t *counter)
{
    std::free(error);
    std::free(name);
    std::free(hash);
    std::free(counter);
    ++gFileLoadedCalls;
}

void onStateChanged(enum PlayerStateEvents *state)
{
    std::free(state);
    ++gStateChangedCalls;
}

void registerCallbacksFor(int64_t engineId)
{
    setDartEventCallback(onVoiceEnded, onFileLoaded, onStateChanged, engineId);
}

/// Number of times the state-changed bridge reached a callable.
int stateChangedCallsAfterDispatch()
{
    soloudTestInvokeStateChanged(0);
    return gStateChangedCalls.load();
}

template <typename Predicate>
bool waitFor(Predicate predicate, int timeoutMs = 5000)
{
    const auto deadline =
        std::chrono::steady_clock::now() + std::chrono::milliseconds(timeoutMs);
    while (std::chrono::steady_clock::now() < deadline)
    {
        if (predicate())
            return true;
        std::this_thread::sleep_for(std::chrono::milliseconds(5));
    }
    return predicate();
}

long long millisSince(std::chrono::steady_clock::time_point start)
{
    return std::chrono::duration_cast<std::chrono::milliseconds>(
               std::chrono::steady_clock::now() - start)
        .count();
}

/// Every test starts from a process with no engine and no lifecycle claim.
void resetGlobalState()
{
    dispose();
    gVoiceEndedCalls = 0;
    gFileLoadedCalls = 0;
    gStateChangedCalls = 0;
}

/// Whether a real output device could be opened. Several scenarios need an
/// initialized engine; on a machine without any audio device those are skipped
/// rather than reported as failures.
bool gHasAudioDevice = false;

bool initEngineAs(int64_t engineId)
{
    prepareEngineInit(engineId);
    const PlayerErrors error = initEngine(-1, 44100, 2048, 2, 0);
    return error == noError;
}

// ---------------------------------------------------------------------------

/// Hot restart keeps the same FlutterEngine -- same engine id, same lifecycle
/// claim -- and replaces only the isolate. The callables must go inert
/// immediately; the engine must stay claimed, because the new isolate's init()
/// is what disposes the stale engine.
void testHotRestartRetiresCallablesButKeepsClaim()
{
    std::printf("hot restart retires callables and keeps the claim\n");
    resetGlobalState();

    prepareEngineInit(kEngineA);
    registerCallbacksFor(kEngineA);

    EXPECT(stateChangedCallsAfterDispatch() == 1,
           "a registered callable should be invoked");

    EXPECT(clearDartCallbackRegistrationsForEngine(kEngineA),
           "the owning engine should be allowed to retire its callables");
    EXPECT(stateChangedCallsAfterDispatch() == 1,
           "a retired callable must never be invoked again");

    // The claim survived the restart, so a later destroy of the same
    // FlutterEngine is still accepted.
    EXPECT(requestEngineTeardownForEngine(kEngineA),
           "hot restart must not release the lifecycle claim");

    resetGlobalState();
}

/// A detaching engine must never retire somebody else's callables.
void testCallbackRetirementIsScopedToTheOwner()
{
    std::printf("callback retirement is scoped to the owning engine\n");
    resetGlobalState();

    prepareEngineInit(kEngineA);
    registerCallbacksFor(kEngineA);

    EXPECT(!clearDartCallbackRegistrationsForEngine(kEngineB),
           "a non-owner must not retire the current registration");
    EXPECT(stateChangedCallsAfterDispatch() == 1,
           "the owner's callables must still be live");

    EXPECT(!clearDartCallbackRegistrationsForEngine(kNoEngineId),
           "the no-engine sentinel must never match an owner");

    resetGlobalState();
}

/// The regression behind the "retire callables even when teardown is refused"
/// rule. Engine A's initialization worker can win init_deinit_mutex after B has
/// already claimed, leaving A owning the callables and B owning the engine.
/// Destroying A must retire A's callables and leave B's engine alone.
void testCallbackOwnerDiffersFromLifecycleOwner()
{
    std::printf("callback owner may differ from lifecycle owner\n");
    resetGlobalState();

    prepareEngineInit(kEngineA);
    registerCallbacksFor(kEngineA);
    EXPECT(stateChangedCallsAfterDispatch() == 1,
           "A's callables should start out live");

    // B claims the native engine while A's callables are still published.
    prepareEngineInit(kEngineB);

    EXPECT(!requestEngineTeardownForEngine(kEngineA),
           "A must not tear down the engine B now owns");
    EXPECT(stateChangedCallsAfterDispatch() == 1,
           "A's callables must be retired even though its teardown was refused");
    EXPECT(requestEngineTeardownForEngine(kEngineB),
           "B's lifecycle claim must be intact");

    resetGlobalState();
}

/// Nothing is claimed, so there is nothing for a destroyed engine to tear down.
void testTeardownRefusedWithoutAClaim()
{
    std::printf("teardown is refused when nothing is claimed\n");
    resetGlobalState();

    EXPECT(!requestEngineTeardownForEngine(kEngineA),
           "an unclaimed engine has nothing to tear down");
    EXPECT(!requestEngineTeardownForEngine(kNoEngineId),
           "the no-engine sentinel must never be accepted");

    // An ordinary Dart deinit releases the claim, so a detach arriving
    // afterwards is a no-op rather than a second teardown.
    prepareEngineInit(kEngineA);
    dispose();
    EXPECT(!requestEngineTeardownForEngine(kEngineA),
           "dispose() must release the lifecycle claim");

    resetGlobalState();
}

/// The ordinary destroy path: bridges inert at once, native engine gone
/// shortly after, and the duplicate notification (onEngineWillDestroy() and
/// onDetachedFromEngine() both fire) tears down exactly once.
void testEngineDestroyDisposesTheNativeEngine()
{
    std::printf("engine destroy disposes the native engine exactly once\n");
    resetGlobalState();

    if (!gHasAudioDevice)
    {
        std::printf("  SKIPPED: no usable output device\n");
        return;
    }

    EXPECT(initEngineAs(kEngineA), "the engine should initialize");
    registerCallbacksFor(kEngineA);
    EXPECT(soloudTestPlayerIsInited() == 1, "the player should be initialized");
    EXPECT(isInited() == 1, "readiness should be published");

    const auto start = std::chrono::steady_clock::now();
    const bool accepted = requestEngineTeardownForEngine(kEngineA);
    const long long elapsed = millisSince(start);

    EXPECT(accepted, "the owning engine's teardown should be accepted");
    EXPECT(elapsed < kPlatformThreadBudgetMs,
           "teardown request took %lldms; it must not block the platform thread",
           elapsed);
    EXPECT(stateChangedCallsAfterDispatch() == 0,
           "the callables must be inert as soon as the request returns");

    EXPECT(waitFor([] { return soloudTestPlayerIsInited() == 0; }),
           "the native engine should be disposed by the worker");
    EXPECT(isInited() == 0, "readiness must not survive the teardown");

    // onDetachedFromEngine() arriving after onEngineWillDestroy().
    EXPECT(!requestEngineTeardownForEngine(kEngineA),
           "a duplicate destroy/detach must not tear down again");

    resetGlobalState();
}

/// A teardown worker that reaches init_deinit_mutex after a replacement engine
/// has claimed must leave that engine completely alone. This is the scenario
/// the generation exists for, forced rather than raced: the mutex is held for
/// the whole window in which the replacement claims and registers.
void testStaleTeardownCannotDisposeReplacement()
{
    std::printf("a stale teardown cannot dispose the replacement engine\n");
    resetGlobalState();

    if (!gHasAudioDevice)
    {
        std::printf("  SKIPPED: no usable output device\n");
        return;
    }

    EXPECT(initEngineAs(kEngineA), "the engine should initialize");
    registerCallbacksFor(kEngineA);

    // Stand in for an unrelated operation holding the lifecycle lock -- a
    // loadFile(), a device change -- so the teardown worker has to queue.
    soloudTestLockInitDeinit();

    const auto start = std::chrono::steady_clock::now();
    const bool accepted = requestEngineTeardownForEngine(kEngineA);
    const long long elapsed = millisSince(start);

    EXPECT(accepted, "A's teardown should be accepted while A still owns");
    EXPECT(elapsed < kPlatformThreadBudgetMs,
           "teardown request took %lldms with the lifecycle lock held; it must "
           "not wait for it",
           elapsed);
    EXPECT(stateChangedCallsAfterDispatch() == 0,
           "A's callables must be inert immediately, not once the lock frees");

    // The replacement claims and publishes its own callables while A's worker
    // is still queued behind the mutex.
    prepareEngineInit(kEngineB);
    registerCallbacksFor(kEngineB);

    soloudTestUnlockInitDeinit();

    // Give the queued worker every chance to run and do damage.
    std::this_thread::sleep_for(std::chrono::milliseconds(300));

    EXPECT(soloudTestPlayerIsInited() == 1,
           "A's stale worker must not dispose the engine B claimed");
    EXPECT(stateChangedCallsAfterDispatch() == 1,
           "B's callables must survive A's teardown");
    EXPECT(requestEngineTeardownForEngine(kEngineB),
           "B's lifecycle claim must still be current");
    EXPECT(waitFor([] { return soloudTestPlayerIsInited() == 0; }),
           "B's own teardown should dispose the engine");

    resetGlobalState();
}

/// The Player-owned callbacks (the per-BufferStream ones) need
/// init_deinit_mutex, which a lifecycle hook must not wait for. The hook has to
/// return with the global bridges already inert and the rest queued.
void testDeferredPlayerCallbackClear()
{
    std::printf("player-owned callback clear is deferred, not waited on\n");
    resetGlobalState();

    prepareEngineInit(kEngineA);
    registerCallbacksFor(kEngineA);

    soloudTestLockInitDeinit();

    const auto start = std::chrono::steady_clock::now();
    const bool cleared = clearDartCallbackRegistrationsForEngine(kEngineA);
    const long long elapsed = millisSince(start);

    EXPECT(cleared, "the owner should be allowed to retire its callables");
    EXPECT(elapsed < kPlatformThreadBudgetMs,
           "callback clear took %lldms with the lifecycle lock held; it must "
           "not wait for it",
           elapsed);
    EXPECT(stateChangedCallsAfterDispatch() == 0,
           "the global bridges must be inert before the lock is released");

    soloudTestUnlockInitDeinit();

    // The queued worker takes the mutex once it is free; this call cannot
    // return until it has released it again.
    EXPECT(waitFor([] { return soloudTestPlayerIsInited() == 0; }),
           "the deferred worker must not wedge the lifecycle lock");

    resetGlobalState();
}

} // namespace

int main()
{
    // One probe decides whether the device-dependent scenarios can run.
    gHasAudioDevice = initEngineAs(kEngineA);
    dispose();
    if (!gHasAudioDevice)
    {
        std::printf(
            "note: no usable output device; engine-init scenarios skipped\n");
    }

    testHotRestartRetiresCallablesButKeepsClaim();
    testCallbackRetirementIsScopedToTheOwner();
    testCallbackOwnerDiffersFromLifecycleOwner();
    testTeardownRefusedWithoutAClaim();
    testEngineDestroyDisposesTheNativeEngine();
    testStaleTeardownCannotDisposeReplacement();
    testDeferredPlayerCallbackClear();

    std::printf("\n%d assertions, %d failures\n", gAssertions, gFailures);
    return gFailures == 0 ? 0 : 1;
}
