// A voice's relative play speed must survive the AUTO stream applying its
// detected format: on the first mix after play(), and after a seek to the
// start. Run one scenario per process: speed-after-play, speed-after-seek.
#include "bindings.h"
#include "audiobuffer/buffer.h"
#include "soloud.h"
#include <cmath>
#include <cstdio>
#include <cstring>
#include <vector>

extern "C" void soloudTestMixPlayerFrames(float *, unsigned int);

static int failures = 0;
#define EXPECT(c, message) do { if (!(c)) { ++failures; std::printf("FAIL: %s\n", message); } } while (0)

// A mono 32-bit float WAV file holding a 1 kHz tone.
static std::vector<unsigned char> toneWav(unsigned int rate, unsigned int frames) {
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

// Positive zero crossings in 5120 engine frames: about 116 for the tone at
// normal speed, about 232 at double speed.
static int crossings() {
    float output[512 * 2] = {};
    int count = 0;
    float previous = 0;
    for (int block = 0; block < 10; ++block) {
        soloudTestMixPlayerFrames(output, 512);
        for (int i = 0; i < 512; ++i) {
            const float sample = output[2 * i];
            if (previous <= 0 && sample > 0) ++count;
            previous = sample;
        }
    }
    return count;
}

static void mix(int blocks) {
    float output[512 * 2] = {};
    for (int i = 0; i < blocks; ++i) soloudTestMixPlayerFrames(output, 512);
}

static unsigned int startStream(unsigned int frames, unsigned int *voice) {
    unsigned int hash = 0;
    EXPECT(setBufferStream(&hash, 1024 * 1024, PRESERVED, 0.0,
                          44100, 1, static_cast<int>(BufferType::AUTO), nullptr, nullptr) == noError,
           "AUTO stream setup");
    auto tone = toneWav(44100, frames);
    EXPECT(addAudioDataStream(hash, tone.data(), tone.size()) == noError, "delivery");
    EXPECT(play(hash, 0, 0.2f, 0, false, false, 0, 0, -1, -1, 1, voice) == noError, "play");
    // The test helper is the sole mixer. A live callback would consume the
    // same source concurrently and race the direct mixer scratch buffers.
    EXPECT(stopAudioDevice(1) == noError, "device stop for manual mixing");
    return hash;
}

static bool doubleSpeed(int count) { return count >= 224 && count <= 240; }

int main(int argc, char **argv) {
    const char *only = argc > 1 ? argv[1] : nullptr;
    auto wanted = [&](const char *name) { return only == nullptr || std::strcmp(only, name) == 0; };
    // The last init argument selects render-ahead depth, not a backend.
    // Keep these first-mix speed assertions on the direct mixing path.
    prepareEngineInit(5127);
    EXPECT(initEngine(-1, 44100, 2048, 2, 1, 0, 0) == noError, "engine init");

    if (wanted("speed-after-play")) {
        // The usual order: play, then set the speed before the device mixes.
        unsigned int voice = 0;
        const unsigned int hash = startStream(44100 * 2, &voice);
        setRelativePlaySpeed(voice, 2.0f);
        const int count = crossings();
        std::printf("speed-after-play: %d crossings\n", count);
        EXPECT(doubleSpeed(count), "speed set right after play() survives the first mix");
        stop(voice);
        disposeSound(hash);
    }
    if (wanted("speed-after-seek")) {
        unsigned int voice = 0;
        const unsigned int hash = startStream(44100 * 2, &voice);
        mix(4);
        setRelativePlaySpeed(voice, 2.0f);
        EXPECT(seek(voice, 0.0f) == noError, "seek to the start");
        const int count = crossings();
        std::printf("speed-after-seek: %d crossings\n", count);
        EXPECT(doubleSpeed(count), "speed survives a seek to the start");
        stop(voice);
        disposeSound(hash);
    }
    dispose();
    std::printf(failures ? "FAIL: %d assertions\n" : "PASS: buffer stream speed\n", failures);
    return failures ? 1 : 0;
}
