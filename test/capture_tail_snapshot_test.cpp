#include "bindings.h"
#include "device_lifecycle_test_hooks.h"
#include "mixeroutput/mixer_output.h"
#include <atomic>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <thread>
#include <vector>

int main() {
    prepareEngineInit(1);
    if (initEngine(-1, 44100, 2048, 2, 0, 0, 0) != noError) {
        printf("Capture snapshot: FAIL (engine init)\n"); return 1;
    }
    stopAudioDevice(1);
    if (startMixerCapture(MIXER_OUTPUT_PCM_F32LE, 44100, 2, 4096, 4096, -1) != noError) {
        printf("Capture snapshot: FAIL (capture start)\n"); dispose(); return 1;
    }
    MixerOutput::instance().setDataCallback(nullptr);
    std::vector<float> input(1024);
    for (size_t i = 0; i < input.size(); ++i) input[i] = static_cast<float>(i) / 2048;
    MixerOutput::instance().onAudioData(input.data(), 512);
    const unsigned int id = getMixerCaptureId();
    std::vector<unsigned char> snapshot;
    std::atomic<bool> attempted{false}, restarted{false};
    int startResult = -1;
    soloud_test::armBarrier(soloud_test::DeviceBarrier::mixerCaptureTailSelected);
    std::thread reader([&] {
        size_t length = 0;
#ifdef SOLOUD_CAPTURE_TAIL_SNAPSHOT
        auto *bytes = stopMixerCaptureAndDrain(id, &length);
        if (length) snapshot.assign(bytes, bytes + length);
        nativeFree(bytes);
#else
        // Exactly the old Dart tail-copy protocol: stop, query, borrow, copy.
        stopMixerCapture();
        length = getMixerCaptureAvailableBytes();
        auto *bytes = getMixerCaptureBufferPointer();
        soloud_test::hitBarrier(soloud_test::DeviceBarrier::mixerCaptureTailSelected);
        snapshot.assign(bytes, bytes + length);
#endif
    });
    soloud_test::waitBarrierReached(soloud_test::DeviceBarrier::mixerCaptureTailSelected);
    std::thread starter([&] {
        attempted.store(true);
        startResult = startMixerCapture(MIXER_OUTPUT_PCM_F32LE, 44100, 2,
                                         1 << 20, 4096, -1);
        restarted.store(true);
    });
    while (!attempted.load()) std::this_thread::yield();
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(1);
    while (!restarted.load() && std::chrono::steady_clock::now() < deadline)
        std::this_thread::sleep_for(std::chrono::milliseconds(1));
    const bool replacedDuringCopy = restarted.load();
    soloud_test::releaseBarrier(soloud_test::DeviceBarrier::mixerCaptureTailSelected);
    reader.join(); starter.join();
    const bool correct = !replacedDuringCopy && startResult == noError &&
        snapshot.size() == input.size() * sizeof(float) &&
        memcmp(snapshot.data(), input.data(), snapshot.size()) == 0;
    stopMixerCapture(); dispose();
    printf("Capture snapshot: %s\n", correct ? "PASS" : "FAIL");
    return correct ? 0 : 1;
}
