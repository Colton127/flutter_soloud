// Standalone native test for countAudioSource(): the number of voices that
// play a given sound.
//
// For every kind of sound the Player loads -- a sound decoded into memory, a
// sound streamed from its encoded bytes, a waveform, a buffer stream and a
// pull buffer stream -- the count is 0 before any voice plays it, follows
// voices as they are played and stopped, and is 0 again once the sound is
// disposed. A hash that names no sound counts 0.
//
// Build and run from the flutter_soloud repository root with:
//
//   ./test/run_count_audio_source_test.sh            (plain)
//   ./test/run_count_audio_source_test.sh --asan     (AddressSanitizer)
//   ./test/run_count_audio_source_test.sh --tsan     (ThreadSanitizer)

#include "bindings.h"
#include "enums.h"

#include <algorithm>
#include <chrono>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <fstream>
#include <thread>
#include <vector>

extern "C"
{
    // Test-only export, compiled in with SOLOUD_LIFECYCLE_TEST_HOOKS.
    int soloudTestSoundCount();
}

namespace
{

constexpr int64_t kEngineId = 4004;
constexpr unsigned int kSampleRate = 44100;
constexpr unsigned int kChannels = 2;

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

void printSummary()
{
    std::printf("\n%d assertions, %d failures\n", gAssertions, gFailures);
    std::fflush(stdout);
    std::fflush(stderr);
}

/// Abort a wedged run instead of hanging. macOS has no `timeout` command.
void startHangGuard(int limitSeconds)
{
    std::thread(
        [limitSeconds]
        {
            std::this_thread::sleep_for(std::chrono::seconds(limitSeconds));
            std::fprintf(stderr,
                         "\n  FAIL: hang guard fired after %ds -- the suite is "
                         "wedged.\n",
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
    setAudioDeviceIdleTimeout(600000);
    prepareEngineInit(kEngineId);
    const PlayerErrors err =
        initEngine(-1, kSampleRate, 2048, kChannels, 1, 0, 0);
    if (err != PlayerErrors::noError)
        return false;
    setDartEventCallback(onVoiceEnded, onFileLoaded, onStateChanged, kEngineId);
    return isInited() != 0;
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
// The sounds
// ---------------------------------------------------------------------------

enum class Kind
{
    decodedIntoMemory,
    streamedFromItsBytes,
    waveform,
    bufferStream,
    pullBufferStream,
};

const char *kindName(Kind kind)
{
    switch (kind)
    {
    case Kind::decodedIntoMemory:
        return "a sound decoded into memory";
    case Kind::streamedFromItsBytes:
        return "a sound streamed from its bytes";
    case Kind::waveform:
        return "a waveform";
    case Kind::bufferStream:
        return "a buffer stream";
    case Kind::pullBufferStream:
        return "a pull buffer stream";
    }
    return "?";
}

/// Load a sound of [kind] that plays for well over the test. 0 on failure.
unsigned int load(Kind kind, const std::vector<unsigned char> &mp3)
{
    unsigned int hash = 0;
    // Two seconds of quiet raw PCM, for the streams.
    const std::vector<float> pcm(kSampleRate * kChannels * 2, 0.001f);
    const auto *pcmBytes = reinterpret_cast<const unsigned char *>(pcm.data());
    const auto pcmSize = static_cast<unsigned int>(pcm.size() * sizeof(float));

    PlayerErrors err = PlayerErrors::unknownError;
    switch (kind)
    {
    case Kind::decodedIntoMemory:
    {
        // Decoded from a copy: the bytes stay the caller's.
        std::vector<unsigned char> bytes = mp3;
        char name[] = "count_audio_source_decoded.mp3";
        err = loadMem(name, bytes.data(), static_cast<int>(bytes.size()), 1,
                      &hash);
        break;
    }
    case Kind::streamedFromItsBytes:
    {
        // Streamed from the bytes themselves, which the stream takes over and
        // frees with delete[].
        auto *bytes = new unsigned char[mp3.size()];
        std::copy(mp3.begin(), mp3.end(), bytes);
        char name[] = "count_audio_source_streamed.mp3";
        err = loadMem(name, bytes, static_cast<int>(mp3.size()), 0, &hash);
        break;
    }
    case Kind::waveform:
        err = loadWaveform(0, false, 1.0f, 1.0f, &hash);
        break;
    case Kind::bufferStream:
        err = setBufferStream(&hash, 4 * 1024 * 1024, /*PRESERVED*/ 0,
                              /*bufferingTimeNeeds*/ 0.0, kSampleRate,
                              kChannels, /*PCM_F32LE*/ 0, nullptr, nullptr);
        if (err == PlayerErrors::noError)
            err = addAudioDataStream(hash, pcmBytes, pcmSize);
        break;
    case Kind::pullBufferStream:
        err = setPullBufferStream(&hash, 4 * 1024 * 1024, 0.5, kSampleRate,
                                  kChannels, /*PCM_F32LE*/ 0, pcmSize, nullptr,
                                  nullptr, nullptr, nullptr);
        if (err == PlayerErrors::noError)
            err = addPullBufferDataStream(hash, pcmBytes, pcmSize, 0);
        break;
    }
    if (err != PlayerErrors::noError && hash != 0)
    {
        disposeSound(hash);
        hash = 0;
    }
    return err == PlayerErrors::noError ? hash : 0;
}

unsigned int playLooping(unsigned int hash)
{
    unsigned int voice = 0;
    const PlayerErrors err = play(hash, 0, 0.05f, 0.0f, /*paused*/ false,
                                  /*looping*/ true, 0.0, 0.0, -1, -1, 1.0f,
                                  &voice);
    return err == PlayerErrors::noError ? voice : 0;
}

// ---------------------------------------------------------------------------
// Scenario
// ---------------------------------------------------------------------------

/// The count of a sound of [kind] follows its voices: 0, then 2 once two
/// voices play it, 1 once one is stopped, and 0 once it is disposed.
void countFollowsTheVoices(Kind kind, const std::vector<unsigned char> &mp3)
{
    const char *what = kindName(kind);
    std::printf("%s\n", what);

    const unsigned int hash = load(kind, mp3);
    EXPECT(hash != 0, "%s: should load", what);
    if (hash == 0)
        return;

    EXPECT(countAudioSource(hash) == 0,
           "%s: no voice plays it yet, but it counts %d", what,
           countAudioSource(hash));

    const unsigned int first = playLooping(hash);
    const unsigned int second = playLooping(hash);
    EXPECT(first != 0 && second != 0, "%s: should play twice", what);
    EXPECT(countAudioSource(hash) == 2,
           "%s: two voices play it, but it counts %d", what,
           countAudioSource(hash));

    stop(first);
    EXPECT(countAudioSource(hash) == 1,
           "%s: one voice plays it after the other was stopped, but it "
           "counts %d",
           what, countAudioSource(hash));

    disposeSound(hash);
    EXPECT(countAudioSource(hash) == 0,
           "%s: disposed, it should count 0, not %d", what,
           countAudioSource(hash));
}

} // namespace

int main()
{
    startHangGuard(120);

    // miniaudio opens its null backend on a machine with no audio hardware,
    // so a failure here means a broken environment, not merely a headless
    // one.
    if (!bringUpEngine())
    {
        if (isInited())
            dispose();
        std::fprintf(stderr,
                     "FATAL: no output device could be opened, not even a null "
                     "backend; the test cannot run.\n"
                     "Set SOLOUD_LIFECYCLE_TEST_ALLOW_NO_DEVICE=1 to downgrade "
                     "this to a skip.\n");
        return std::getenv("SOLOUD_LIFECYCLE_TEST_ALLOW_NO_DEVICE") ? 0 : 1;
    }

    const std::vector<unsigned char> mp3 =
        readFile("example/assets/audio/explosion.mp3");
    EXPECT(!mp3.empty(),
           "the MP3 asset should be readable from the repository root");

    for (Kind kind : {Kind::decodedIntoMemory, Kind::streamedFromItsBytes,
                      Kind::waveform, Kind::bufferStream,
                      Kind::pullBufferStream})
    {
        if (mp3.empty() && (kind == Kind::decodedIntoMemory ||
                            kind == Kind::streamedFromItsBytes))
            continue;
        countFollowsTheVoices(kind, mp3);
    }

    std::printf("a hash that names no sound\n");
    EXPECT(countAudioSource(0x12345) == 0,
           "a hash that names no sound should count 0, not %d",
           countAudioSource(0x12345));

    EXPECT(getVoiceCount() == 0, "%u voice(s) still alive", getVoiceCount());
    EXPECT(soloudTestSoundCount() == 0, "%d sound(s) still loaded",
           soloudTestSoundCount());

    dispose();

    printSummary();
    return gFailures == 0 ? 0 : 1;
}
