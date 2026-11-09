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
//
//
// Build and run from the flutter_soloud repository root with:
//
//   ./test/run_filter_ops_test.sh [--tsan] [scenario]

#include "../src/filters/parametric_eq_filter.h"
#include "../src/filters/pitch_shift_filter.h"

#include "soloud.h"
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

} // namespace

int main(int argc, char **argv) {
    const char *only = argc > 1 ? argv[1] : nullptr;
    auto wanted = [&](const char *name) {
        return only == nullptr || std::strcmp(only, name) == 0;
    };

    if (wanted("eq"))
        testEqWindowChangeOnOneOfTwoVoices();

    if (gFailures == 0)
        std::printf("\nAll %d assertions passed.\n", gAssertions);
    else
        std::printf("\n%d assertions, %d failures\n", gAssertions, gFailures);
    return gFailures == 0 ? 0 : 1;
}
