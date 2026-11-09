// Standalone native tests for the waveform synthesizer (Basicwave) and the
// Player calls that create and change it.
//
// Scenarios (pass one name to run only that one):
//
//   params  loadWaveform(waveform, superWave, scale, detune) must build the
//           waveform with that scale and that detune, not with the two
//           swapped.
//
//
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
               "scale should be %f, got %f", c.scale, wave->mSuperwaveScale);
        EXPECT(wave->mSuperwaveDetune == static_cast<double>(c.detune),
               "detune should be %f, got %f", c.detune, wave->mSuperwaveDetune);
        EXPECT(wave->mSuperwave == c.superWave, "superWave should be %d",
               static_cast<int>(c.superWave));
    }

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

    if (gFailures == 0)
        std::printf("\nAll %d assertions passed.\n", gAssertions);
    else
        std::printf("\n%d assertions, %d failures\n", gAssertions, gFailures);
    return gFailures == 0 ? 0 : 1;
}
