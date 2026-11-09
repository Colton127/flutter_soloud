// The buffer passed to loadMem() belongs to the caller. Dart allocates it
// with package:ffi's calloc (CoTaskMemAlloc on Windows), copies the encoded
// bytes into it and frees it once the call returns, so the native loader must
// neither free it nor keep it:
//
//   * in disk mode, a format the SoLoud stream cannot parse (M4A, AAC) falls
//     back to the plugin's other decoders, which read the same bytes again,
//     so the failed stream must not have freed them;
//   * a stream that does parse them must not free them with delete[] when the
//     sound is disposed: they were not allocated with new[].
//
// This calls the loadMem() export the way Dart does, on a real engine, and is
// meant to run under AddressSanitizer (test/run_load_mem_ownership_test.sh).
// The argument picks the files:
//
//   soloud    WAV and MP3, which the SoLoud loaders parse. Run with
//             alloc_dealloc_mismatch enabled, so a delete[] of the caller's
//             buffer aborts.
//   platform  M4A and AAC, which fall back to the platform decoder. Run
//             without it: Apple's AAC decoder has a mismatch of its own. A
//             read of the buffer after the failed stream freed it still
//             aborts. Where the platform cannot decode AAC, the fallback
//             fails cleanly and only the memory checks apply.
#include "bindings.h"
#include "enums.h"

#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <fstream>
#include <vector>

namespace {
int gFailures = 0;

void onVoiceEnded(unsigned int *handle) { std::free(handle); }
void onFileLoaded(enum PlayerErrors *, char *, unsigned int *,
                  unsigned long long *) {}
void onStateChanged(enum PlayerStateEvents *state) { std::free(state); }

std::vector<unsigned char> readFile(const char *path) {
    std::ifstream in(path, std::ios::binary | std::ios::ate);
    std::vector<unsigned char> data;
    if (!in) return data;
    data.resize(static_cast<size_t>(in.tellg()));
    in.seekg(0);
    in.read(reinterpret_cast<char *>(data.data()),
            static_cast<std::streamsize>(data.size()));
    return data;
}

/// Loads [path] like Dart's loadMem() does, plays nothing, disposes it and
/// frees the caller's buffer. [mustLoad]: whether the load must succeed here.
void loadLikeDart(const char *path, bool loadIntoMem, bool mustLoad) {
    const std::vector<unsigned char> data = readFile(path);
    if (data.empty()) {
        std::printf("  FAIL: cannot read %s\n", path);
        ++gFailures;
        return;
    }
    auto *buffer = static_cast<unsigned char *>(std::calloc(data.size(), 1));
    std::memcpy(buffer, data.data(), data.size());
    unsigned int hash = 0;
    const PlayerErrors error =
        loadMem(const_cast<char *>(path), buffer,
                static_cast<int>(data.size()), loadIntoMem ? 1 : 0, &hash);
    std::printf("  %s, %s mode: error %d\n", path,
                loadIntoMem ? "memory" : "disk", static_cast<int>(error));
    if (mustLoad && error != PlayerErrors::noError) {
        std::printf("  FAIL: %s should load in %s mode (error %d)\n", path,
                    loadIntoMem ? "memory" : "disk", static_cast<int>(error));
        ++gFailures;
    }
    if (hash != 0) disposeSound(hash);
    // The buffer is still the caller's: Dart frees it after the call.
    std::free(buffer);
}
} // namespace

int main(int argc, char **argv) {
    setAudioDeviceIdleTimeout(600000);
    prepareEngineInit(4018);
    if (initEngine(-1, 44100, 2048, 2, 1, 0, 0) != PlayerErrors::noError) {
        std::printf("the engine did not start\n");
        return 2;
    }
    setDartEventCallback(onVoiceEnded, onFileLoaded, onStateChanged, 4018);

#ifdef __APPLE__
    const bool platformDecodesAac = true; // AudioToolbox
#else
    const bool platformDecodesAac = false;
#endif
    const bool soloudFormats = argc < 2 || std::strcmp(argv[1], "platform") != 0;
    const bool platformFormats = argc < 2 || std::strcmp(argv[1], "soloud") != 0;
    for (const bool loadIntoMem : {false, true}) {
        if (soloudFormats) {
            loadLikeDart("example/assets/audio/sample-WAV.wav", loadIntoMem,
                         true);
            loadLikeDart("example/assets/audio/sample-MP3.mp3", loadIntoMem,
                         true);
        }
        if (platformFormats) {
            loadLikeDart("example/assets/audio/sample-AAC.m4a", loadIntoMem,
                         platformDecodesAac);
            loadLikeDart("example/assets/audio/sample-AAC.aac", loadIntoMem,
                         platformDecodesAac);
        }
    }

    dispose();
    std::printf(gFailures ? "%d failure(s)\n" : "Passed!\n", gFailures);
    return gFailures ? 1 : 0;
}
