// Standalone native test for the Player's mixing-bus table (busMap) used from
// several threads at once, as the Dart API allows: buses are created and
// destroyed on one isolate while another plays sounds into them or queries
// them (SoLoudIsolate calls from a worker isolate run on its own thread).
//
// createBus() and destroyBus() insert into and erase from the std::map, and
// play() and the bus queries look buses up in it and then use the BusData they
// found. Unsynchronized, a lookup racing an insertion or an erase is undefined
// behavior, and a play that found a bus just before destroyBus() erased it
// plays into a destroyed Bus. Under ThreadSanitizer (--tsan) the
// unsynchronized map accesses are reported; under AddressSanitizer (--asan) a
// use of an erased BusData is reported when the timing lands on it.
//
// The test opens the default output device through a Player (miniaudio falls
// back to its null backend without one); the sound plays at a volume of 0.001.
//
// Build and run from the flutter_soloud repository root with:
//
//   ./test/run_bus_map_race_test.sh [--asan|--tsan]

#include "player.h"

#include <atomic>
#include <chrono>
#include <cstdio>
#include <thread>

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

void testBusesCreatedAndDestroyedWhilePlayedInto() {
    std::printf("buses created and destroyed while another thread plays into them\n");

    Player player;
    if (player.init(44100, 512, 2) != noError) {
        EXPECT(false, "the player should initialize");
        return;
    }

    unsigned int hash = 0;
    EXPECT(player.loadWaveform(SoLoud::Soloud::WAVE_SIN, false, 0.0f, 0.0f, hash) ==
               noError,
           "loadWaveform should succeed");

    std::atomic<bool> stop{false};
    std::atomic<unsigned int> latestBus{0};
    std::atomic<unsigned long> created{0};
    std::atomic<unsigned long> playedIntoBus{0};
    std::atomic<unsigned long> queries{0};

    // One isolate: creates a bus, starts it, and destroys it again.
    std::thread owner([&] {
        while (!stop.load()) {
            const unsigned int id = player.createBus();
            unsigned int busHandle = 0;
            player.busPlayOnEngine(id, 1.0f, false, busHandle);
            latestBus.store(id);
            created.fetch_add(1);
            std::this_thread::sleep_for(std::chrono::microseconds(300));
            player.destroyBus(id);
        }
    });

    // Another isolate: plays the sound into whichever bus is current, and
    // queries it.
    std::thread user([&] {
        while (!stop.load()) {
            const unsigned int id = latestBus.load();
            if (id == 0)
                continue;
            unsigned int handle = 0;
            if (player.play(hash, handle, id, 0.001f) == noError && handle != 0) {
                playedIntoBus.fetch_add(1);
                player.stop(handle);
            }
            (void)player.busGetActiveVoiceCount(id);
            (void)player.busIsVisualizationEnabled(id);
            queries.fetch_add(1);
        }
    });

    std::this_thread::sleep_for(std::chrono::milliseconds(1500));
    stop.store(true);
    user.join();
    owner.join();

    std::printf("  %lu buses, %lu plays into a bus, %lu query rounds\n",
                created.load(), playedIntoBus.load(), queries.load());
    EXPECT(created.load() > 100, "too few buses created to race: %lu", created.load());
    EXPECT(playedIntoBus.load() > 100, "too few plays into a bus: %lu",
           playedIntoBus.load());

    player.dispose();
}

} // namespace

int main() {
    testBusesCreatedAndDestroyedWhilePlayedInto();

    if (gFailures == 0)
        std::printf("\nAll %d assertions passed.\n", gAssertions);
    else
        std::printf("\n%d assertions, %d failures\n", gAssertions, gFailures);
    return gFailures == 0 ? 0 : 1;
}
