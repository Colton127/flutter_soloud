#include "bindings.h"
#include "mixeroutput/mixer_output.h"
#include "soloud.h"
#include <atomic>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <thread>

static std::atomic<bool> open1{true}, open2{true};
static std::atomic<int64_t> token{0}, target{0};
static std::atomic<int> failedPosts{0};
static bool postInteger(int64_t port, int64_t message) {
    const bool live = port == 1 ? open1.load() : open2.load();
    if (!live) { failedPosts.fetch_add(1); return false; }
    if (message != 0) { target.store(port); token.store(message); }
    return true;
}
static int failures = 0;
#define EXPECT(c, m) do { if (!(c)) { ++failures; std::printf("FAIL: %s\n", m); } } while (0)
static bool waitForToken(int64_t port) {
    for (int i = 0; i < 500; ++i) {
        if (token.load() != 0 && target.load() == port) return true;
        std::this_thread::sleep_for(std::chrono::milliseconds(1));
    }
    return false;
}
int main() {
    struct Entry { const char *name; void (*function)(); };
    struct Api { int major; int minor; const Entry *functions; };
    const Entry entries[] = {{"Dart_PostInteger", reinterpret_cast<void (*)()>(postInteger)}, {nullptr, nullptr}};
    Api api{2, 0, entries};
    prepareEngineInit(5123);
    // The last init argument is the render-ahead depth, not a backend.
    EXPECT(initEngine(-1, 44100, 2048, 2, 1, 0, 0) == noError, "engine init");
    setDartEventCallback(nullptr, nullptr, nullptr, 5123);
    unsigned int first = 0, second = 0;
    EXPECT(startMixerCaptureOnPort(MIXER_OUTPUT_PCM_F32LE, -1, -1, 4096, 4096, -1,
                                  &first, &api, 1, 5123) == noError, "first port capture");
    float input[1024] = {};
    MixerOutput::instance().onAudioData(input, 512);
    EXPECT(waitForToken(1), "first packet posted");
    unsigned int id = 0; size_t length = 0;
    auto *bytes = takeMixerOutputPacketWithLength(reinterpret_cast<unsigned char *>(token.load()), &id, &length);
    EXPECT(bytes != nullptr && id == first && length == sizeof(input), "port packet owns its bytes and length");
    nativeFree(bytes);
    // No audio callback is required to discover an exit: the state query's
    // zero probe detects a closed port even while the device is idle.
    open1.store(false);
    EXPECT(!isMixerCaptureRunningForStart(), "closed first port is inactive");
    token.store(0);
    EXPECT(startMixerCaptureOnPort(MIXER_OUTPUT_PCM_F32LE, -1, -1, 4096, 4096, -1,
                                  &second, &api, 2, 5123) == noError && second != first,
           "new start reaps the closed port without deinit");
    size_t staleLength = 0;
    nativeFree(stopMixerCaptureAndDrain(first, &staleLength));
    EXPECT(isMixerCaptureRunning(), "old owner cannot stop the replacement");
    MixerOutput::instance().onAudioData(input, 512);
    EXPECT(waitForToken(2), "replacement port receives packets");
    open2.store(false);
    const int previousFailures = failedPosts.load();
    // Stop and drain the already-posted threshold token, then use chunk
    // mode so a failed asynchronous post is observable without a query.
    nativeFree(stopMixerCaptureAndDrain(second, &length));
    open2.store(true);
    EXPECT(startMixerCaptureOnPort(MIXER_OUTPUT_PCM_F32LE, -1, -1, 32768, 4096, 2048,
                                  &second, &api, 2, 5123) == noError, "chunk port capture");
    open2.store(false);
    float chunk[4096] = {};
    MixerOutput::instance().onAudioData(chunk, 2048);
    for (int i = 0; i < 500 && failedPosts.load() == previousFailures; ++i)
        std::this_thread::sleep_for(std::chrono::milliseconds(1));
    EXPECT(failedPosts.load() > previousFailures, "closed port rejects a real packet post");
    EXPECT(!isMixerCaptureRunning(), "failed packet post marks the capture inactive");
    dispose();
    std::printf(failures ? "FAIL: %d port assertions\n" : "PASS: port capture lifecycle\n", failures);
    return failures ? 1 : 0;
}
