// Standalone native regression tests for a play racing the disposal of the
// sound it plays.
//
// Player::findByHash() takes sounds_mutex only for the lookup and returns a
// raw ActiveSound*. disposeSound() and disposeAllSound() stop the sound's
// voices and take it out of the list under that mutex, then destroy it. A
// play*() that kept using the pointer without the mutex could therefore:
//
//   * dereference the sound after another thread destroyed it, if the
//     disposal ran between its lookup and its use; or
//   * create a voice on the sound after the disposal had stopped its voices,
//     leaving a voice alive while the sound and its filters are destroyed.
//
// What these tests pin down, for each of the six play variants and both
// disposals:
//
//   * a play parked between its lookup and its use of the sound (the
//     playSoundLookedUp barrier) keeps the disposal from finishing: the
//     disposal is still waiting for it a bounded while later. The wait is
//     bounded because, with the fix, the play holds the mutex while parked and
//     an unbounded wait for the disposal would deadlock;
//   * once the play resumes, it either finished first -- its voice is then
//     stopped by the disposal while the sound is still registered, i.e.
//     before the sound is destroyed -- or it found no sound;
//   * afterwards no voice is alive, no handle is registered and no sound is
//     loaded;
//   * disposeSound() stops every voice of the sound before destroying it, not
//     only some of them, however many there are;
//   * a feeder thread parked inside addPullBufferDataStream() after looking
//     its playing PullBufferStream up (the pullDataSoundLookedUp barrier)
//     keeps either disposal from finishing in the same way, its data is added
//     before the stream is destroyed, and a delivery after the disposal finds
//     no stream. Without the mutex held across the delivery this is a
//     heap-use-after-free in PullBufferStream::addAudioData() under
//     AddressSanitizer.
//
// Under AddressSanitizer the first scenario reports a heap-use-after-free in
// Player::play() when the play does not hold sounds_mutex across its use of
// the sound. Without a sanitizer that build fails the "disposal finished
// while the play was parked" assertion instead.
//
// Holding sounds_mutex across a play must not deadlock against the threads
// that pause and unpause voices automatically, nor post device lifecycle
// requests (which take mInterruptionMutex) while it is held:
//
//   * a BufferStream's background decode thread runs checkBuffering() with
//     check_buffer_mutex held, and a play() of that stream holds sounds_mutex
//     and goes on to take check_buffer_mutex. The decode thread's automatic
//     unpause must complete while such a play is parked holding sounds_mutex
//     (the bufferStreamBackgroundDecoded and playSoundLookedUp barriers), so
//     it must not take sounds_mutex. This one cannot fail softly: if the
//     unpause does not complete in a bounded time, releasing the parked play
//     would deadlock, so the run ends there;
//   * an automatic pause and an automatic unpause made under sounds_mutex (a
//     play() and an addAudioDataStream() of a buffer stream) post their
//     lifecycle request only once sounds_mutex is free: the thread is parked
//     where the request is posted (lifecyclePostEntered), before
//     mInterruptionMutex is taken, and sounds_mutex is shown to be free then;
//   * a PullBufferStream's buffering check, which pauses and unpauses its
//     voice automatically, runs only on a thread delivering data, holding
//     sounds_mutex, never on the mixer: a mixer that completes a pending seek
//     by decoding delivered data itself leaves the check to the delivery (the
//     pullSeekCompleted and pullBufferingCheckEntered barriers). The check
//     reads the voice's entry in the sound's handle list and writes its
//     user-paused flag, which sounds_mutex protects, and its getPause() can
//     wait for a control thread, which the audio thread must never do.
//
// Build and run from the flutter_soloud repository root with:
//
//   ./test/run_play_dispose_race_test.sh            (plain)
//   ./test/run_play_dispose_race_test.sh --asan     (AddressSanitizer)
//   ./test/run_play_dispose_race_test.sh --tsan     (ThreadSanitizer)

#include "bindings.h"
#include "device_lifecycle_test_hooks.h"
#include "enums.h"

#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <fstream>
#include <functional>
#include <memory>
#include <mutex>
#include <thread>
#include <vector>

extern "C"
{
    // Test-only exports, compiled in with SOLOUD_LIFECYCLE_TEST_HOOKS.
    int soloudTestSoundCount();
    int soloudTestSoundIsLoaded(unsigned int hash);
    int soloudTestVoiceIsValid(unsigned int handle);
}

namespace
{

using soloud_test::DeviceBarrier;

constexpr int64_t kEngineId = 4001;

// How long the test waits for a disposal to finish while a play is parked
// with the sound looked up. With the fix the disposal cannot finish until the
// play is released, so this is how long it is shown to be held off; without
// it the disposal finishes in well under a millisecond.
constexpr long long kParkMs = 300;

// Long enough that the idle policy never stops the device during a test.
constexpr int64_t kQuietIdleTimeoutMs = 600000;

// Bound on waits for work that needs no lock the test holds back: a thread
// reaching a barrier, a decode thread's buffering decision. Generous, since
// passing runs take milliseconds; only a deadlocked run waits this long.
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
// Voice-ended bookkeeping
// ---------------------------------------------------------------------------

/// A voice-ended notification, and whether the watched sound was still
/// registered when it arrived. A disposal stops the voices under sounds_mutex
/// before it takes the sound out of the list, so a voice it stopped reports
/// the sound as still loaded. A voice that outlived that point is only
/// stopped by the AudioSource destructor, after the sound left the list.
struct EndedVoice
{
    unsigned int handle;
    int soundLoaded;
};

std::mutex gEndedMutex;
std::vector<EndedVoice> gEnded;
std::atomic<unsigned int> gWatchedHash{0};

void onVoiceEnded(unsigned int *handle)
{
    const unsigned int ended = *handle;
    // Ownership is handed over exactly as it is to Dart.
    std::free(handle);

    const unsigned int hash = gWatchedHash.load(std::memory_order_acquire);
    // Runs on the disposing thread, which may hold sounds_mutex; it is
    // recursive, and this is the lookup the voice-ended path already makes.
    const int loaded = hash != 0 ? soloudTestSoundIsLoaded(hash) : -2;

    std::lock_guard<std::mutex> lock(gEndedMutex);
    gEnded.push_back({ended, loaded});
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

void watchSound(unsigned int hash)
{
    std::lock_guard<std::mutex> lock(gEndedMutex);
    gEnded.clear();
    gWatchedHash.store(hash, std::memory_order_release);
}

/// The sound-loaded flag recorded when [handle] ended, or -1 if it never
/// reported ending.
int loadedWhenEnded(unsigned int handle)
{
    std::lock_guard<std::mutex> lock(gEndedMutex);
    for (const EndedVoice &e : gEnded)
    {
        if (e.handle == handle)
            return e.soundLoaded;
    }
    return -1;
}

// ---------------------------------------------------------------------------
// Buffer stream buffering events
// ---------------------------------------------------------------------------

/// A buffering callback: [isBuffering] true when a voice was paused for
/// buffering, false when it was unpaused again. It is invoked right after the
/// stream's Player::setPause() call returns.
struct BufferingEvent
{
    bool isBuffering;
    unsigned int handle;
};

std::mutex gBufferingMutex;
std::condition_variable gBufferingCv;
std::vector<BufferingEvent> gBuffering;

void onBuffering(bool isBuffering, unsigned int handle, double)
{
    {
        std::lock_guard<std::mutex> lock(gBufferingMutex);
        gBuffering.push_back({isBuffering, handle});
    }
    gBufferingCv.notify_all();
}

void clearBufferingEvents()
{
    std::lock_guard<std::mutex> lock(gBufferingMutex);
    gBuffering.clear();
}

bool sawBufferingEventLocked(bool isBuffering, unsigned int handle)
{
    for (const BufferingEvent &e : gBuffering)
    {
        if (e.isBuffering == isBuffering && e.handle == handle)
            return true;
    }
    return false;
}

/// Wait, at most [ms], for a buffering event since the last clear.
bool waitForBufferingEvent(bool isBuffering, unsigned int handle,
                           long long ms)
{
    std::unique_lock<std::mutex> lock(gBufferingMutex);
    return gBufferingCv.wait_for(
        lock, std::chrono::milliseconds(ms),
        [&] { return sawBufferingEventLocked(isBuffering, handle); });
}

// ---------------------------------------------------------------------------
// Bounded waits
// ---------------------------------------------------------------------------

void printSummary()
{
    std::printf("\n%d assertions, %d failures\n", gAssertions, gFailures);
    std::fflush(stdout);
    std::fflush(stderr);
}

/// End the run now: a thread is wedged on a lock, so neither releasing it nor
/// waiting for it can end, and static destruction must not run under it.
[[noreturn]] void abortWedgedRun(const char *why)
{
    ++gFailures;
    std::fprintf(stderr, "  FAIL: %s\n  Ending the run here.\n", why);
    printSummary();
    std::_Exit(1);
}

/// soloud_test::waitBarrierReached() with a bound, which it does not have: a
/// helper thread waits for the barrier and reports back. The helper stays
/// blocked when the barrier is never reached, so a false return must end the
/// run (abortWedgedRun()).
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

/// Takes Player::sounds_mutex (a findByHash() through the test export) on its
/// own thread, so a test can tell whether the mutex is free right now without
/// blocking itself on it.
class SoundsMutexProbe
{
public:
    explicit SoundsMutexProbe(unsigned int hash)
        : mThread(
              [this, hash]
              {
                  soloudTestSoundIsLoaded(hash);
                  {
                      std::lock_guard<std::mutex> lock(mMutex);
                      mDone = true;
                  }
                  mCv.notify_all();
              })
    {
    }

    ~SoundsMutexProbe()
    {
        if (mThread.joinable())
            mThread.join();
    }

    /// Whether the probe got the mutex within [ms].
    bool acquiredWithin(long long ms)
    {
        std::unique_lock<std::mutex> lock(mMutex);
        return mCv.wait_for(lock, std::chrono::milliseconds(ms),
                            [&] { return mDone; });
    }

private:
    // Declared before the thread, which uses them from its first instruction.
    std::mutex mMutex;
    std::condition_variable mCv;
    bool mDone = false;
    std::thread mThread;
};

/// The first [count] bytes of [path], relative to the repository root.
std::vector<unsigned char> readFilePrefix(const char *path, size_t count)
{
    std::ifstream in(path, std::ios::binary);
    std::vector<unsigned char> data;
    if (!in)
        return data;
    data.resize(count);
    in.read(reinterpret_cast<char *>(data.data()),
            static_cast<std::streamsize>(count));
    data.resize(static_cast<size_t>(in.gcount()));
    return data;
}

// ---------------------------------------------------------------------------
// Engine
// ---------------------------------------------------------------------------

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
    watchSound(0);
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

// ---------------------------------------------------------------------------
// The play variants and the disposals
// ---------------------------------------------------------------------------

enum class PlayVariant
{
    play,
    playClocked,
    playScheduled,
    play3d,
    play3dClocked,
    play3dScheduled,
};

constexpr PlayVariant kPlayVariants[] = {
    PlayVariant::play,          PlayVariant::playClocked,
    PlayVariant::playScheduled, PlayVariant::play3d,
    PlayVariant::play3dClocked, PlayVariant::play3dScheduled,
};

const char *variantName(PlayVariant variant)
{
    switch (variant)
    {
    case PlayVariant::play:
        return "play()";
    case PlayVariant::playClocked:
        return "playClocked()";
    case PlayVariant::playScheduled:
        return "playScheduled()";
    case PlayVariant::play3d:
        return "play3d()";
    case PlayVariant::play3dClocked:
        return "play3dClocked()";
    case PlayVariant::play3dScheduled:
        return "play3dScheduled()";
    }
    return "?";
}

/// Each variant through its FFI export, exactly as Dart reaches it.
PlayerErrors playVariant(PlayVariant variant, unsigned int hash,
                         unsigned int *handle)
{
    const float volume = 0.05f;
    switch (variant)
    {
    case PlayVariant::play:
        return play(hash, 0, volume, 0.0f, /*paused*/ false,
                    /*looping*/ false, 0.0, 0.0, -1, -1, 1.0f, handle);
    case PlayVariant::playClocked:
        return playClocked(hash, 0.0, 0, volume, 0.0f, 1.0f, false, 0.0, 0.0,
                           -1, -1, handle);
    case PlayVariant::playScheduled:
        return playScheduled(hash, 0.0, 0.0, 0, volume, 0.0f, 1.0f, false,
                             0.0, 0.0, -1, -1, handle);
    case PlayVariant::play3d:
        return play3dWithLoopPoints(hash, 0, 0.0f, 0.0f, 0.0f, 0.0f, 0.0f,
                                    0.0f, volume, false, false, 0.0, 0.0, -1,
                                    -1, 1.0f, handle);
    case PlayVariant::play3dClocked:
        return play3dClocked(hash, 0.0, 0, 0.0f, 0.0f, 0.0f, 0.0f, 0.0f, 0.0f,
                             volume, 1.0f, false, 0.0, 0.0, -1, -1, handle);
    case PlayVariant::play3dScheduled:
        return play3dScheduled(hash, 0.0, 0.0, 0, 0.0f, 0.0f, 0.0f, 0.0f, 0.0f,
                               0.0f, volume, 1.0f, false, 0.0, 0.0, -1, -1,
                               handle);
    }
    return PlayerErrors::invalidParameter;
}

enum class Disposal
{
    allSounds,
    oneSound,
};

const char *disposalName(Disposal disposal)
{
    return disposal == Disposal::allSounds ? "disposeAllSound()"
                                           : "disposeSound(hash)";
}

void runDisposal(Disposal disposal, unsigned int hash)
{
    if (disposal == Disposal::allSounds)
        disposeAllSound();
    else
        disposeSound(hash);
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

/// Thread B parks inside [variant] right after it has looked the sound up.
/// Thread A then runs [disposal]. The test waits a bounded while for A, then
/// releases B.
///
/// Without the fix A frees the sound at once and B, released, uses it: a
/// heap-use-after-free under AddressSanitizer, and a voice on a destroyed
/// sound without it. With the fix B holds sounds_mutex while parked, so A
/// cannot finish until B has registered its voice, which A then stops.
void raceDisposalAgainstParkedPlay(PlayVariant variant, Disposal disposal)
{
    char what[96];
    std::snprintf(what, sizeof(what), "%s vs %s", variantName(variant),
                  disposalName(disposal));
    std::printf("%s, with the play parked after its lookup\n", what);

    const unsigned int hash = loadTestWaveform();
    EXPECT(hash != 0, "%s: the test waveform should load", what);
    if (hash == 0)
        return;
    watchSound(hash);

    soloud_test::armBarrier(DeviceBarrier::playSoundLookedUp);

    PlayerErrors playResult = PlayerErrors::noError;
    unsigned int playHandle = 0;
    std::thread playThread(
        [&]
        { playResult = playVariant(variant, hash, &playHandle); });

    soloud_test::waitBarrierReached(DeviceBarrier::playSoundLookedUp);

    std::mutex doneMutex;
    std::condition_variable doneCv;
    bool disposalDone = false;
    std::thread disposeThread(
        [&]
        {
            runDisposal(disposal, hash);
            {
                std::lock_guard<std::mutex> lock(doneMutex);
                disposalDone = true;
            }
            doneCv.notify_all();
        });

    // Bounded on purpose: with the fix the disposal waits for the parked
    // play, so waiting for it without a bound would never return.
    bool finishedWhileParked;
    {
        std::unique_lock<std::mutex> lock(doneMutex);
        finishedWhileParked =
            doneCv.wait_for(lock, std::chrono::milliseconds(kParkMs),
                            [&] { return disposalDone; });
    }

    soloud_test::releaseBarrier(DeviceBarrier::playSoundLookedUp);
    playThread.join();
    disposeThread.join();

    EXPECT(!finishedWhileParked,
           "%s: the disposal finished while the play was parked between its "
           "lookup and its use of the sound",
           what);
    EXPECT(playResult == PlayerErrors::noError ||
               playResult == PlayerErrors::soundHashNotFound,
           "%s: the play should succeed or find no sound, got error %d", what,
           static_cast<int>(playResult));

    if (playResult == PlayerErrors::noError)
    {
        EXPECT(playHandle != 0, "%s: a successful play returns a handle",
               what);
        EXPECT(soloudTestVoiceIsValid(playHandle) == 0,
               "%s: the voice the play created should have been stopped",
               what);
        const int loaded = loadedWhenEnded(playHandle);
        EXPECT(loaded == 1,
               "%s: the disposal should stop the play's voice before the "
               "sound is destroyed (sound loaded when it ended: %d, -1 = "
               "never ended)",
               what, loaded);
    }
    else
    {
        EXPECT(playHandle == 0, "%s: a failed play returns no handle", what);
    }

    expectNothingLeft(what);
    watchSound(0);
}

/// disposeSound() stops the sound's voices one handle at a time, and each stop
/// reports the voice ended on the disposing thread, which takes the handle out
/// of the very list being walked. Every voice must still be stopped while the
/// sound is registered, before it is destroyed, however many it has.
void disposalStopsEveryVoiceFirst(Disposal disposal)
{
    constexpr int kVoices = 5;
    char what[96];
    std::snprintf(what, sizeof(what), "%s with %d voices",
                  disposalName(disposal), kVoices);
    std::printf("%s stops every voice before destroying the sound\n", what);

    const unsigned int hash = loadTestWaveform();
    EXPECT(hash != 0, "%s: the test waveform should load", what);
    if (hash == 0)
        return;

    unsigned int handles[kVoices] = {};
    for (unsigned int &handle : handles)
    {
        const PlayerErrors err =
            playVariant(PlayVariant::play, hash, &handle);
        EXPECT(err == PlayerErrors::noError && handle != 0,
               "%s: play should succeed (error %d)", what,
               static_cast<int>(err));
    }
    watchSound(hash);

    runDisposal(disposal, hash);

    for (unsigned int handle : handles)
    {
        if (handle == 0)
            continue;
        EXPECT(soloudTestVoiceIsValid(handle) == 0,
               "%s: voice %u should be stopped", what, handle);
        const int loaded = loadedWhenEnded(handle);
        EXPECT(loaded == 1,
               "%s: voice %u should be stopped before the sound is destroyed "
               "(sound loaded when it ended: %d, -1 = never ended)",
               what, handle, loaded);
    }

    expectNothingLeft(what);
    watchSound(0);
}

/// A feeder thread parks inside addPullBufferDataStream() right after it has
/// looked the stream up, and [disposal] runs meanwhile. The stream is playing,
/// so the delivery also runs the stream's checkBuffering() against its voice.
///
/// Without the fix the disposal destroys the PullBufferStream at once and the
/// feeder, released, decodes into it: a heap-use-after-free under
/// AddressSanitizer. With the fix the feeder holds sounds_mutex while parked,
/// so the disposal cannot finish until the data has been added.
void pullDataDeliveryAgainstDisposal(Disposal disposal)
{
    char what[96];
    std::snprintf(what, sizeof(what), "addPullBufferDataStream() vs %s",
                  disposalName(disposal));
    std::printf("%s, with the feeder parked after its lookup\n", what);

    // 32-bit float stereo PCM at 44.1 kHz: no decoder involved, so the
    // delivery is a copy into the stream's buffer plus its buffering check.
    constexpr unsigned int kSampleRate = 44100;
    constexpr unsigned int kChannels = 2;
    constexpr unsigned int kBufferBytes = 1u << 20;
    unsigned int hash = 0;
    PlayerErrors err = setPullBufferStream(
        &hash, kBufferBytes, 0.5, kSampleRate, kChannels,
        static_cast<int>(PCM_F32LE), /*audioSizeBytes*/ 64u << 20, nullptr,
        nullptr, nullptr, nullptr);
    EXPECT(err == PlayerErrors::noError && hash != 0,
           "%s: the pull stream should be created (error %d)", what,
           static_cast<int>(err));
    if (err != PlayerErrors::noError || hash == 0)
        return;

    unsigned int voice = 0;
    err = playVariant(PlayVariant::play, hash, &voice);
    EXPECT(err == PlayerErrors::noError && voice != 0,
           "%s: the pull stream should play (error %d)", what,
           static_cast<int>(err));
    watchSound(hash);

    // A quarter of a second of silence.
    const std::vector<float> pcm(kSampleRate / 4 * kChannels, 0.0f);

    soloud_test::armBarrier(DeviceBarrier::pullDataSoundLookedUp);

    PlayerErrors feedResult = PlayerErrors::noError;
    std::thread feeder(
        [&]
        {
            feedResult = addPullBufferDataStream(
                hash, reinterpret_cast<const unsigned char *>(pcm.data()),
                static_cast<unsigned int>(pcm.size() * sizeof(float)), 0);
        });

    soloud_test::waitBarrierReached(DeviceBarrier::pullDataSoundLookedUp);

    std::mutex doneMutex;
    std::condition_variable doneCv;
    bool disposalDone = false;
    std::thread disposeThread(
        [&]
        {
            runDisposal(disposal, hash);
            {
                std::lock_guard<std::mutex> lock(doneMutex);
                disposalDone = true;
            }
            doneCv.notify_all();
        });

    // Bounded on purpose: with the fix the disposal waits for the parked
    // feeder, so waiting for it without a bound would never return.
    bool finishedWhileParked;
    {
        std::unique_lock<std::mutex> lock(doneMutex);
        finishedWhileParked =
            doneCv.wait_for(lock, std::chrono::milliseconds(kParkMs),
                            [&] { return disposalDone; });
    }

    soloud_test::releaseBarrier(DeviceBarrier::pullDataSoundLookedUp);
    feeder.join();
    disposeThread.join();

    EXPECT(!finishedWhileParked,
           "%s: the disposal finished while the feeder was parked between "
           "its lookup and its use of the stream",
           what);
    EXPECT(feedResult == PlayerErrors::noError,
           "%s: the parked delivery should complete before the disposal, got "
           "error %d",
           what, static_cast<int>(feedResult));
    if (voice != 0)
    {
        EXPECT(soloudTestVoiceIsValid(voice) == 0,
               "%s: the stream's voice should have been stopped", what);
        const int loaded = loadedWhenEnded(voice);
        EXPECT(loaded == 1,
               "%s: the disposal should stop the stream's voice before the "
               "stream is destroyed (sound loaded when it ended: %d, -1 = "
               "never ended)",
               what, loaded);
    }

    // A delivery after the disposal finds no stream.
    err = addPullBufferDataStream(
        hash, reinterpret_cast<const unsigned char *>(pcm.data()),
        static_cast<unsigned int>(pcm.size() * sizeof(float)), 0);
    EXPECT(err == PlayerErrors::soundHashNotFound,
           "%s: a delivery after the disposal should find no stream, got "
           "error %d",
           what, static_cast<int>(err));

    expectNothingLeft(what);
    watchSound(0);
}

/// The mixer completes a PullBufferStream's pending seek with data a feeder
/// delivered, and must not run the stream's buffering check on the way.
///
/// PullBufferStream::checkBuffering() reads the voice's entry in the sound's
/// handle list and writes its user-paused flag, which sounds_mutex protects,
/// and calls Player::getPause(), whose lock-free read can wait for a control
/// thread in the middle of publishing a queued command. A seek out of the
/// buffered range stays pending until data at its target has been decoded,
/// and the mixer decodes delivered data itself when the feeder has not yet
/// (getAudio() -> tryDecodePendingData()), completing the seek. That used to
/// run checkBuffering() there: on the audio thread, without sounds_mutex.
///
/// The stream seeks out of its buffer, a feeder delivers data at the target
/// and parks before decoding it (pullDataQueued), and the mixer decodes it and
/// completes the seek (pullSeekCompleted). The mixer must not go on to the
/// check (pullBufferingCheckEntered); the feeder, released, must, holding
/// sounds_mutex.
void mixerCompletingAPullSeekSkipsTheBufferingCheck()
{
    const char *what = "pull stream seek completed by the mixer";
    std::printf("%s, with the feeder parked before decoding\n", what);

    // As in pullDataDeliveryAgainstDisposal(): raw PCM, no decoder.
    constexpr unsigned int kSampleRate = 44100;
    constexpr unsigned int kChannels = 2;
    unsigned int hash = 0;
    PlayerErrors err = setPullBufferStream(
        &hash, /*bufferSizeBytes*/ 1u << 20, 0.5, kSampleRate, kChannels,
        static_cast<int>(PCM_F32LE), /*audioSizeBytes*/ 64u << 20, nullptr,
        nullptr, nullptr, nullptr);
    EXPECT(err == PlayerErrors::noError && hash != 0,
           "%s: the pull stream should be created (error %d)", what,
           static_cast<int>(err));
    if (err != PlayerErrors::noError || hash == 0)
        return;

    unsigned int voice = 0;
    err = playVariant(PlayVariant::play, hash, &voice);
    EXPECT(err == PlayerErrors::noError && voice != 0,
           "%s: the pull stream should play (error %d)", what,
           static_cast<int>(err));
    if (err != PlayerErrors::noError || voice == 0)
    {
        disposeSound(hash);
        expectNothingLeft(what);
        return;
    }

    // A quarter of a second of silence. The first delivery, from the start,
    // is decoded by the feeder, and the voice plays it.
    const std::vector<float> pcm(kSampleRate / 4 * kChannels, 0.0f);
    const auto *bytes = reinterpret_cast<const unsigned char *>(pcm.data());
    const auto length = static_cast<unsigned int>(pcm.size() * sizeof(float));
    err = addPullBufferDataStream(hash, bytes, length, 0);
    EXPECT(err == PlayerErrors::noError,
           "%s: the first delivery should be added (error %d)", what,
           static_cast<int>(err));

    // Seek well past it. The mixer applies the seek at its next block (or the
    // seeking thread does, if no mixer is running, hence its own thread): the
    // buffer is discarded and the seek stays pending until data at 10 s has
    // been decoded.
    soloud_test::armBarrier(DeviceBarrier::pullSeekPending);
    PlayerErrors seekResult = PlayerErrors::noError;
    std::thread seeker([&] { seekResult = seek(voice, 10.0f); });
    if (!waitBarrierReachedFor(DeviceBarrier::pullSeekPending, kProgressMs))
        abortWedgedRun("the pull stream's seek was never applied");
    soloud_test::releaseBarrier(DeviceBarrier::pullSeekPending);
    seeker.join();
    EXPECT(seekResult == PlayerErrors::noError,
           "%s: the seek should be accepted (error %d)", what,
           static_cast<int>(seekResult));

    // Deliver data at the target. The feeder parks once its bytes are queued,
    // before decoding them, holding sounds_mutex; the mixer decodes them and
    // completes the seek meanwhile.
    soloud_test::armBarrier(DeviceBarrier::pullDataQueued);
    soloud_test::armBarrier(DeviceBarrier::pullSeekCompleted);
    soloud_test::armBarrier(DeviceBarrier::pullBufferingCheckEntered);
    PlayerErrors feedResult = PlayerErrors::noError;
    std::thread feeder(
        [&] { feedResult = addPullBufferDataStream(hash, bytes, length, 0); });
    if (!waitBarrierReachedFor(DeviceBarrier::pullDataQueued, kProgressMs))
        abortWedgedRun("the delivery at the seek target never queued its data");
    if (!waitBarrierReachedFor(DeviceBarrier::pullSeekCompleted, kProgressMs))
        abortWedgedRun("the mixer never completed the seek with the queued data");
    soloud_test::releaseBarrier(DeviceBarrier::pullSeekCompleted);

    // The mixer must not go on to the buffering check. Without the fix it gets
    // there at once and parks; bounded, since with the fix nothing does until
    // the feeder is released.
    const bool mixerChecked =
        waitBarrierReachedFor(DeviceBarrier::pullBufferingCheckEntered, kParkMs);
    EXPECT(!mixerChecked,
           "%s: the mixer ran the stream's buffering check after completing "
           "the seek, on the audio thread and without sounds_mutex",
           what);
    if (mixerChecked)
    {
        soloud_test::releaseBarrier(DeviceBarrier::pullBufferingCheckEntered);
        soloud_test::releaseBarrier(DeviceBarrier::pullDataQueued);
    }
    else
    {
        // The delivery runs the check itself once released, holding
        // sounds_mutex. Reaching the barrier also ends the wait above.
        soloud_test::releaseBarrier(DeviceBarrier::pullDataQueued);
        if (!waitBarrierReachedFor(DeviceBarrier::pullBufferingCheckEntered,
                                   kProgressMs))
            abortWedgedRun("the delivery never ran the stream's buffering check");
        SoundsMutexProbe probe(hash);
        EXPECT(!probe.acquiredWithin(kParkMs),
               "%s: the delivery should run the buffering check holding "
               "sounds_mutex",
               what);
        soloud_test::releaseBarrier(DeviceBarrier::pullBufferingCheckEntered);
    }
    feeder.join();
    EXPECT(feedResult == PlayerErrors::noError,
           "%s: the delivery at the seek target should be added (error %d)",
           what, static_cast<int>(feedResult));

    // The seek took effect: the voice plays on from its target.
    const double position = getPosition(voice);
    EXPECT(position >= 10.0 - 1e-3,
           "%s: the voice should play from the seek target (position %.3f s)",
           what, position);

    disposeSound(hash);
    expectNothingLeft(what);
}

/// A play() of a BufferStream parks holding sounds_mutex while the stream's
/// background decode thread unpauses a voice automatically.
///
/// The decode thread runs checkBuffering() with check_buffer_mutex held, and
/// the play takes check_buffer_mutex in its own checkBuffering() once it goes
/// on, with sounds_mutex still held. So the decode thread's pause change must
/// not take sounds_mutex: if it did, it would wait here for the parked play,
/// and released, the play would wait for it.
///
/// Getting a decode thread to make that call takes a few steps. The stream is
/// AUTO (MP3) and is fed 4 KiB, which it keeps undecoded until more arrives.
/// setDataIsEnded() then decodes a bounded first chunk inline and leaves the
/// rest to the decode thread, which parks (bufferStreamBackgroundDecoded) just
/// before its checkBuffering(). The voice is then paused through the engine
/// (schedulePause() with no delay, which leaves the user-paused flag clear),
/// so that checkBuffering() unpauses it automatically once the stream has
/// ended.
void automaticUnpauseCompletesWhilePlayHoldsTheSounds()
{
    const char *what = "BufferStream decode thread vs a parked play()";
    std::printf("%s\n", what);

    // Its first 4 KiB decode to 25438 samples (44.1 kHz stereo, no ID3 tag):
    // more than one decode chunk of 16384 samples and fewer than two, so
    // setDataIsEnded() decodes one inline and the decode thread the other.
    const std::vector<unsigned char> mp3 =
        readFilePrefix("example/assets/audio/explosion.mp3", 4096);
    EXPECT(mp3.size() == 4096,
           "%s: the MP3 asset should be readable from the repository root "
           "(read %zu bytes)",
           what, mp3.size());
    if (mp3.size() != 4096)
        return;

    unsigned int hash = 0;
    PlayerErrors err = setBufferStream(
        &hash, 10 * 1024 * 1024, /*PRESERVED*/ 0, /*bufferingTimeNeeds*/ 10.0,
        44100, 2, /*AUTO*/ 4, onBuffering, nullptr);
    EXPECT(err == PlayerErrors::noError && hash != 0,
           "%s: setBufferStream should succeed (error %d)", what,
           static_cast<int>(err));
    if (err != PlayerErrors::noError || hash == 0)
        return;

    // Kept undecoded: the stream decodes once it holds more than 4 KiB.
    err = addAudioDataStream(hash, mp3.data(),
                             static_cast<unsigned int>(mp3.size()));
    EXPECT(err == PlayerErrors::noError,
           "%s: addAudioDataStream should succeed (error %d)", what,
           static_cast<int>(err));

    unsigned int first = 0;
    err = play(hash, 0, 0.05f, 0.0f, /*paused*/ false, /*looping*/ false, 0.0,
               0.0, -1, -1, 1.0f, &first);
    EXPECT(err == PlayerErrors::noError && first != 0,
           "%s: the first play should succeed (error %d)", what,
           static_cast<int>(err));
    if (first == 0)
        return;
    EXPECT(getPause(first) != 0,
           "%s: with no data decoded, buffering should pause the first voice",
           what);
    // Slow, so the first chunk outlasts the test once the voice plays.
    setRelativePlaySpeed(first, 0.05f);

    soloud_test::armBarrier(DeviceBarrier::bufferStreamBackgroundDecoded);
    err = setDataIsEnded(hash);
    EXPECT(err == PlayerErrors::noError,
           "%s: setDataIsEnded should succeed (error %d)", what,
           static_cast<int>(err));
    if (!waitBarrierReachedFor(DeviceBarrier::bufferStreamBackgroundDecoded,
                               kProgressMs))
        abortWedgedRun("the stream's background decode thread never decoded "
                       "a chunk; the scenario needs it to");

    // setDataIsEnded() unpaused the voice. Pause it through the engine, which
    // leaves isUserPaused clear, so the decode thread unpauses it again.
    schedulePause(first, 0.0f);
    EXPECT(getPause(first) != 0,
           "%s: schedulePause(0) should pause the first voice", what);

    soloud_test::armBarrier(DeviceBarrier::playSoundLookedUp);
    PlayerErrors secondResult = PlayerErrors::noError;
    unsigned int second = 0;
    std::thread playThread(
        [&]
        {
            secondResult = play(hash, 0, 0.05f, 0.0f, false, false, 0.0, 0.0,
                                -1, -1, 1.0f, &second);
        });
    if (!waitBarrierReachedFor(DeviceBarrier::playSoundLookedUp, kProgressMs))
        abortWedgedRun("the second play() never reached its sound lookup");

    // The play now holds sounds_mutex. Let the decode thread run its
    // checkBuffering().
    clearBufferingEvents();
    soloud_test::releaseBarrier(DeviceBarrier::bufferStreamBackgroundDecoded);

    if (!waitForBufferingEvent(false, first, kProgressMs))
    {
        abortWedgedRun(
            "the decode thread's automatic unpause did not complete while a "
            "play() of the stream held sounds_mutex. It waits for sounds_mutex "
            "with check_buffer_mutex held, and the parked play() takes them "
            "in the other order once released: releasing it would deadlock");
    }
    EXPECT(getPause(first) == 0,
           "%s: the decode thread should have unpaused the first voice", what);

    soloud_test::releaseBarrier(DeviceBarrier::playSoundLookedUp);
    playThread.join();
    EXPECT(secondResult == PlayerErrors::noError && second != 0,
           "%s: the parked play should succeed once released (error %d)", what,
           static_cast<int>(secondResult));

    // Joins the decode thread, so it has finished before the stream goes.
    resetBufferStream(hash);
    disposeSound(hash);
    expectNothingLeft(what);
}

/// One step of lifecycleRequestsWaitForTheSoundsLock(): run [call] on a thread
/// that observes its own lifecycle posts, park it at its first one, and check
/// that sounds_mutex is free at that point.
void expectFirstPostAfterRelease(const char *what, unsigned int hash,
                                 const std::function<void()> &call)
{
    soloud_test::armBarrier(DeviceBarrier::lifecyclePostEntered);
    std::thread caller(
        [&]
        {
            soloud_test::tObserveLifecyclePosts = true;
            call();
        });
    if (!waitBarrierReachedFor(DeviceBarrier::lifecyclePostEntered,
                               kProgressMs))
        abortWedgedRun("the call under test posted no lifecycle request");

    bool free;
    {
        // Bounded: if the caller holds sounds_mutex while parked, the probe
        // gets it only after the release below, and joins then.
        SoundsMutexProbe probe(hash);
        free = probe.acquiredWithin(kParkMs);
        soloud_test::releaseBarrier(DeviceBarrier::lifecyclePostEntered);
    }
    caller.join();

    EXPECT(free,
           "%s: a lifecycle request was posted, about to take "
           "mInterruptionMutex, while sounds_mutex was still held",
           what);
}

/// An automatic pause made inside play() and an automatic unpause made inside
/// addAudioDataStream(), both under sounds_mutex, post their lifecycle request
/// (an idle evaluation, a device start) only once sounds_mutex is released.
void lifecycleRequestsWaitForTheSoundsLock()
{
    const char *what = "lifecycle requests deferred past sounds_mutex";
    std::printf("%s\n", what);

    unsigned int hash = 0;
    PlayerErrors err = setBufferStream(
        &hash, 1024 * 1024, /*PRESERVED*/ 0, /*bufferingTimeNeeds*/ 1.0, 44100,
        2, /*PCM_F32LE*/ 0, onBuffering, nullptr);
    EXPECT(err == PlayerErrors::noError && hash != 0,
           "%s: setBufferStream should succeed (error %d)", what,
           static_cast<int>(err));
    if (err != PlayerErrors::noError || hash == 0)
        return;

    // No data yet: play()'s checkBuffering(0) pauses the new voice for
    // buffering under play()'s lock, which asks for an idle evaluation.
    clearBufferingEvents();
    PlayerErrors playResult = PlayerErrors::noError;
    unsigned int handle = 0;
    char step[128];
    std::snprintf(step, sizeof(step), "%s: automatic pause in play()", what);
    expectFirstPostAfterRelease(step, hash,
                                [&]
                                {
                                    playResult = play(hash, 0, 0.05f, 0.0f,
                                                      false, false, 0.0, 0.0,
                                                      -1, -1, 1.0f, &handle);
                                });
    EXPECT(playResult == PlayerErrors::noError && handle != 0,
           "%s: play should succeed (error %d)", what,
           static_cast<int>(playResult));
    if (handle == 0)
        return;
    EXPECT(getPause(handle) != 0 &&
               waitForBufferingEvent(true, handle, kProgressMs),
           "%s: buffering should have paused the voice", what);

    // Two seconds of silence, more than bufferingTimeNeeds: checkBuffering()
    // unpauses the voice under addAudioDataStream()'s lock, which asks for a
    // device start.
    clearBufferingEvents();
    const std::vector<float> pcm(44100 * 2 * 2, 0.0f);
    PlayerErrors addResult = PlayerErrors::noError;
    std::snprintf(step, sizeof(step),
                  "%s: automatic unpause in addAudioDataStream()", what);
    expectFirstPostAfterRelease(
        step, hash,
        [&]
        {
            addResult = addAudioDataStream(
                hash, reinterpret_cast<const unsigned char *>(pcm.data()),
                static_cast<unsigned int>(pcm.size() * sizeof(float)));
        });
    EXPECT(addResult == PlayerErrors::noError,
           "%s: addAudioDataStream should succeed (error %d)", what,
           static_cast<int>(addResult));
    EXPECT(getPause(handle) == 0 &&
               waitForBufferingEvent(false, handle, kProgressMs),
           "%s: enough data should have unpaused the voice", what);

    disposeSound(hash);
    expectNothingLeft(what);
}

/// Abort a wedged run instead of hanging: a play and a disposal that deadlock
/// are exactly the failure the bounded park exists to rule out. macOS has no
/// `timeout` command.
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
    startHangGuard(120);

    // Every scenario needs an engine. miniaudio opens its null backend on a
    // machine with no audio hardware, so a failure here means a broken
    // environment, not merely a headless one.
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

    for (Disposal disposal : {Disposal::allSounds, Disposal::oneSound})
    {
        for (PlayVariant variant : kPlayVariants)
            raceDisposalAgainstParkedPlay(variant, disposal);
    }

    disposalStopsEveryVoiceFirst(Disposal::oneSound);
    disposalStopsEveryVoiceFirst(Disposal::allSounds);

    pullDataDeliveryAgainstDisposal(Disposal::oneSound);
    pullDataDeliveryAgainstDisposal(Disposal::allSounds);
    mixerCompletingAPullSeekSkipsTheBufferingCheck();

    lifecycleRequestsWaitForTheSoundsLock();
#if defined(__has_feature)
#if __has_feature(thread_sanitizer)
#define PLAY_DISPOSE_RACE_TSAN 1
#endif
#endif
#if defined(__SANITIZE_THREAD__)
#define PLAY_DISPOSE_RACE_TSAN 1
#endif
#if defined(PLAY_DISPOSE_RACE_TSAN)
    // Skipped under ThreadSanitizer: driving a BufferStream's decode thread
    // trips data races BufferStream already has, independent of this test's
    // locking. That thread's checkBuffering() reads the sound's handle list
    // under check_buffer_mutex only, while a play() appends to it under
    // sounds_mutex, and the thread updates mUncompressedBytesReceived and
    // mSampleCount with no lock while getLength() and the mixer's hasEnded()
    // read them. The plain and AddressSanitizer builds run it.
    std::printf("BufferStream decode thread vs a parked play(): skipped under "
                "ThreadSanitizer (pre-existing BufferStream data races)\n");
#else
    automaticUnpauseCompletesWhilePlayHoldsTheSounds();
#endif

    tearDownEngine();

    printSummary();
    return gFailures == 0 ? 0 : 1;
}
