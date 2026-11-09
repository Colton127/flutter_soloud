// Standalone native tests for the waveform synthesizer (Basicwave) and the
// Player calls that create and change it.
//
// Scenarios (pass one name to run only that one):
//
//   params  loadWaveform(waveform, superWave, scale, detune) must build the
//           waveform with that scale and that detune, not with the two
//           swapped.
//
//   phases  A superwave instance must start its three harmonics at a defined
//           phase. The instance is constructed in storage filled with a NaN
//           bit pattern, so a harmonic phase the constructor leaves
//           uninitialized turns every generated sample into NaN.
//
//   race    The setWaveform*() calls change a waveform that is playing while
//           the audio thread renders it. The parameters the mixer reads must
//           not be written unsynchronized; run under ThreadSanitizer
//           (--tsan), which reports the setters racing
//           BasicwaveInstance::getAudio().
//
// `params` and `race` open the default output device through a Player (the
// waveform plays at a volume of 0.001); `phases` needs no engine.
//
// Build and run from the flutter_soloud repository root with:
//
//   ./test/run_waveform_synth_test.sh [--asan|--tsan] [scenario]

#include "player.h"
#include "synth/basic_wave.h"

#include <atomic>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <new>
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

Basicwave *waveformOf(Player &player, unsigned int hash) {
    ActiveSound *sound = player.findByHash(hash);
    if (sound == nullptr || sound->soundType != TYPE_SYNTH)
        return nullptr;
    return static_cast<Basicwave *>(sound->sound.get());
}

// ---- params -----------------------------------------------------------------

void testLoadWaveformKeepsScaleAndDetune() {
    std::printf("params: loadWaveform builds the waveform with its scale and detune\n");

    Player player;
    if (player.init(44100, 2048, 2) != noError) {
        EXPECT(false, "the player should initialize");
        return;
    }

    struct Case {
        bool superWave;
        float scale;
        float detune;
    };
    const Case cases[] = {{true, 0.0f, 0.25f}, {true, 0.75f, 0.125f}, {false, 0.5f, 0.0f}};
    for (const Case &c : cases) {
        unsigned int hash = 0;
        EXPECT(player.loadWaveform(SoLoud::Soloud::WAVE_SIN, c.superWave, c.scale,
                                   c.detune, hash) == noError,
               "loadWaveform should succeed");
        Basicwave *wave = waveformOf(player, hash);
        EXPECT(wave != nullptr, "the waveform should be registered");
        if (wave == nullptr)
            continue;
        EXPECT(wave->mSuperwaveScale == static_cast<double>(c.scale),
               "scale should be %f, got %f", c.scale, wave->mSuperwaveScale.load());
        EXPECT(wave->mSuperwaveDetune == static_cast<double>(c.detune),
               "detune should be %f, got %f", c.detune, wave->mSuperwaveDetune.load());
        EXPECT(wave->mSuperwave == c.superWave, "superWave should be %d",
               static_cast<int>(c.superWave));
    }

    player.dispose();
}

// ---- phases -----------------------------------------------------------------

void testSuperwaveHarmonicsStartAtADefinedPhase() {
    std::printf("phases: superwave harmonics start at a defined phase\n");

    Basicwave wave(SoLoud::Soloud::WAVE_SIN, true, 0.5, 0.1);

    alignas(BasicwaveInstance) unsigned char storage[sizeof(BasicwaveInstance)];
    // Every double this pattern covers is a NaN.
    std::memset(storage, 0xff, sizeof(storage));
    BasicwaveInstance *instance = new (storage) BasicwaveInstance(&wave);

    constexpr unsigned int kFrames = 256;
    std::vector<float> samples(kFrames);
    instance->getAudio(samples.data(), kFrames, kFrames);

    int nonFinite = 0;
    for (float s : samples)
        if (!std::isfinite(s))
            ++nonFinite;
    EXPECT(nonFinite == 0, "%d of %u superwave samples are not finite", nonFinite,
           kFrames);

    // The same waveform, constructed in zeroed storage, renders the same block:
    // where the harmonics start must not depend on what the memory held.
    alignas(BasicwaveInstance) unsigned char zeroed[sizeof(BasicwaveInstance)];
    std::memset(zeroed, 0, sizeof(zeroed));
    BasicwaveInstance *reference = new (zeroed) BasicwaveInstance(&wave);
    std::vector<float> expected(kFrames);
    reference->getAudio(expected.data(), kFrames, kFrames);
    int differ = 0;
    for (unsigned int i = 0; i < kFrames; ++i)
        if (!(samples[i] == expected[i]))
            ++differ;
    EXPECT(differ == 0, "%d samples depend on the instance's prior memory", differ);

    reference->~BasicwaveInstance();
    instance->~BasicwaveInstance();
}

// ---- race -------------------------------------------------------------------

void testSettersWhileTheWaveformPlays() {
    std::printf("race: setWaveform*() while the waveform plays\n");

    Player player;
    if (player.init(44100, 512, 2) != noError) {
        EXPECT(false, "the player should initialize");
        return;
    }

    unsigned int hash = 0;
    EXPECT(player.loadWaveform(SoLoud::Soloud::WAVE_SIN, true, 0.5f, 0.1f, hash) ==
               noError,
           "loadWaveform should succeed");
    unsigned int handle = 0;
    EXPECT(player.play(hash, handle, 0, 0.001f) == noError && handle != 0,
           "the waveform should play");

    // Let the device start rendering it.
    std::this_thread::sleep_for(std::chrono::milliseconds(200));

    const auto deadline = std::chrono::steady_clock::now() + std::chrono::milliseconds(1500);
    unsigned int changes = 0;
    while (std::chrono::steady_clock::now() < deadline) {
        const float f = static_cast<float>(changes % 100);
        player.setWaveformFreq(hash, 220.0f + f);
        player.setWaveformScale(hash, 0.01f * f);
        player.setWaveformDetune(hash, 0.001f * f);
        player.setWaveformSuperwave(hash, (changes & 1) != 0);
        player.setWaveform(hash, static_cast<int>(changes % 4));
        ++changes;
        std::this_thread::sleep_for(std::chrono::microseconds(200));
    }
    std::printf("  %u rounds of parameter changes\n", changes);
    // A low bound: a sanitizer reporting a race stalls the loop for a while.
    EXPECT(changes > 20, "too few parameter changes: %u", changes);
    EXPECT(player.isValidHandle(handle), "the waveform should still be playing");

    player.dispose();
}

} // namespace

int main(int argc, char **argv) {
    const char *only = argc > 1 ? argv[1] : nullptr;
    auto wanted = [&](const char *name) {
        return only == nullptr || std::strcmp(only, name) == 0;
    };

    if (wanted("params"))
        testLoadWaveformKeepsScaleAndDetune();
    if (wanted("phases"))
        testSuperwaveHarmonicsStartAtADefinedPhase();
    if (wanted("race"))
        testSettersWhileTheWaveformPlays();

    if (gFailures == 0)
        std::printf("\nAll %d assertions passed.\n", gAssertions);
    else
        std::printf("\n%d assertions, %d failures\n", gAssertions, gFailures);
    return gFailures == 0 ? 0 : 1;
}
