// Standalone native tests for a mixing bus in an engine that doesn't run at
// 44.1 kHz (a flutter_soloud local patch in soloud_bus.cpp: a bus mixes at
// the engine's rate).
//
// A bus is an AudioSource, whose rate SoLoud leaves at 44.1 kHz. Played on a
// 48 kHz engine it used to mix its voices at 44.1 kHz and be resampled back
// to 48 kHz, so:
//
//   * a 48 kHz voice through it was resampled twice, into the bus and out of
//     it, where straight to the output it plays as it is;
//   * a voice's delay, counted in the engine's samples (playScheduled,
//     setDelaySamples), was counted out in the bus's: it started 48/44.1 late.
//
// Each test plays the same voice straight to the output and through a bus,
// each on an engine of its own, which runs on SoLoud's null driver and is
// mixed by hand.
//
// Build and run from the flutter_soloud repository root with:
//
//   ./test/run_bus_engine_rate_test.sh

#include "soloud.h"
#include "soloud_bus.h"

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <vector>

namespace {

constexpr unsigned int kSampleRate = 48000;
constexpr unsigned int kBlockFrames = 512;
constexpr unsigned int kChannels = 2;

/// How many frames after a voice straight to the output the same voice
/// through a bus is heard. At a step of 1, SoLoud's linear resampler reads
/// each frame from the one before it (at fraction 0), so it plays a voice a
/// frame late; through a bus that happens twice, into the bus and out of it.
constexpr long long kBusLag = 1;

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

/// A SoLoud engine at kSampleRate on the null driver, mixed by hand. Declare
/// it before any source or bus: those stop themselves through it when
/// destroyed.
struct Engine {
    SoLoud::Soloud soloud;
    bool ok = false;

    Engine() {
        ok = soloud.init(0, SoLoud::Soloud::NULLDRIVER, kSampleRate,
                         kBlockFrames, kChannels) == SoLoud::SO_NO_ERROR;
        EXPECT(ok, "soloud.init failed");
    }
    ~Engine() { soloud.deinit(); }

    /// The left channel of the next [frames] frames the engine mixes:
    /// silence if it couldn't start.
    std::vector<float> mix(unsigned int frames) {
        std::vector<float> left;
        if (!ok) return std::vector<float>(frames, 0.0f);
        std::vector<float> block(kBlockFrames * kChannels);
        while (left.size() < frames) {
            soloud.mix(block.data(), kBlockFrames);
            for (unsigned int i = 0; i < kBlockFrames; i++) left.push_back(block[i * kChannels]);
        }
        left.resize(frames);
        return left;
    }
};

/// An engine whose voices play straight to its output, or through a bus
/// played on it at full level in both channels, as flutter_soloud plays one
/// (Player::busPlayOnEngine). Declare a voice's source after it.
struct Route {
    Engine engine;
    SoLoud::Bus bus;
    const bool throughBus;

    explicit Route(bool aThroughBus) : throughBus(aThroughBus) {
        if (!throughBus || !engine.ok) return;
        const SoLoud::handle handle = engine.soloud.play(bus);
        engine.soloud.setPanAbsolute(handle, 1.0f, 1.0f);
    }

    /// Plays [source] at full level in both channels.
    SoLoud::handle play(SoLoud::AudioSource &source, bool paused = false) {
        return panned(throughBus ? bus.play(source, 1.0f, 0.0f, paused)
                                 : engine.soloud.play(source, 1.0f, 0.0f, paused));
    }

    /// Starts [source] at engine time [at], at full level in both channels.
    SoLoud::handle playScheduled(SoLoud::time at, SoLoud::AudioSource &source) {
        return panned(throughBus ? bus.playScheduled(at, source)
                                 : engine.soloud.playScheduled(at, source));
    }

private:
    SoLoud::handle panned(SoLoud::handle voice) {
        EXPECT(voice != 0, "the voice should play");
        engine.soloud.setPanAbsolute(voice, 1.0f, 1.0f);
        return voice;
    }
};

/// The first of [left]'s frames heard, or -1.
long long firstHeard(const std::vector<float> &left) {
    for (size_t i = 0; i < left.size(); i++) {
        if (std::fabs(left[i]) > 1e-4f) return static_cast<long long>(i);
    }
    return -1;
}

class NoiseInstance : public SoLoud::AudioSourceInstance {
    unsigned int mState = 12345;

public:
    unsigned int getAudio(float *aBuffer, unsigned int aSamplesToRead,
                          unsigned int aBufferSize) override {
        for (unsigned int i = 0; i < aSamplesToRead; i++) {
            mState = mState * 1664525u + 1013904223u;
            const float sample = (static_cast<float>(mState >> 8) / 16777216.0f - 0.5f) * 0.5f;
            for (unsigned int c = 0; c < mChannels; c++) aBuffer[c * aBufferSize + i] = sample;
        }
        return aSamplesToRead;
    }
    bool hasEnded() override { return false; }
};

/// Seeded white noise at kSampleRate, the same from every voice: all of the
/// band, where resampling shows.
class NoiseSource : public SoLoud::AudioSource {
public:
    NoiseSource() {
        mChannels = kChannels;
        mBaseSamplerate = kSampleRate;
    }
    SoLoud::AudioSourceInstance *createInstance() override { return new NoiseInstance(); }
};

class ConstantInstance : public SoLoud::AudioSourceInstance {
public:
    unsigned int getAudio(float *aBuffer, unsigned int aSamplesToRead,
                          unsigned int aBufferSize) override {
        for (unsigned int c = 0; c < mChannels; c++) {
            for (unsigned int i = 0; i < aSamplesToRead; i++) aBuffer[c * aBufferSize + i] = 0.5f;
        }
        return aSamplesToRead;
    }
    bool hasEnded() override { return false; }
};

/// Plays 0.5 on every channel at kSampleRate, forever: heard from its first
/// sample.
class ConstantSource : public SoLoud::AudioSource {
public:
    ConstantSource() {
        mChannels = kChannels;
        mBaseSamplerate = kSampleRate;
    }
    SoLoud::AudioSourceInstance *createInstance() override { return new ConstantInstance(); }
};

/// The first [frames] frames of noise played straight or through a bus.
std::vector<float> noise(bool throughBus, unsigned int frames) {
    Route route(throughBus);
    NoiseSource source;
    route.play(source);
    return route.engine.mix(frames);
}

/// Where a voice scheduled [ahead] seconds from now is first heard, straight
/// or through a bus.
long long scheduledStart(bool throughBus, SoLoud::time ahead) {
    Route route(throughBus);
    ConstantSource source;
    route.engine.mix(2 * kBlockFrames);
    // The wait counts from the first sample of the next block mixed, which
    // is at the engine's time now.
    route.playScheduled(route.engine.soloud.getEngineTime() + ahead, source);
    return firstHeard(route.engine.mix(kSampleRate / 2));
}

/// Where a voice delayed by [delay] of the engine's samples is first heard,
/// straight or through a bus.
long long delayedStart(bool throughBus, unsigned int delay) {
    Route route(throughBus);
    ConstantSource source;
    const SoLoud::handle voice = route.play(source, /*paused=*/true);
    route.engine.soloud.setDelaySamples(voice, delay);
    route.engine.soloud.setPause(voice, false);
    return firstHeard(route.engine.mix(kSampleRate / 4));
}

void testVoiceThroughBusPlaysAsItIs() {
    std::printf("a 48 kHz voice through a bus on a 48 kHz engine comes out as it does straight\n");
    constexpr unsigned int kFrames = 8 * kBlockFrames;
    const std::vector<float> straight = noise(false, kFrames);
    const std::vector<float> through = noise(true, kFrames);

    // From the third block: SoLoud starts a voice at the pan it was played at
    // (0, so 0.707 in each channel) and ramps it to (1, 1) over the first
    // block it mixes, and through the bus two voices ramp, the bus's and the
    // noise's.
    float worst = 0.0f;
    for (unsigned int i = 2 * kBlockFrames; i + kBusLag < kFrames; i++) {
        worst = std::max(worst, std::fabs(straight[i] - through[i + kBusLag]));
    }
    EXPECT(firstHeard(straight) >= 0, "the voice should be heard straight");
    EXPECT(worst < 1e-6f, "through the bus it should be the same samples, %lld frame later (differs by up to %g)",
           kBusLag, worst);
}

void testScheduledVoiceStartsOnItsSample() {
    std::printf("a voice scheduled through a bus starts on its sample\n");
    const long long straight = scheduledStart(false, 0.25);
    const long long through = scheduledStart(true, 0.25);
    EXPECT(straight >= 0, "the voice should be heard straight");
    EXPECT(through == straight + kBusLag, "heard from frame %lld through the bus, not %lld (%lld straight)",
           through, straight + kBusLag, straight);
}

void testDelaySamplesAreTheEngines() {
    std::printf("a voice's delay in a bus counts the engine's samples\n");
    constexpr unsigned int kDelay = 4800; // 100 ms
    const long long straight = delayedStart(false, kDelay);
    const long long through = delayedStart(true, kDelay);
    EXPECT(straight >= kDelay, "the voice should be heard straight after its delay, not from frame %lld", straight);
    EXPECT(through == straight + kBusLag, "heard from frame %lld through the bus, not %lld (%lld straight)",
           through, straight + kBusLag, straight);
}

} // namespace

int main() {
    testVoiceThroughBusPlaysAsItIs();
    testScheduledVoiceStartsOnItsSample();
    testDelaySamplesAreTheEngines();

    std::printf("\n%d assertions, %d failures\n", gAssertions, gFailures);
    return gFailures == 0 ? 0 : 1;
}
