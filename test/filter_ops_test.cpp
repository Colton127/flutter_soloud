// Standalone native tests for filter instances and the core filter
// operations, on the null backend (no audio device needed).
//
// Scenarios (pass one name to run only that one):
//
//   eq      Two voices of one sound share a parametric EQ. Changing the STFT
//           window size through one voice's handle must not leave the other
//           voice's live instance processing with buffers sized for the old
//           window. Built with AddressSanitizer (the runner's default), an
//           instance that indexes its old, smaller buffers with the new
//           window size is reported as a heap-buffer-overflow.
//
//   getter  getFilterParameter() on a voice handle races voices that end in
//           the mix and new voices that take their slots. The handle must be
//           resolved under the audio mutex: resolving it first reads the
//           voice table (and the voice's play index) while the mixer can
//           delete that voice. Run under ThreadSanitizer (--tsan), which
//           reports the unsynchronized voice-table read.
//
//   pitch   Pitch shift on a planar buffer whose channel stride is larger
//           than the frame count, as Soloud::mix() passes for block sizes
//           that are not a multiple of 16. The filter must address channel
//           c at c * stride: the samples must match the same input processed
//           in a packed buffer, and the padding between channels must stay
//           untouched.
//
// Build and run from the flutter_soloud repository root with:
//
//   ./test/run_filter_ops_test.sh [--tsan] [scenario]
//
//   compressor  A new compressor instance (one is made for every voice that
//           plays with the filter) fed a steady signal well below its
//           threshold must leave it untouched from the first sample. Its
//           level detector has to start from silence: one that starts at
//           full scale ducks the start of every voice until it releases.

#include "../src/filters/compressor.h"
#include "../src/filters/parametric_eq_filter.h"
#include "../src/filters/pitch_shift_filter.h"

#include "soloud.h"
#include "soloud_fader.h"
#include "soloud_biquadresonantfilter.h"

#include <atomic>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <thread>
#include <vector>

namespace {

constexpr unsigned int kSampleRate = 44100;
constexpr unsigned int kBuffer = 512;
constexpr unsigned int kChannels = 2;

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

// A stereo tone. With a frame limit the voice ends after that many frames,
// which the mixer notices and stops (deleting the voice instance) inside
// the mix block.
class ToneSource;

class ToneInstance final : public SoLoud::AudioSourceInstance {
public:
    explicit ToneInstance(ToneSource *parent) : parent_(parent) {}
    unsigned int getAudio(float *buffer, unsigned int samples,
                          unsigned int bufferSize) override;
    bool hasEnded() override;

private:
    ToneSource *parent_;
    unsigned int offset_ = 0;
};

class ToneSource final : public SoLoud::AudioSource {
public:
    explicit ToneSource(unsigned int frames = 0) : frames(frames) {
        mBaseSamplerate = static_cast<float>(kSampleRate);
        mChannels = kChannels;
    }
    SoLoud::AudioSourceInstance *createInstance() override {
        return new ToneInstance(this);
    }
    const unsigned int frames;
};

unsigned int ToneInstance::getAudio(float *buffer, unsigned int samples,
                                    unsigned int bufferSize) {
    unsigned int count = samples;
    if (parent_->frames != 0)
        count = std::min(samples, parent_->frames - std::min(offset_, parent_->frames));
    for (unsigned int ch = 0; ch < mChannels; ++ch)
        for (unsigned int i = 0; i < count; ++i)
            buffer[ch * bufferSize + i] =
                0.25f * std::sin(0.05f * static_cast<float>(offset_ + i) +
                                 static_cast<float>(ch));
    offset_ += count;
    return count;
}

bool ToneInstance::hasEnded() {
    return parent_->frames != 0 && offset_ >= parent_->frames;
}

bool initEngine(SoLoud::Soloud &soloud) {
    const SoLoud::result r = soloud.init(SoLoud::Soloud::CLIP_ROUNDOFF,
                                         SoLoud::Soloud::NULLDRIVER,
                                         kSampleRate, kBuffer, kChannels);
    EXPECT(r == SoLoud::SO_NO_ERROR, "soloud.init failed: %u", r);
    return r == SoLoud::SO_NO_ERROR;
}

bool allFinite(const std::vector<float> &v) {
    for (float f : v)
        if (!std::isfinite(f))
            return false;
    return true;
}

// ---- eq ---------------------------------------------------------------------

void testEqWindowChangeOnOneOfTwoVoices() {
    std::printf("eq: window size changed through one of two voices sharing the EQ\n");

    SoLoud::Soloud soloud;
    if (!initEngine(soloud))
        return;

    ToneSource tone;
    ParametricEq eq(&soloud);
    tone.setFilter(0, &eq);

    const SoLoud::handle first = soloud.play(tone);
    const SoLoud::handle second = soloud.play(tone);
    EXPECT(first != 0 && second != 0, "the two voices failed to play");

    std::vector<float> out(kBuffer * kChannels);
    for (int i = 0; i < 4; ++i)
        soloud.mix(out.data(), kBuffer);

    // What FilterParam.value does for `stftWindowSize` on a single sound's
    // handle: Filters::setFilterParams -> Soloud::setFilterParameter(handle).
    soloud.setFilterParameter(first, 0, 1, 4096.0f);

    // Both voices keep rendering their live EQ instances.
    for (int i = 0; i < 8; ++i)
        soloud.mix(out.data(), kBuffer);
    EXPECT(allFinite(out), "the mix produced non-finite samples");

    soloud.stopAll();
    soloud.mix(out.data(), kBuffer);
    tone.setFilter(0, nullptr);
    soloud.deinit();
}

// ---- getter -----------------------------------------------------------------

void testFilterGetterRacesVoiceEnd() {
    std::printf("getter: getFilterParameter racing voices that end in the mix\n");

    SoLoud::Soloud soloud;
    if (!initEngine(soloud))
        return;

    // Shorter than one block: every voice ends, and is deleted, in the first
    // mix block that renders it.
    ToneSource blip(64);
    SoLoud::BiquadResonantFilter biquad;
    blip.setFilter(0, &biquad);

    std::atomic<bool> stop{false};
    std::atomic<SoLoud::handle> latest{0};
    std::atomic<unsigned long> reads{0};
    std::atomic<unsigned long> plays{0};

    std::thread mixer([&] {
        std::vector<float> out(256 * kChannels);
        while (!stop.load())
            soloud.mix(out.data(), 256);
    });
    std::thread player([&] {
        while (!stop.load()) {
            const SoLoud::handle h = soloud.play(blip);
            if (h != 0) {
                latest.store(h);
                plays.fetch_add(1);
            }
            std::this_thread::yield();
        }
    });
    std::thread getter([&] {
        while (!stop.load()) {
            const SoLoud::handle h = latest.load();
            if (h != 0) {
                (void)soloud.getFilterParameter(h, 0, 0);
                reads.fetch_add(1);
            }
        }
    });

    std::this_thread::sleep_for(std::chrono::milliseconds(1500));
    stop.store(true);
    getter.join();
    player.join();
    mixer.join();

    std::printf("  %lu plays, %lu filter reads\n", plays.load(), reads.load());
    EXPECT(plays.load() > 100, "too few voices were played to race: %lu",
           plays.load());
    EXPECT(reads.load() > 100, "too few filter reads: %lu", reads.load());

    soloud.stopAll();
    std::vector<float> out(kBuffer * kChannels);
    soloud.mix(out.data(), kBuffer);
    blip.setFilter(0, nullptr);
    soloud.deinit();
}

// ---- pitch ------------------------------------------------------------------

void testPitchShiftUsesChannelStride() {
    std::printf("pitch: pitch shift on a padded planar buffer\n");

    constexpr unsigned int kFrames = 129;  // Soloud::mix(…, 129)
    constexpr unsigned int kStride = 144;  // (129 + 15) & ~15
    constexpr float kPad = 7.0f;

    PitchShift parent;
    SoLoud::FilterInstance *padded = parent.createInstance();
    SoLoud::FilterInstance *packed = parent.createInstance();
    padded->setFilterParameter(PitchShift::SHIFT, 1.5f);
    packed->setFilterParameter(PitchShift::SHIFT, 1.5f);

    std::vector<float> a(kStride * kChannels);
    std::vector<float> b(kFrames * kChannels);
    unsigned int frame = 0;
    int paddingChanged = 0;
    int samplesDiffer = 0;
    int rightDiffer = 0;
    float maxOut = 0.0f;

    for (int block = 0; block < 64; ++block) {
        std::fill(a.begin(), a.end(), kPad);
        for (unsigned int ch = 0; ch < kChannels; ++ch)
            for (unsigned int i = 0; i < kFrames; ++i) {
                const float s = 0.25f * std::sin(0.03f * static_cast<float>(frame + i) *
                                                 static_cast<float>(ch + 1));
                a[ch * kStride + i] = s;
                b[ch * kFrames + i] = s;
            }
        frame += kFrames;

        padded->filter(a.data(), kFrames, kStride, kChannels,
                       static_cast<float>(kSampleRate), 0.0);
        packed->filter(b.data(), kFrames, kFrames, kChannels,
                       static_cast<float>(kSampleRate), 0.0);

        for (unsigned int ch = 0; ch < kChannels; ++ch) {
            for (unsigned int i = kFrames; i < kStride; ++i)
                if (a[ch * kStride + i] != kPad)
                    ++paddingChanged;
            for (unsigned int i = 0; i < kFrames; ++i) {
                maxOut = std::max(maxOut, std::fabs(b[ch * kFrames + i]));
                if (std::fabs(a[ch * kStride + i] - b[ch * kFrames + i]) > 1e-6f) {
                    ++samplesDiffer;
                    if (ch == 1)
                        ++rightDiffer;
                }
            }
        }
    }

    EXPECT(maxOut > 0.01f, "the pitch shifter produced no output to compare (max %f)",
           maxOut);
    EXPECT(paddingChanged == 0,
           "%d padding samples between the channels were overwritten", paddingChanged);
    EXPECT(samplesDiffer == 0,
           "%d samples (%d right-channel) differ from the packed-buffer result",
           samplesDiffer, rightDiffer);

    delete padded;
    delete packed;
}

void testCompressorStartsFromSilence() {
    std::printf("compressor: a signal below the threshold passes untouched from the start\n");
    constexpr unsigned int kFrames = 512;
    constexpr float kLevel = 0.1f; // -20 dBFS, below the default -6 dB threshold

    Compressor parent(kSampleRate);
    SoLoud::FilterInstance *instance = parent.createInstance();

    std::vector<float> buffer(kFrames * kChannels);
    int changed = 0;
    float firstGain = 1.0f;
    for (int block = 0; block < 16; ++block) {
        for (unsigned int ch = 0; ch < kChannels; ++ch)
            for (unsigned int i = 0; i < kFrames; ++i)
                buffer[ch * kFrames + i] = (i % 2 == 0) ? kLevel : -kLevel;
        instance->filter(buffer.data(), kFrames, kFrames, kChannels,
                         static_cast<float>(kSampleRate), 0.0);
        if (block == 0)
            firstGain = std::fabs(buffer[0]) / kLevel;
        for (float s : buffer)
            if (std::fabs(std::fabs(s) - kLevel) > 1e-4f)
                ++changed;
    }

    EXPECT(changed == 0,
           "%d of %u samples changed; the first came out at %.3f of its level",
           changed, 16 * kFrames * kChannels, firstGain);
    delete instance;
}

void testCompressorTimingFollowsFadersAndRate() {
    constexpr unsigned int frames = 4096;
    for (unsigned int attribute : {Compressor::ATTACK_TIME, Compressor::RELEASE_TIME}) {
        Compressor parent(44100);
        auto *moving = parent.createInstance();
        auto *reference = parent.createInstance();
        for (auto *instance : {moving, reference}) {
            instance->setFilterParameter(Compressor::THRESHOLD, -30);
            instance->setFilterParameter(Compressor::RATIO, 10);
            instance->setFilterParameter(attribute, 1);
        }
        std::vector<float> a(frames, 0.9f), b = a;
        if (attribute == Compressor::ATTACK_TIME) a.assign(frames, 0.01f);
        b = a;
        moving->filter(a.data(), frames, frames, 1, 44100, 0);
        reference->filter(b.data(), frames, frames, 1, 44100, 0);
        moving->mParamFader[attribute].set(1, 80, 0.1, 0);
        reference->setFilterParameter(attribute, 80);
        a.assign(frames, attribute == Compressor::ATTACK_TIME ? 0.9f : 0.01f);
        b = a;
        moving->filter(a.data(), frames, frames, 1, 44100, 0.2);
        reference->filter(b.data(), frames, frames, 1, 44100, 0.2);
        float difference = 0;
        for (unsigned int i = 0; i < frames; ++i) difference = std::max(difference, std::fabs(a[i] - b[i]));
        EXPECT(difference < 1e-6f, "compressor: fader timing differs for parameter %u (%g)", attribute, difference);
        delete moving; delete reference;
    }
    Compressor oldRate(44100), actualRate(48000);
    auto *aInstance = oldRate.createInstance();
    auto *bInstance = actualRate.createInstance();
    std::vector<float> a(frames, 0.9f), b = a;
    aInstance->filter(a.data(), frames, frames, 1, 48000, 0);
    bInstance->filter(b.data(), frames, frames, 1, 48000, 0);
    float difference = 0;
    for (unsigned int i = 0; i < frames; ++i) difference = std::max(difference, std::fabs(a[i] - b[i]));
    EXPECT(difference < 1e-6f, "compressor: construction rate changes block timing (%g)", difference);
    delete aInstance; delete bInstance;
}

} // namespace

int main(int argc, char **argv) {
    const char *only = argc > 1 ? argv[1] : nullptr;
    auto wanted = [&](const char *name) {
        return only == nullptr || std::strcmp(only, name) == 0;
    };

    if (wanted("eq"))
        testEqWindowChangeOnOneOfTwoVoices();
    if (wanted("getter"))
        testFilterGetterRacesVoiceEnd();
    if (wanted("pitch"))
        testPitchShiftUsesChannelStride();
    if (wanted("compressor")) {
        testCompressorStartsFromSilence();
        testCompressorTimingFollowsFadersAndRate();
    }

    if (gFailures == 0)
        std::printf("\nAll %d assertions passed.\n", gAssertions);
    else
        std::printf("\n%d assertions, %d failures\n", gAssertions, gFailures);
    return gFailures == 0 ? 0 : 1;
}
