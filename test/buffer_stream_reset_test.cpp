#include "bindings.h"
#include "audiobuffer/buffer.h"
#include "soloud.h"
#include <cmath>
#include <cstdio>
#include <cstring>
#include <vector>

extern "C" void soloudTestMixPlayerFrames(float *, unsigned int);
extern "C" float soloudTestVoiceSampleRate(unsigned int);

static int failures = 0;
#define EXPECT(c, message) do { if (!(c)) { ++failures; std::printf("FAIL: %s\n", message); } } while (0)

static std::vector<unsigned char> toneWav(unsigned int rate) {
    const unsigned int frames = rate * 2;
    std::vector<unsigned char> data(44 + frames * 4);
    auto put = [&](size_t offset, unsigned int value, int count) {
        for (int i = 0; i < count; ++i) data[offset + i] = (value >> (8 * i)) & 255;
    };
    std::memcpy(data.data(), "RIFF", 4); put(4, data.size() - 8, 4);
    std::memcpy(data.data() + 8, "WAVEfmt ", 8); put(16, 16, 4);
    put(20, 3, 2); put(22, 1, 2); put(24, rate, 4); put(28, rate * 4, 4);
    put(32, 4, 2); put(34, 32, 2);
    std::memcpy(data.data() + 36, "data", 4); put(40, frames * 4, 4);
    for (unsigned int i = 0; i < frames; ++i) {
        const float sample = 0.25f * std::sin(2.0 * 3.141592653589793 * 1000.0 * i / rate);
        std::memcpy(data.data() + 44 + i * 4, &sample, 4);
    }
    return data;
}

int main() {
    prepareEngineInit(5119);
    // The last init argument is the render-ahead depth, not a backend: 0
    // keeps the direct mixing path the test helper drives.
    EXPECT(initEngine(-1, 44100, 2048, 2, 1, 0, 0) == noError, "engine init");
    unsigned int hash = 0, voice = 0;
    EXPECT(setBufferStream(&hash, 1024 * 1024, PRESERVED, 0.0,
                          44100, 1, static_cast<int>(BufferType::AUTO), nullptr, nullptr) == noError,
           "AUTO stream setup");
    auto first = toneWav(44100);
    EXPECT(addAudioDataStream(hash, first.data(), first.size()) == noError, "first source delivery");
    EXPECT(play(hash, 0, 0.2f, 0, false, false, 0, 0, -1, -1, 1, &voice) == noError, "play");
    // The test helper is the sole mixer: a live device callback would consume
    // the same source concurrently.
    EXPECT(stopAudioDevice(1) == noError, "device stop for manual mixing");
    float output[512 * 2] = {};
    for (int i = 0; i < 24; ++i) soloudTestMixPlayerFrames(output, 512);
    EXPECT(soloudTestVoiceSampleRate(voice) == 44100, "initial voice rate");
    EXPECT(resetBufferStream(hash) == noError, "reset while the voice remains alive");
    auto second = toneWav(22050);
    EXPECT(addAudioDataStream(hash, second.data(), second.size()) == noError, "replacement source delivery");
    for (int i = 0; i < 24; ++i) soloudTestMixPlayerFrames(output, 512);
    const float rate = soloudTestVoiceSampleRate(voice);
    EXPECT(rate == 22050, "existing voice adopts the replacement sample rate");
    int crossings = 0;
    float previous = 0;
    for (int block = 0; block < 10; ++block) {
        soloudTestMixPlayerFrames(output, 512);
        for (int i = 0; i < 512; ++i) {
            const float sample = output[2 * i];
            if (previous <= 0 && sample > 0) ++crossings;
            previous = sample;
        }
    }
    // A 1 kHz tone has about 116 positive crossings in 5120 engine frames.
    EXPECT(crossings >= 112 && crossings <= 120, "replacement tone keeps its pitch and duration");
    std::printf("Reset rate: %.0f; crossings: %d\n", rate, crossings);
    // Replacing the source must also preserve a speed already set on the
    // same voice. A raw source-rate assignment would turn this back to 1x.
    setRelativePlaySpeed(voice, 2.0f);
    EXPECT(resetBufferStream(hash) == noError, "reset with relative speed");
    EXPECT(addAudioDataStream(hash, first.data(), first.size()) == noError, "replacement at the original rate");
    for (int i = 0; i < 24; ++i) soloudTestMixPlayerFrames(output, 512);
    EXPECT(soloudTestVoiceSampleRate(voice) == 44100, "second replacement base rate");
    crossings = 0;
    previous = 0;
    for (int block = 0; block < 10; ++block) {
        soloudTestMixPlayerFrames(output, 512);
        for (int i = 0; i < 512; ++i) {
            const float sample = output[2 * i];
            if (previous <= 0 && sample > 0) ++crossings;
            previous = sample;
        }
    }
    EXPECT(crossings >= 228 && crossings <= 237, "reset preserves relative speed");
    dispose();
    std::printf(failures ? "FAIL: %d assertions\n" : "PASS: buffer stream reset\n", failures);
    return failures ? 1 : 0;
}
