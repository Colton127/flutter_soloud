#include "soloud.h"
#include "audiobuffer/mp3_stream_decoder.h"
// Pull-stream MP3 seek origins on a generated quiet/noisy VBR file.
// The requested backward offset must agree with a whole-file drmp3 map,
// rather than a map whose byte and PCM origins are the last seek fragment.
#include "soloud/src/audiosource/wav/dr_mp3.h"
#include "bindings.h"
#include "audiobuffer/stream_decoder.h"
#include "enums.h"

#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <deque>
#include <fstream>
#include <functional>
#include <mutex>
#include <thread>
#include <vector>

extern "C" void soloudTestMixPlayerFrames(float *, unsigned int);

namespace {
constexpr int64_t kEngineId = 4017;
int gFailures = 0;
#define EXPECT(cond, ...)                                                      \
    do {                                                                       \
        if (!(cond)) {                                                         \
            ++gFailures;                                                       \
            std::printf("  FAIL: ");                                           \
            std::printf(__VA_ARGS__);                                          \
            std::printf("\n");                                                 \
        }                                                                      \
    } while (0)

std::vector<unsigned char> gFile;
std::atomic<unsigned int> gHash{0};
std::mutex gMutex;
std::condition_variable gCv;
std::deque<uint64_t> gRequests;
std::vector<uint64_t> gLog;
std::atomic<bool> gStop{false};

void onMoreData(uint64_t offset) {
    std::lock_guard<std::mutex> lock(gMutex);
    gRequests.push_back(offset);
    gLog.push_back(offset);
    gCv.notify_all();
}

void feeder() {
    while (!gStop.load()) {
        uint64_t offset;
        {
            std::unique_lock<std::mutex> lock(gMutex);
            gCv.wait_for(lock, std::chrono::milliseconds(20), [] {
                return (!gRequests.empty() && gHash.load() != 0) ||
                       gStop.load();
            });
            if (gRequests.empty() || gHash.load() == 0) continue;
            offset = gRequests.front();
            gRequests.pop_front();
        }
        if (offset >= gFile.size()) continue; // nothing there to give
        const size_t len =
            std::min<size_t>(32768, gFile.size() - static_cast<size_t>(offset));
        addPullBufferDataStream(gHash, gFile.data() + offset,
                                static_cast<unsigned int>(len), offset);
    }
}

void onVoiceEnded(unsigned int *h) { std::free(h); }
void onFileLoaded(enum PlayerErrors *e, char *n, unsigned int *h,
                  unsigned long long *t) {
    (void)e; (void)n; (void)h; (void)t;
}
void onStateChanged(enum PlayerStateEvents *s) { std::free(s); }

bool waitFor(const std::function<bool()> &pred, int ms) {
    float output[512 * 2] = {};
    for (int i = 0; i < ms / 10; ++i) {
        if (pred()) return true;
        soloudTestMixPlayerFrames(output, 512);
        std::this_thread::sleep_for(std::chrono::milliseconds(1));
    }
    return pred();
}

size_t logSize() {
    std::lock_guard<std::mutex> lock(gMutex);
    return gLog.size();
}
} // namespace


uint64_t fileOffsetOf(double seconds) {
    drmp3 decoder{};
    EXPECT(drmp3_init_memory(&decoder, gFile.data(), gFile.size(), nullptr),
           "whole-file reference init");
    drmp3_uint32 count = 4096;
    std::vector<drmp3_seek_point> points(count);
    const bool ok = drmp3_calculate_seek_points(&decoder, &count, points.data());
    EXPECT(ok && count > 0, "whole-file reference seek map");
    const uint64_t frame = uint64_t(seconds * decoder.sampleRate);
    uint64_t offset = 0;
    for (drmp3_uint32 i = 0; ok && i < count; ++i) {
        if (points[i].pcmFrameIndex > frame) break;
        offset = points[i].seekPosInBytes;
    }
    drmp3_uninit(&decoder);
    return offset;
}

unsigned int startStream(const char *path) {
    {
        std::lock_guard<std::mutex> lock(gMutex);
        gRequests.clear();
        gLog.clear();
    }
    unsigned int hash = 0;
    PlayerErrors err = setPullBufferStream(
        &hash, 44100 * 2 * 4, 0.8, 44100, 2, static_cast<int>(BufferType::AUTO),
        gFile.size(), nullptr, nullptr, onMoreData, nullptr);
    EXPECT(err == PlayerErrors::noError, "setPullBufferStream: %d", (int)err);
    gHash.store(hash);
    unsigned int voice = 0;
    err = play(gHash, 0, 0.05f, 0.0f, false, false, 0.0, 0.0, -1, -1, 1.0f, &voice);
    EXPECT(err == PlayerErrors::noError && voice != 0, "play: %d", (int)err);
    double start = 0, end = 0;
    waitFor([&] {
        getPullBufferTimeRange(gHash, &start, &end);
        return end > 0.5;
    }, 5000);
    std::printf("  %s: buffered [%.2f, %.2f] before the first seek\n", path,
                start, end);
    return voice;
}

uint64_t seekAndGetRequest(unsigned int voice, float target) {
    const size_t before = logSize();
    seek(voice, target);
    waitFor([&] { return logSize() > before; }, 3000);
    std::lock_guard<std::mutex> lock(gMutex);
    return gLog.size() > before ? gLog[before] : UINT64_MAX;
}

void endStream(unsigned int voice) {
    stop(voice);
    const unsigned int hash = gHash.exchange(0);
    disposeSound(hash);
}



void checkMp3SeekOrigin(const char *path) {
    MP3DecoderWrapper indexed;
    indexed.detectedType = DetectedType::BUFFER_MP3_WITH_ID3;
    EXPECT(indexed.initializeDecoder(44100, 2), "retained map decoder init");
    auto complete = gFile;
    int mapRate = 44100, mapChannels = 2;
    indexed.decode(complete, &mapRate, &mapChannels, 8192);
    const auto originalOffset = indexed.timeToByteOffset(0.5);
    EXPECT(originalOffset != 0, "retained map has a backward entry");
    indexed.prepareForSeek(6 * 44100 * 2, fileOffsetOf(6.0));
    EXPECT(indexed.canSeekToTime(0.5) &&
               indexed.timeToByteOffset(0.5) == originalOffset,
           "retained map is usable before another fragment arrives");
    indexed.cleanup();
    EXPECT(!indexed.canSeekToTime(0.5), "full cleanup discards the retained map");
    const unsigned int voice = startStream(path);
    for (int pass = 0; pass < 2; ++pass) {
        const uint64_t first = seekAndGetRequest(voice, 6.0f);
        double start = 0, end = 0;
        const bool ready = waitFor([&] {
            getPullBufferTimeRange(gHash, &start, &end);
            return end > 6.8;
        }, 5000);
        EXPECT(ready, "MP3 must decode enough of the noisy section for a local seek map");
        MP3DecoderWrapper local;
        local.detectedType = DetectedType::BUFFER_MP3_STREAM;
        EXPECT(local.initializeDecoder(44100, 2), "local reference init");
        std::vector<unsigned char> window;
        if (first < gFile.size()) {
            window.assign(gFile.begin() + first,
                          gFile.begin() + std::min<uint64_t>(gFile.size(), first + 32768));
        }
        int rate = 44100, channels = 2;
        local.decode(window, &rate, &channels);
        const uint64_t localOffset = local.timeToByteOffset(0.5);
        EXPECT(local.canSeekToTime(0.5) && localOffset != 0,
               "the fragment map must cover the backward target");
        const uint64_t expected = fileOffsetOf(0.5);
        const size_t before = logSize();
        uint64_t requested = UINT64_MAX;
        seek(voice, 0.5f);
        const uint64_t upper = std::max<uint64_t>(first, 32768);
        waitFor([&] {
            std::lock_guard<std::mutex> lock(gMutex);
            for (size_t i = before; i < gLog.size(); ++i) {
                if (gLog[i] < upper) { requested = gLog[i]; return true; }
            }
            return false;
        }, 3000);
        const uint64_t difference = requested > expected ? requested - expected : expected - requested;
        std::printf("MP3 seek: first=%llu back=%llu reference=%llu local=%llu difference=%llu\n",
                    (unsigned long long)first, (unsigned long long)requested,
                    (unsigned long long)expected, (unsigned long long)localOffset,
                    (unsigned long long)difference);
        EXPECT(expected != 0 && requested != UINT64_MAX, "MP3 seek requests and reference exist");
        // Allow adjacent seek-point/preroll boundaries, bounded by four maximum
        // MPEG-1 frames at this fixture's 44.1 kHz sample rate.
        EXPECT(difference <= 4 * 1045,
               "MP3 backward seek must use the original file coordinate system");
        const bool backReady = waitFor([&] {
            getPullBufferTimeRange(gHash, &start, &end);
            return start < 2.0 && end > 1.0;
        }, 5000);
        EXPECT(backReady, "MP3 must resume near the backward target");
    }
    endStream(voice);
}

bool loadFile(const char *path) {
    std::ifstream in(path, std::ios::binary | std::ios::ate);
    if (!in) {
        std::printf("cannot open %s\n", path);
        ++gFailures;
        return false;
    }
    gFile.resize(static_cast<size_t>(in.tellg()));
    in.seekg(0);
    in.read(reinterpret_cast<char *>(gFile.data()),
            static_cast<std::streamsize>(gFile.size()));
    return true;
}

int main(int argc, char **argv) {
    if (argc < 2) { std::printf("FAIL: MP3 fixture path required\n"); return 2; }
    std::vector<const char *> files{argv[1]};
    setAudioDeviceIdleTimeout(600000);
    prepareEngineInit(kEngineId);
    if (initEngine(-1, 44100, 2048, 2, 1, 0, SoLoud::Soloud::NULLDRIVER) != PlayerErrors::noError) {
        std::printf("engine did not start\n");
        return 2;
    }
    setDartEventCallback(onVoiceEnded, onFileLoaded, onStateChanged, kEngineId);
    std::thread feed(feeder);

    for (const char *path : files) {
        if (!loadFile(path)) continue;
        checkMp3SeekOrigin(path);
    }

    gStop.store(true);
    feed.join();
    dispose();
    std::printf(gFailures ? "%d failure(s)\n" : "Passed!\n", gFailures);
    return gFailures ? 1 : 0;
}
