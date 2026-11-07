// Host benchmark: how long do synchronous voice-control calls wait while the
// engine mixes disk streams, and on what?
//
// A mixer thread drives the engine at real time with the null backend, like
// the device callback does: one Soloud::mix() per engine quantum, or, with
// --render-ahead, the render-ahead ring top-up (one mix() per quantum until
// the ring is full) plus a device-period read. The engine plays WavStream
// voices streamed from disk (MP3, OGG, FLAC and WAV from
// example/assets/audio, the same path as LoadMode.disk) and fully decoded
// in-memory Wav voices (LoadMode.memory). A control thread issues the calls
// the Dart side makes on its platform thread -- setVolume, getVolume,
// getStreamPosition, seek, isValidVoiceHandle, setPan, getPan, setPause,
// getPause -- at UI-like intervals and times each one.
//
// Attribution: the sources and the file layer are instrumented, so every mix
// block records how long it spent inside getAudio() of disk voices (and how
// much of that in file reads) versus memory voices, and every seek records
// how long the source's own seek took and on which thread. A control call
// that starts while a mix block is running and returns when it ends waited
// for that block.
//
// --disk-delay-ms/--disk-delay-prob inject stalls into the disk reads under
// getAudio() to model slow storage (cold page cache, flash under load).
//
// --m4a <file> adds NativeStream voices (LoadMode.disk for M4A/MP4; macOS
// and Android only), to time NativeStream's seek inside the mix as well.
//
// --burst <n> runs a different scenario instead (see runBurst()): bursts of
// n setVolume/setPan/setPause calls spread over --burst-voices voices, each
// followed right away by getActiveVoiceCount(), stop() and play(), which
// take the audio mutex and so apply whatever the burst left queued.
// --burst-hold-ms <ms> issues each burst while another thread holds the
// audio mutex (as a long mix block would), so that all of it is queued.
//
// "late wake-ups/underruns" counts mixer periods that fell more than a period
// behind real time (or, with the ring, device reads the ring could not
// fill): the late blocks a listener would hear.
//
// Build and run from the flutter_soloud repository root with:
//
//   ./test/run_control_latency_bench.sh [options]

#include "soloud.h"
#include "soloud_file.h"
#include "soloud_wav.h"
#include "soloud_wavstream.h"
#include "native_stream/native_stream_source.h"

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <mutex>
#include <random>
#include <string>
#include <thread>
#include <vector>

namespace {

using Clock = std::chrono::steady_clock;

Clock::time_point gEpoch = Clock::now();

double nowUs() {
    return std::chrono::duration<double, std::micro>(Clock::now() - gEpoch)
        .count();
}

struct Options {
    double seconds = 10.0;
    unsigned int sampleRate = 44100;
    unsigned int buffer = 2048;  // engine quantum (flutter_soloud default)
    unsigned int renderAhead = 0;
    unsigned int devicePeriod = 512;
    double diskDelayMs = 0.0;
    double diskDelayProb = 0.0;
    unsigned int streamsPerFormat = 2;
    unsigned int memoryVoices = 4;
    unsigned int minIntervalMs = 2;
    unsigned int maxIntervalMs = 8;
    double pollMs = 5.0;
    const char *assets = "example/assets/audio";
    // M4A file to stream with NativeStream (LoadMode.disk for M4A/MP4), off
    // by default so the default runs stay comparable.
    const char *m4a = nullptr;
    // Burst mode (runBurst()): calls per burst (0 = off), voices, rounds.
    unsigned int burst = 0;
    unsigned int burstVoices = 100;
    unsigned int burstRounds = 3;
    double burstHoldMs = 0;
};

Options gOptions;

// ---- Instrumentation ------------------------------------------------------

// Time accumulated by the current thread, in microseconds. The mixer thread
// snapshots it around every block; a control thread around every call.
struct ThreadTimes {
    double diskGetAudio = 0;  // inside getAudio() of disk voices
    double diskIo = 0;        // inside File::read() under those (subset)
    double memGetAudio = 0;   // inside getAudio() of memory voices
    double seekWork = 0;      // inside a source's own seek()
};

thread_local ThreadTimes tTimes;
thread_local bool tInSeek = false;

std::atomic<double> gDiskDelayMs{0.0};
std::atomic<double> gDiskDelayProb{0.0};

// DiskFile with timed (and optionally stalled) reads.
class TimedDiskFile final : public SoLoud::File {
public:
    bool open(const std::string &path) {
        return mInner.open(path.c_str()) == SoLoud::SO_NO_ERROR;
    }

    unsigned int read(unsigned char *dst, unsigned int bytes) override {
        const double t0 = nowUs();
        const double delayMs = gDiskDelayMs.load(std::memory_order_relaxed);
        if (delayMs > 0 && !tInSeek) {
            std::uniform_real_distribution<double> dist(0.0, 1.0);
            if (dist(mRng) < gDiskDelayProb.load(std::memory_order_relaxed)) {
                std::this_thread::sleep_for(
                    std::chrono::microseconds((long long)(delayMs * 1000)));
            }
        }
        const unsigned int n = mInner.read(dst, bytes);
        tTimes.diskIo += nowUs() - t0;
        return n;
    }
    int eof() override { return mInner.eof(); }
    unsigned int length() override { return mInner.length(); }
    void seek(int offset) override { mInner.seek(offset); }
    unsigned int pos() override { return mInner.pos(); }

private:
    SoLoud::DiskFile mInner;
    std::mt19937 mRng{1234};
};

enum class Format { MP3, OGG, FLAC, WAV, M4A, MEMORY };
constexpr int kFormats = 6;

const char *formatName(Format f) {
    switch (f) {
    case Format::MP3: return "mp3";
    case Format::OGG: return "ogg";
    case Format::FLAC: return "flac";
    case Format::WAV: return "wav";
    case Format::M4A: return "m4a";
    case Format::MEMORY: return "memory";
    }
    return "?";
}

struct SeekRecord {
    Format format;
    bool backward;
    bool onMixerThread;
    double us;
};

std::mutex gSeekRecordsMutex;
std::vector<SeekRecord> gSeekRecords;
std::thread::id gMixerThreadId;

std::atomic<unsigned long long> gLoopRestarts{0};
// Frames each format's voices actually produced (getAudio() return values),
// to tell a run whose streamed voices kept playing from one where they went
// silent (which also takes their disk reads out of the mix).
std::atomic<unsigned long long> gFramesByFormat[6];

void recordSeek(Format format, bool backward, double seconds, double us) {
    // The looping voices restart with seek(0) from inside the mix; only the
    // control thread's seeks (random non-zero targets) are of interest.
    if (seconds == 0.0) {
        ++gLoopRestarts;
        return;
    }
    std::lock_guard<std::mutex> lock(gSeekRecordsMutex);
    gSeekRecords.push_back(
        {format, backward, std::this_thread::get_id() == gMixerThreadId, us});
}

class TimedWavStreamInstance final : public SoLoud::WavStreamInstance {
public:
    TimedWavStreamInstance(SoLoud::WavStream *parent, Format format)
        : SoLoud::WavStreamInstance(parent), mFormat(format) {}

    unsigned int getAudio(float *buffer, unsigned int samples,
                          unsigned int bufferSize) override {
        const double t0 = nowUs();
        const unsigned int n =
            SoLoud::WavStreamInstance::getAudio(buffer, samples, bufferSize);
        tTimes.diskGetAudio += nowUs() - t0;
        gFramesByFormat[(int)mFormat] += n;
        return n;
    }

    SoLoud::result seek(double seconds, float *scratch,
                        unsigned int scratchSize) override {
        const bool backward = seconds < mStreamPosition;
        const double t0 = nowUs();
        tInSeek = true;
        const SoLoud::result r =
            SoLoud::WavStreamInstance::seek(seconds, scratch, scratchSize);
        tInSeek = false;
        const double dt = nowUs() - t0;
        if (seconds != 0.0) tTimes.seekWork += dt;
        recordSeek(mFormat, backward, seconds, dt);
        return r;
    }

private:
    Format mFormat;
};

class TimedWavStream final : public SoLoud::WavStream {
public:
    explicit TimedWavStream(Format format) : mFormat(format) {}
    SoLoud::AudioSourceInstance *createInstance() override {
        return new TimedWavStreamInstance(this, mFormat);
    }

private:
    Format mFormat;
};

class TimedWavInstance final : public SoLoud::WavInstance {
public:
    explicit TimedWavInstance(SoLoud::Wav *parent)
        : SoLoud::WavInstance(parent) {}

    unsigned int getAudio(float *buffer, unsigned int samples,
                          unsigned int bufferSize) override {
        const double t0 = nowUs();
        const unsigned int n =
            SoLoud::WavInstance::getAudio(buffer, samples, bufferSize);
        tTimes.memGetAudio += nowUs() - t0;
        return n;
    }

    SoLoud::result seek(double seconds, float *scratch,
                        unsigned int scratchSize) override {
        const bool backward = seconds < mStreamPosition;
        const double t0 = nowUs();
        const SoLoud::result r =
            SoLoud::WavInstance::seek(seconds, scratch, scratchSize);
        const double dt = nowUs() - t0;
        if (seconds != 0.0) tTimes.seekWork += dt;
        recordSeek(Format::MEMORY, backward, seconds, dt);
        return r;
    }
};

// NativeStream: getAudio() reads a ring its decoder thread fills, and seek()
// posts a request to that thread; timed like the WavStream voices.
class TimedNativeStreamInstance final : public SoLoud::NativeStreamInstance {
public:
    TimedNativeStreamInstance(SoLoud::NativeStream *parent)
        : SoLoud::NativeStreamInstance(parent, 0) {}

    unsigned int getAudio(float *buffer, unsigned int samples,
                          unsigned int bufferSize) override {
        const double t0 = nowUs();
        const unsigned int n =
            SoLoud::NativeStreamInstance::getAudio(buffer, samples, bufferSize);
        tTimes.diskGetAudio += nowUs() - t0;
        gFramesByFormat[(int)Format::M4A] += n;
        return n;
    }

    SoLoud::result seek(double seconds, float *scratch,
                        unsigned int scratchSize) override {
        const bool backward = seconds < mStreamPosition;
        const double t0 = nowUs();
        const SoLoud::result r =
            SoLoud::NativeStreamInstance::seek(seconds, scratch, scratchSize);
        const double dt = nowUs() - t0;
        if (seconds != 0.0) tTimes.seekWork += dt;
        recordSeek(Format::M4A, backward, seconds, dt);
        return r;
    }
};

class TimedNativeStream final : public SoLoud::NativeStream {
public:
    SoLoud::AudioSourceInstance *createInstance() override {
        // No render-ahead history: the bench's ring runs keep it short.
        return new TimedNativeStreamInstance(this);
    }
};

class TimedWav final : public SoLoud::Wav {
public:
    SoLoud::AudioSourceInstance *createInstance() override {
        return new TimedWavInstance(this);
    }
};

// ---- Records --------------------------------------------------------------

struct BlockRecord {
    double start, end;
    double diskGetAudio, diskIo, memGetAudio, seekWork;
};

enum class Op {
    SetVolume, GetVolume, GetStreamPosition, Seek, IsValidVoiceHandle,
    SetPan, GetPan, SetPause, GetPause, Count
};

const char *opName(Op op) {
    switch (op) {
    case Op::SetVolume: return "setVolume";
    case Op::GetVolume: return "getVolume";
    case Op::GetStreamPosition: return "getStreamPosition";
    case Op::Seek: return "seek";
    case Op::IsValidVoiceHandle: return "isValidVoiceHandle";
    case Op::SetPan: return "setPan";
    case Op::GetPan: return "getPan";
    case Op::SetPause: return "setPause";
    case Op::GetPause: return "getPause";
    default: return "?";
    }
}

struct CallRecord {
    Op op;
    Format format;
    int thread;  // 0 = ui, 1 = poller
    double start, end;
    double seekWorkOnCaller;  // source seek() time spent on this thread
};

struct Voice {
    SoLoud::handle handle;
    Format format;
    double length;
};

// ---- Helpers --------------------------------------------------------------

double percentile(std::vector<double> v, double p) {
    if (v.empty()) return 0;
    std::sort(v.begin(), v.end());
    const size_t i = std::min(v.size() - 1,
                              (size_t)std::ceil(p / 100.0 * v.size()) - 1);
    return v[i];
}

void printDist(const char *label, const std::vector<double> &us) {
    if (us.empty()) {
        std::printf("  %-22s      n=0\n", label);
        return;
    }
    std::printf("  %-22s n=%-6zu p50=%8.3f ms  p99=%8.3f ms  max=%8.3f ms\n",
                label, us.size(), percentile(us, 50) / 1000.0,
                percentile(us, 99) / 1000.0,
                *std::max_element(us.begin(), us.end()) / 1000.0);
}

bool parseArgs(int argc, char **argv) {
    for (int i = 1; i < argc; ++i) {
        const std::string a = argv[i];
        auto next = [&](double &out) {
            if (i + 1 >= argc) return false;
            out = std::atof(argv[++i]);
            return true;
        };
        double v = 0;
        if (a == "--seconds" && next(v)) gOptions.seconds = v;
        else if (a == "--buffer" && next(v)) gOptions.buffer = (unsigned)v;
        else if (a == "--rate" && next(v)) gOptions.sampleRate = (unsigned)v;
        else if (a == "--render-ahead" && next(v)) gOptions.renderAhead = (unsigned)v;
        else if (a == "--device-period" && next(v)) gOptions.devicePeriod = (unsigned)v;
        else if (a == "--disk-delay-ms" && next(v)) gOptions.diskDelayMs = v;
        else if (a == "--disk-delay-prob" && next(v)) gOptions.diskDelayProb = v;
        else if (a == "--streams" && next(v)) gOptions.streamsPerFormat = (unsigned)v;
        else if (a == "--mem" && next(v)) gOptions.memoryVoices = (unsigned)v;
        else if (a == "--poll-ms" && next(v)) gOptions.pollMs = v;
        else if (a == "--assets" && i + 1 < argc) gOptions.assets = argv[++i];
        else if (a == "--m4a" && i + 1 < argc) gOptions.m4a = argv[++i];
        else if (a == "--burst" && next(v)) gOptions.burst = (unsigned)v;
        else if (a == "--burst-voices" && next(v)) gOptions.burstVoices = (unsigned)v;
        else if (a == "--burst-rounds" && next(v)) gOptions.burstRounds = (unsigned)v;
        else if (a == "--burst-hold-ms" && next(v)) gOptions.burstHoldMs = v;
        else {
            std::fprintf(stderr, "unknown or incomplete option: %s\n", a.c_str());
            return false;
        }
    }
    return true;
}

// ---- Burst mode -------------------------------------------------------------

// A burst of --burst setVolume/setPan/setPause calls spread round-robin over
// --burst-voices in-memory voices, issued as fast as the caller can, then,
// right away, one getActiveVoiceCount(), one stop() and one play(): calls
// unrelated to the burst that take the audio mutex, and so apply whatever
// the burst left queued first (with the render-ahead ring, possibly a
// retroactive re-mix per queued change). Repeated --burst-rounds times while
// the mixer thread runs at real time (with --render-ahead, the ring top-up
// plus a device-period read, as in the main bench). Reports, per round: the
// burst (total and worst single call), each of the three calls, and the
// longest mix block from the start of the burst to 1 s after its end (wall
// time of mix(), waiting for the audio mutex included).
//
// With --burst-hold-ms, a holder thread takes the audio mutex before the
// burst and releases it when the burst is done (or after that many ms,
// whichever is first), as a mix block stalled in a slow source would: the
// whole burst is then queued, and the three calls right after it -- or the
// mixer, if it gets the mutex first -- apply all of it.
int runBurst(SoLoud::Soloud &soloud) {
    SoLoud::Wav sound;
    const std::string path = std::string(gOptions.assets) + "/sample-WAV.wav";
    if (sound.load(path.c_str()) != SoLoud::SO_NO_ERROR) {
        std::fprintf(stderr, "cannot load %s\n", path.c_str());
        return 1;
    }
    sound.setLooping(true);
    std::vector<SoLoud::handle> voices;
    for (unsigned int i = 0; i < gOptions.burstVoices; ++i)
        voices.push_back(soloud.play(sound, 0.2f));
    std::printf("control-call burst bench: %u in-memory voices (64 audible), %u calls "
                "per burst, quantum %u frames @ %u Hz, render-ahead %u (device period %u), "
                "%u rounds, audio mutex held during the burst: %s\n",
                gOptions.burstVoices, gOptions.burst, gOptions.buffer, gOptions.sampleRate,
                gOptions.renderAhead, gOptions.devicePeriod, gOptions.burstRounds,
                gOptions.burstHoldMs > 0 ? "yes" : "no");

    std::atomic<bool> running{true};
    std::vector<BlockRecord> blocks;
    blocks.reserve(1 << 16);
    std::vector<float> out(std::max(gOptions.buffer, gOptions.devicePeriod) * 2);
    std::thread mixer([&] {
        auto mixOne = [&](float *dst) {
            BlockRecord b{};
            b.start = nowUs();
            soloud.mix(dst, gOptions.buffer);
            b.end = nowUs();
            if (blocks.size() < blocks.capacity())
                blocks.push_back(b);
        };
        const bool ring = soloud.isRenderAheadEnabled();
        const unsigned int period = ring ? gOptions.devicePeriod : gOptions.buffer;
        const auto periodDuration = std::chrono::duration_cast<Clock::duration>(
            std::chrono::duration<double>((double)period / gOptions.sampleRate));
        auto next = Clock::now();
        while (running.load(std::memory_order_acquire)) {
            std::this_thread::sleep_until(next);
            if (ring) {
                while (soloud.mRenderRing.availableToRead() < gOptions.renderAhead &&
                       soloud.mRenderRing.availableToWrite() >= gOptions.buffer) {
                    mixOne(soloud.mRenderRingStaging.mData);
                    soloud.mRenderRing.write(soloud.mRenderRingStaging.mData, gOptions.buffer);
                }
                soloud.mRenderRing.read(out.data(), period);
            } else {
                mixOne(out.data());
            }
            next += periodDuration;
            if (Clock::now() > next + periodDuration)
                next = Clock::now();
        }
    });

    struct Round {
        double start, end;  // burst
        double worstCall;
        double activeCount, stop, play;
    };
    std::vector<Round> rounds;
    for (unsigned int r = 0; r < gOptions.burstRounds; ++r) {
        std::this_thread::sleep_for(std::chrono::milliseconds(300));
        std::atomic<bool> held{false}, burstDone{false};
        std::thread holder;
        if (gOptions.burstHoldMs > 0) {
            holder = std::thread([&] {
                soloud.lockAudioMutex_internal();
                held.store(true);
                const double until = nowUs() + gOptions.burstHoldMs * 1000.0;
                while (!burstDone.load() && nowUs() < until)
                    std::this_thread::sleep_for(std::chrono::microseconds(100));
                soloud.unlockAudioMutex_internal();
            });
            while (!held.load())
                std::this_thread::sleep_for(std::chrono::microseconds(100));
        }
        Round round{};
        round.start = nowUs();
        const unsigned int voiceCount = (unsigned int)voices.size();
        for (unsigned int i = 0; i < gOptions.burst; ++i) {
            const SoLoud::handle h = voices[i % voiceCount];
            const unsigned int pass = i / voiceCount;
            const double t0 = nowUs();
            switch (pass % 3) {
            case 0: soloud.setVolume(h, 0.1f + 0.05f * (float)((pass / 3) % 5)); break;
            case 1: soloud.setPan(h, (pass / 3) % 2 ? 0.3f : -0.3f); break;
            default: soloud.setPause(h, (pass / 3) % 2 == 0); break;
            }
            round.worstCall = std::max(round.worstCall, nowUs() - t0);
        }
        round.end = nowUs();
        burstDone.store(true);
        if (holder.joinable())
            holder.join();
        double t0 = nowUs();
        static volatile unsigned int sink = 0;
        sink = sink + soloud.getActiveVoiceCount();
        round.activeCount = nowUs() - t0;
        t0 = nowUs();
        soloud.stop(voices[0]);
        round.stop = nowUs() - t0;
        t0 = nowUs();
        voices[0] = soloud.play(sound, 0.2f);
        round.play = nowUs() - t0;
        rounds.push_back(round);
        std::this_thread::sleep_for(std::chrono::seconds(1));
        for (SoLoud::handle h : voices)
            soloud.setPause(h, false);
    }
    running.store(false, std::memory_order_release);
    mixer.join();

    std::printf("\n  round   burst ms  worst call ms  getActiveVoiceCount ms  stop ms  play ms  "
                "longest block ms\n");
    Round worst{};
    double worstBlock = 0;
    for (size_t r = 0; r < rounds.size(); ++r) {
        const Round &round = rounds[r];
        double longest = 0;
        for (const BlockRecord &b : blocks)
            if (b.end >= round.start && b.start <= round.end + 1e6)
                longest = std::max(longest, b.end - b.start);
        std::printf("  %5zu  %9.1f  %13.3f  %22.3f  %7.3f  %7.3f  %16.3f\n", r + 1,
                    (round.end - round.start) / 1000.0, round.worstCall / 1000.0,
                    round.activeCount / 1000.0, round.stop / 1000.0, round.play / 1000.0,
                    longest / 1000.0);
        worst.end = std::max(worst.end, round.end - round.start);
        worst.worstCall = std::max(worst.worstCall, round.worstCall);
        worst.activeCount = std::max(worst.activeCount, round.activeCount);
        worst.stop = std::max(worst.stop, round.stop);
        worst.play = std::max(worst.play, round.play);
        worstBlock = std::max(worstBlock, longest);
    }
    std::printf("  max    %9.1f  %13.3f  %22.3f  %7.3f  %7.3f  %16.3f\n", worst.end / 1000.0,
                worst.worstCall / 1000.0, worst.activeCount / 1000.0, worst.stop / 1000.0,
                worst.play / 1000.0, worstBlock / 1000.0);
    soloud.deinit();
    return 0;
}

}  // namespace

int main(int argc, char **argv) {
    if (!parseArgs(argc, argv)) return 2;
    gDiskDelayMs = gOptions.diskDelayMs;
    gDiskDelayProb = gOptions.diskDelayProb;

    SoLoud::Soloud soloud;
    if (gOptions.renderAhead > 0)
        soloud.setRenderAheadConfig(gOptions.devicePeriod, gOptions.renderAhead);
    if (soloud.init(SoLoud::Soloud::CLIP_ROUNDOFF, SoLoud::Soloud::NULLDRIVER,
                    gOptions.sampleRate, gOptions.buffer, 2) !=
        SoLoud::SO_NO_ERROR) {
        std::fprintf(stderr, "engine init failed\n");
        return 1;
    }
    soloud.setMaxActiveVoiceCount(64);
    if (gOptions.burst > 0)
        return runBurst(soloud);

    // ---- Load sounds -------------------------------------------------------
    struct DiskAsset { Format format; const char *file; };
    const DiskAsset diskAssets[] = {
        {Format::MP3, "8_bit_mentality.mp3"},
        {Format::OGG, "sample-vorbis.ogg"},
        {Format::FLAC, "sample-FLAC.flac"},
        {Format::WAV, "sample-WAV.wav"},
    };
    std::vector<TimedDiskFile *> files;
    std::vector<TimedWavStream *> streams;
    std::vector<Voice> voices;

    for (const DiskAsset &asset : diskAssets) {
        for (unsigned int k = 0; k < gOptions.streamsPerFormat; ++k) {
            auto *file = new TimedDiskFile();
            const std::string path = std::string(gOptions.assets) + "/" + asset.file;
            if (!file->open(path)) {
                std::fprintf(stderr, "cannot open %s\n", path.c_str());
                return 1;
            }
            auto *stream = new TimedWavStream(asset.format);
            if (stream->loadFile(file) != SoLoud::SO_NO_ERROR) {
                std::fprintf(stderr, "cannot stream %s\n", path.c_str());
                return 1;
            }
            stream->setLooping(true);
            files.push_back(file);
            streams.push_back(stream);
            const double length =
                stream->mBaseSamplerate > 0
                    ? stream->mSampleCount / stream->mBaseSamplerate
                    : 0;
            const SoLoud::handle h = soloud.play(*stream, 0.2f);
            voices.push_back({h, asset.format, length});
        }
    }

    std::vector<TimedNativeStream *> nativeStreams;
    if (gOptions.m4a) {
        for (unsigned int k = 0; k < gOptions.streamsPerFormat; ++k) {
            auto *stream = new TimedNativeStream();
            if (stream->load(gOptions.m4a) != SoLoud::SO_NO_ERROR) {
                std::fprintf(stderr, "cannot stream %s with NativeStream\n", gOptions.m4a);
                return 1;
            }
            stream->setLooping(true);
            nativeStreams.push_back(stream);
            const SoLoud::handle h = soloud.play(*stream, 0.2f);
            voices.push_back({h, Format::M4A, stream->getLength()});
        }
    }

    TimedWav memorySound;
    {
        const std::string path = std::string(gOptions.assets) + "/sample-WAV.wav";
        if (memorySound.load(path.c_str()) != SoLoud::SO_NO_ERROR) {
            std::fprintf(stderr, "cannot load %s\n", path.c_str());
            return 1;
        }
        memorySound.setLooping(true);
    }
    for (unsigned int k = 0; k < gOptions.memoryVoices; ++k) {
        const SoLoud::handle h = soloud.play(memorySound, 0.2f);
        voices.push_back({h, Format::MEMORY,
                          memorySound.mSampleCount / memorySound.mBaseSamplerate});
    }

    std::printf("control-call latency bench: %u disk voices (%u per format: "
                "mp3 ogg flac wav%s), %u memory voices, quantum %u frames @ %u Hz "
                "(%.1f ms), render-ahead %u (device period %u), disk stall %.1f ms "
                "@ p=%.3f, %.0f s\n",
                (unsigned)(voices.size() - gOptions.memoryVoices),
                gOptions.streamsPerFormat, gOptions.m4a ? " m4a" : "",
                gOptions.memoryVoices,
                gOptions.buffer, gOptions.sampleRate,
                1000.0 * gOptions.buffer / gOptions.sampleRate,
                gOptions.renderAhead, gOptions.devicePeriod,
                gOptions.diskDelayMs, gOptions.diskDelayProb, gOptions.seconds);

    // ---- Mixer thread ------------------------------------------------------
    std::atomic<bool> running{true};
    std::vector<BlockRecord> blocks;
    blocks.reserve((size_t)(gOptions.seconds * gOptions.sampleRate / 64) + 64);
    std::vector<float> out(std::max(gOptions.buffer, gOptions.devicePeriod) * 2);
    unsigned long long underruns = 0;

    std::thread mixer([&] {
        gMixerThreadId = std::this_thread::get_id();
        auto mixOne = [&](float *dst) {
            const ThreadTimes before = tTimes;
            BlockRecord b;
            b.start = nowUs();
            soloud.mix(dst, gOptions.buffer);
            b.end = nowUs();
            b.diskGetAudio = tTimes.diskGetAudio - before.diskGetAudio;
            b.diskIo = tTimes.diskIo - before.diskIo;
            b.memGetAudio = tTimes.memGetAudio - before.memGetAudio;
            b.seekWork = tTimes.seekWork - before.seekWork;
            blocks.push_back(b);
        };
        const bool ring = soloud.isRenderAheadEnabled();
        const unsigned int period = ring ? gOptions.devicePeriod : gOptions.buffer;
        const auto periodDuration = std::chrono::duration<double>(
            (double)period / gOptions.sampleRate);
        auto next = Clock::now();
        while (running.load(std::memory_order_acquire)) {
            std::this_thread::sleep_until(next);
            if (ring) {
                // Same loop as Soloud::renderRingTopUp_internal(), timed per
                // quantum, then the device read.
                while (soloud.mRenderRing.availableToRead() < gOptions.renderAhead &&
                       soloud.mRenderRing.availableToWrite() >= gOptions.buffer) {
                    mixOne(soloud.mRenderRingStaging.mData);
                    soloud.mRenderRing.write(soloud.mRenderRingStaging.mData,
                                             gOptions.buffer);
                }
                if (soloud.mRenderRing.read(out.data(), period) < period)
                    ++underruns;
            } else {
                mixOne(out.data());
            }
            next += std::chrono::duration_cast<Clock::duration>(periodDuration);
            if (Clock::now() > next + std::chrono::duration_cast<Clock::duration>(periodDuration)) {
                ++underruns;  // fell more than a period behind real time
                next = Clock::now();
            }
        }
    });

    // ---- Control threads ---------------------------------------------------
    // "ui": a random control call every 2-8 ms, seeks included (sliders,
    // buttons). "poller": a position/volume poll every --poll-ms, as a
    // progress bar or a volume automation running on another isolate or
    // thread does.
    const double deadline = nowUs() + gOptions.seconds * 1e6;
    auto doCall = [&](Op op, const Voice &v, double seekTarget, float value,
                      int thread, std::vector<CallRecord> &out) {
        static thread_local volatile double sink = 0;
        const ThreadTimes before = tTimes;
        CallRecord c;
        c.op = op;
        c.format = v.format;
        c.thread = thread;
        c.start = nowUs();
        switch (op) {
        case Op::SetVolume: soloud.setVolume(v.handle, value); break;
        case Op::GetVolume: sink = sink + soloud.getVolume(v.handle); break;
        case Op::GetStreamPosition: sink = sink + soloud.getStreamPosition(v.handle); break;
        case Op::Seek: soloud.seek(v.handle, seekTarget); break;
        case Op::IsValidVoiceHandle: sink = sink + soloud.isValidVoiceHandle(v.handle); break;
        case Op::SetPan: soloud.setPan(v.handle, value - 0.2f); break;
        case Op::GetPan: sink = sink + soloud.getPan(v.handle); break;
        case Op::SetPause: soloud.setPause(v.handle, false); break;
        case Op::GetPause: sink = sink + soloud.getPause(v.handle); break;
        default: break;
        }
        c.end = nowUs();
        c.seekWorkOnCaller = tTimes.seekWork - before.seekWork;
        out.push_back(c);
    };
    std::vector<CallRecord> uiCalls, pollCalls;
    uiCalls.reserve((size_t)(gOptions.seconds * 1000) + 64);
    pollCalls.reserve((size_t)(gOptions.seconds * 1000) + 64);
    std::thread ui([&] {
        std::mt19937 rng(42);
        std::uniform_int_distribution<int> voicePick(0, (int)voices.size() - 1);
        std::uniform_int_distribution<int> opPick(0, (int)Op::Count - 1);
        std::uniform_int_distribution<int> sleepPick(gOptions.minIntervalMs,
                                                     gOptions.maxIntervalMs);
        std::uniform_real_distribution<double> unit(0.0, 1.0);
        while (nowUs() < deadline) {
            std::this_thread::sleep_for(std::chrono::milliseconds(sleepPick(rng)));
            const Voice &v = voices[voicePick(rng)];
            const double seekTarget = unit(rng) * std::max(0.0, v.length - 2.0);
            doCall((Op)opPick(rng), v, seekTarget, 0.1f + 0.2f * (float)unit(rng),
                   0, uiCalls);
        }
    });
    std::thread poller([&] {
        if (gOptions.pollMs <= 0) return;
        const Op pollOps[] = {Op::GetStreamPosition, Op::IsValidVoiceHandle,
                              Op::GetVolume, Op::SetVolume};
        unsigned int tick = 0;
        while (nowUs() < deadline) {
            std::this_thread::sleep_for(std::chrono::microseconds(
                (long long)(gOptions.pollMs * 1000)));
            const Voice &v = voices[tick % voices.size()];
            doCall(pollOps[tick % 4], v, 0, 0.2f, 1, pollCalls);
            ++tick;
        }
    });

    ui.join();
    poller.join();
    running.store(false, std::memory_order_release);
    mixer.join();

    std::vector<CallRecord> calls = uiCalls;
    calls.insert(calls.end(), pollCalls.begin(), pollCalls.end());
    std::sort(calls.begin(), calls.end(),
              [](const CallRecord &a, const CallRecord &b) { return a.start < b.start; });

    // ---- Report --------------------------------------------------------------
    std::printf("\nControl calls (wall time on the calling thread, both control "
                "threads):\n");
    std::vector<double> all;
    for (int o = 0; o < (int)Op::Count; ++o) {
        std::vector<double> us;
        for (const CallRecord &c : calls)
            if ((int)c.op == o) us.push_back(c.end - c.start);
        all.insert(all.end(), us.begin(), us.end());
        printDist(opName((Op)o), us);
    }
    printDist("all", all);
    {
        std::vector<double> diskSeek, memSeek, poll;
        for (const CallRecord &c : calls) {
            if (c.thread == 1) poll.push_back(c.end - c.start);
            if (c.op != Op::Seek) continue;
            (c.format == Format::MEMORY ? memSeek : diskSeek).push_back(c.end - c.start);
        }
        printDist("seek (disk voice)", diskSeek);
        printDist("seek (memory voice)", memSeek);
        printDist("poller thread calls", poll);
    }

    // The mixer's own lock wait: a block that started while a control call
    // was doing source seek work under the mutex waited for that call.
    std::vector<double> blockWait(blocks.size(), 0.0);
    for (size_t k = 0; k < blocks.size(); ++k) {
        const BlockRecord &b = blocks[k];
        for (const CallRecord &c : calls) {
            if (c.start > b.start) break;
            if (c.seekWorkOnCaller > 0 && c.end > b.start)
                blockWait[k] = std::max(blockWait[k], std::min(c.end, b.end) - b.start);
        }
    }

    std::vector<double> hold, waitInMix, disk, io, mem, seekInMix;
    double sumHold = 0, sumDisk = 0, sumIo = 0, sumMem = 0, sumSeek = 0;
    for (size_t k = 0; k < blocks.size(); ++k) {
        const BlockRecord &b = blocks[k];
        const double d = b.end - b.start - blockWait[k];
        hold.push_back(d);
        if (blockWait[k] > 0) waitInMix.push_back(blockWait[k]);
        disk.push_back(b.diskGetAudio);
        io.push_back(b.diskIo);
        mem.push_back(b.memGetAudio);
        if (b.seekWork > 0) seekInMix.push_back(b.seekWork);
        sumHold += d;
        sumDisk += b.diskGetAudio;
        sumIo += b.diskIo;
        sumMem += b.memGetAudio;
        sumSeek += b.seekWork;
    }
    std::printf("\nMix blocks (one Soloud::mix() per quantum; the audio mutex is "
                "held for all of it but the final clip/interlace):\n");
    printDist("mutex hold per block", hold);
    printDist("  disk getAudio", disk);
    printDist("    of which file I/O", io);
    printDist("  memory getAudio", mem);
    printDist("  seeks applied in mix", seekInMix);
    printDist("mixer waited for a seek", waitInMix);
    if (sumHold > 0) {
        std::printf("  share of hold time: disk getAudio %.1f%% (file I/O %.1f%%), "
                    "memory getAudio %.1f%%, seeks %.1f%%, rest %.1f%%\n",
                    100 * sumDisk / sumHold, 100 * sumIo / sumHold,
                    100 * sumMem / sumHold, 100 * sumSeek / sumHold,
                    100 * (sumHold - sumDisk - sumMem - sumSeek) / sumHold);
    }
    std::printf("  blocks %zu, late wake-ups/underruns %llu\n", blocks.size(), underruns);

    // Wait attribution. While a call waits for the audio mutex, someone else
    // holds it: the mixer (a block's hold, after its own wait for a seek) or
    // another control thread doing source seek work under it. Each waited
    // interval is split across the holds it overlaps; a call queued behind
    // several holders (a seek, then the mixer that was waiting for it) is
    // split across all of them.
    struct Interval { double start, end; const BlockRecord *block; Format format; };
    std::vector<Interval> holds;
    for (size_t k = 0; k < blocks.size(); ++k)
        holds.push_back({blocks[k].start + blockWait[k], blocks[k].end, &blocks[k],
                         Format::MEMORY});
    for (const CallRecord &s : calls)
        if (s.seekWorkOnCaller > 0)
            holds.push_back({s.end - s.seekWorkOnCaller, s.end, nullptr, s.format});
    double totalWait = 0, mixWait = 0, seekWait = 0, waitDisk = 0, waitIo = 0,
           waitMem = 0;
    size_t waited = 0;
    double seekWaitByFormat[kFormats] = {};
    std::vector<double> ownSeekWork;
    for (const CallRecord &c : calls) {
        if (c.seekWorkOnCaller > 0) ownSeekWork.push_back(c.seekWorkOnCaller);
        const double wait = c.end - c.start - c.seekWorkOnCaller;
        if (wait < 500) continue;  // under 0.5 ms: not a wait
        ++waited;
        totalWait += wait;
        const double w0 = c.start, w1 = c.start + wait;
        for (const Interval &h : holds) {
            if (h.end <= w0 || h.start >= w1) continue;
            if (!h.block && h.end == c.end) continue;  // its own seek
            const double o = std::min(h.end, w1) - std::max(h.start, w0);
            if (o <= 0) continue;
            if (h.block) {
                mixWait += o;
                const double d = h.end - h.start;
                if (d > 0) {
                    waitDisk += o * h.block->diskGetAudio / d;
                    waitIo += o * h.block->diskIo / d;
                    waitMem += o * h.block->memGetAudio / d;
                }
            } else {
                seekWait += o;
                seekWaitByFormat[(int)h.format] += o;
            }
        }
    }
    std::printf("\nWait attribution (calls slower than 0.5 ms, not counting their "
                "own seek work):\n");
    std::printf("  %zu of %zu calls waited, %.1f ms in total\n", waited,
                calls.size(), totalWait / 1000.0);
    std::printf("  held by the mixer (mix block): %.1f ms (%.1f%%)\n",
                mixWait / 1000.0, totalWait > 0 ? 100 * mixWait / totalWait : 0.0);
    if (mixWait > 0) {
        std::printf("    of which: disk getAudio %.1f%% (file I/O %.1f%%), memory "
                    "getAudio %.1f%%, rest of the mix %.1f%%\n",
                    100 * waitDisk / mixWait, 100 * waitIo / mixWait,
                    100 * waitMem / mixWait,
                    100 * (mixWait - waitDisk - waitMem) / mixWait);
    }
    std::printf("  held by seek() on another control thread: %.1f ms (%.1f%%)",
                seekWait / 1000.0, totalWait > 0 ? 100 * seekWait / totalWait : 0.0);
    for (int f = 0; f < kFormats; ++f)
        if (seekWaitByFormat[f] > 0)
            std::printf(" [%s %.1f ms]", formatName((Format)f), seekWaitByFormat[f] / 1000.0);
    std::printf("\n  not covered by a recorded hold: %.1f ms\n",
                std::max(0.0, totalWait - mixWait - seekWait) / 1000.0);
    printDist("seek work on caller", ownSeekWork);

    std::printf("\nSource seek() work by format (where it ran; %llu loop "
                "restarts excluded):\n", gLoopRestarts.load());
    for (int f = 0; f <= (int)Format::MEMORY; ++f) {
        for (int dir = 0; dir < 2; ++dir) {
            for (int onMixer = 0; onMixer < 2; ++onMixer) {
                std::vector<double> us;
                for (const SeekRecord &s : gSeekRecords)
                    if ((int)s.format == f && (int)s.backward == dir &&
                        (int)s.onMixerThread == onMixer)
                        us.push_back(s.us);
                if (us.empty()) continue;
                char label[64];
                std::snprintf(label, sizeof(label), "%s %s (%s)",
                              formatName((Format)f), dir ? "backward" : "forward",
                              onMixer ? "mixer" : "caller");
                printDist(label, us);
            }
        }
    }

    std::printf("\nStreamed audio produced (seconds per voice over the %.0f s run; "
                "voices alive and unpaused at the end):\n", gOptions.seconds);
    for (int f = 0; f < (int)Format::MEMORY; ++f) {
        unsigned int count = 0, alive = 0;
        for (const Voice &v : voices) {
            if ((int)v.format != f) continue;
            ++count;
            if (soloud.isValidVoiceHandle(v.handle) && !soloud.getPause(v.handle)) ++alive;
        }
        if (count == 0) continue;
        std::printf("  %-5s %6.2f s  %u/%u alive\n", formatName((Format)f),
                    gFramesByFormat[f].load() / (double)gOptions.sampleRate / count, alive, count);
    }

    soloud.stopAll();
    soloud.deinit();
    for (auto *s : streams) delete s;
    for (auto *s : nativeStreams) delete s;
    for (auto *f : files) delete f;
    return 0;
}
