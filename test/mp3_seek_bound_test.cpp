// Regression test: a WavStream seek never does unbounded decoding inside a mix.
//
// The mixer applies a voice's queued seek at the start of a mix block, and
// restarts a looping voice with a seek in the middle of one, under the audio
// mutex: every voice waits for it. Some WavStream seeks decode their way to
// the target, so on a long file one seek could stall the mix for tens to
// hundreds of milliseconds and underrun the device:
// - MP3 without a seek table: dr_mp3 decodes from the start of the stream;
// - MP3 whose table seek does not line up with the stream: decoded from the
//   start too;
// - FLAC without a usable seek table, Opus in Ogg and ADPCM WAV the same way.
// WavStreamInstance::seek() now only positions the decoder (bounded work)
// and getAudio() decodes on to the target at most a fixed budget per call,
// playing silence until it gets there (see soloud_wavstream.cpp). This test
// checks, on a long MP3 built at run time from a repo asset (and on FLAC,
// Opus and ADPCM files when the run script could make them), that:
// - the worst single getAudio() and mix() call around a seek stays under
//   5 ms of the calling thread's CPU time, with the seek table, without it,
//   and with it broken;
// - with the per-call time cap lowered to 30 us (gWavStreamSeekCallCapUs),
//   seeks in every format still land sample exact, and the worst getAudio()
//   stays under the cap plus two units of decoding (see decodeUnitMs()),
//   with a margin;
// - playback then starts sample-exact at the target, versus decoding the
//   file from the start without seeking;
// - the voice reports the target as its position until the audio from there
//   starts, and advances from there;
// - a new seek while one is still pending, a seek past the end, a looping
//   voice with a far loop point and a loop end point all behave;
// - a mixer thread with seeks from another thread (for ThreadSanitizer).
//
// Null backend, no audio hardware. Build and run from the flutter_soloud
// repository root with:
//
//   ./test/run_mp3_seek_bound_test.sh          (add --tsan for ThreadSanitizer)

#include "dr_mp3.h"
#include "soloud.h"
#include "soloud_wavstream.h"

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <fstream>
#include <string>
#include <thread>
#include <time.h>
#include <vector>

#if defined(__has_feature)
#if __has_feature(thread_sanitizer) || __has_feature(address_sanitizer)
#define SANITIZED 1
#endif
#endif
#if defined(__SANITIZE_THREAD__) || defined(__SANITIZE_ADDRESS__)
#define SANITIZED 1
#endif
#ifndef SANITIZED
#define SANITIZED 0
#endif

namespace {

constexpr unsigned int kBlock = 512;   // engine buffer = SAMPLE_GRANULARITY: one source read per block
constexpr double kMaxCallMs = 5.0;     // acceptance bound per getAudio()/mix() call
constexpr unsigned int kMp3Budget = 65536;  // WavStreamInstance::seekSkipBudget_internal() for MP3
// kSeekChunkFrames in soloud_wavstream.cpp: the least a call with seek work
// pending decodes (the time cap stops it after one chunk at the earliest),
// so the most silent calls a seek can take is about frames / kSeekChunk.
constexpr unsigned int kSeekChunk = 2048;
// testSmallCap(): the lowered time cap, and the margin of the bound on the
// worst getAudio(). That bound is the cap plus two units of decoding (the
// unit the call ran past the cap with, as the cap is checked after each,
// and the call's own 512 frames once the seek is done), each counted
// kDecodeSlack times, as the calibration and the seek can run on cores of
// different speeds (hybrid CPUs), plus kSmallCapMarginMs for the clock reads,
// the output conversion and noise.
constexpr unsigned int kSmallCapUs = 30;
constexpr double kDecodeSlack = 2.0;
constexpr double kSmallCapMarginMs = 0.25;
constexpr unsigned int kCompare = 8 * kBlock;  // frames compared after a landing

int gFailures = 0;
int gAssertions = 0;
double gWorstGetAudioMs = 0;
double gWorstMixMs = 0;

#define EXPECT(condition, format, ...) do { \
    ++gAssertions; \
    if (!(condition)) { \
        ++gFailures; \
        std::fprintf(stderr, "  FAIL [%s:%d] " format "\n", __FILE__, __LINE__, ##__VA_ARGS__); \
    } \
} while (0)

// Timing bounds are asserted in plain builds only: sanitizers slow decoding
// down several times.
#define EXPECT_TIME(ms, format, ...) do { \
    if (!SANITIZED) EXPECT((ms) < kMaxCallMs, format, ##__VA_ARGS__); \
} while (0)

using Clock = std::chrono::steady_clock;

// Calls are timed in the calling thread's CPU time: the bound is on the
// decoding work a call does, and a busy machine (parallel builds, CI)
// preempting the thread must not fail the test.
double threadCpuMs() {
    timespec ts;
    clock_gettime(CLOCK_THREAD_CPUTIME_ID, &ts);
    return ts.tv_sec * 1e3 + ts.tv_nsec / 1e6;
}
double msSince(double t0) { return threadCpuMs() - t0; }

std::vector<unsigned char> readFile(const std::string &path) {
    std::ifstream in(path, std::ios::binary);
    return std::vector<unsigned char>((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
}

// ---- A long MP3, built from a repo asset --------------------------------------

// Length in bytes of the MPEG audio Layer III frame whose header is at p, or 0.
size_t mp3FrameLength(const unsigned char *p) {
    if (p[0] != 0xFF || (p[1] & 0xE0) != 0xE0)
        return 0;
    const int version = (p[1] >> 3) & 3;  // 3: MPEG-1, 2: MPEG-2, 0: MPEG-2.5
    const int layer = (p[1] >> 1) & 3;    // 1: Layer III
    const int bitrateIndex = p[2] >> 4, rateIndex = (p[2] >> 2) & 3, padding = (p[2] >> 1) & 1;
    if (version == 1 || layer != 1 || bitrateIndex == 0 || bitrateIndex == 15 || rateIndex == 3)
        return 0;
    static const int kBitrate1[] = {0, 32, 40, 48, 56, 64, 80, 96, 112, 128, 160, 192, 224, 256, 320};
    static const int kBitrate2[] = {0, 8, 16, 24, 32, 40, 48, 56, 64, 80, 96, 112, 128, 144, 160};
    static const int kRate[] = {44100, 48000, 32000};
    const int rate = kRate[rateIndex] >> (version == 3 ? 0 : version == 2 ? 1 : 2);
    const int bitrate = (version == 3 ? kBitrate1 : kBitrate2)[bitrateIndex] * 1000;
    return size_t((version == 3 ? 144 : 72) * bitrate / rate + padding);
}

// The audio frames of an MP3 file repeated `copies` times: the ID3 tags and
// the Xing/Info frame dropped (its frame count and encoder delay are those of
// one copy), so dr_mp3 sees one long plain stream. Every copy starts with an
// empty bit reservoir, as the original did, so it decodes cleanly.
std::vector<unsigned char> repeatMp3(const std::vector<unsigned char> &file, int copies) {
    size_t pos = 0;
    if (file.size() > 10 && std::memcmp(file.data(), "ID3", 3) == 0) {
        const size_t size = (size_t(file[6] & 0x7f) << 21) | (size_t(file[7] & 0x7f) << 14) |
                            (size_t(file[8] & 0x7f) << 7) | size_t(file[9] & 0x7f);
        pos = 10 + size + ((file[5] & 0x10) ? 10 : 0);
    }
    std::vector<std::pair<size_t, size_t>> frames;
    while (pos + 4 <= file.size()) {
        const size_t length = mp3FrameLength(&file[pos]);
        if (length == 0 || pos + length > file.size())
            break;
        frames.emplace_back(pos, length);
        pos += length;
    }
    if (!frames.empty()) {
        const auto first = file.begin() + long(frames[0].first);
        const auto end = first + long(frames[0].second);
        const char *tags[] = {"Xing", "Info", "VBRI"};
        for (const char *tag : tags)
            if (std::search(first, end, tag, tag + 4) != end) {
                frames.erase(frames.begin());
                break;
            }
    }
    std::vector<unsigned char> out;
    for (int c = 0; c < copies; ++c)
        for (const auto &frame : frames)
            out.insert(out.end(), file.begin() + long(frame.first), file.begin() + long(frame.first + frame.second));
    return out;
}

// ---- Sources and reference decoding --------------------------------------------

enum class Mp3Table { Intact, None, Broken };
const char *tableName(Mp3Table t) {
    return t == Mp3Table::Intact ? "seek table" : t == Mp3Table::None ? "no seek table" : "broken seek table";
}

struct Source {
    std::string name;
    std::vector<unsigned char> data;  // kept alive: loadMem does not copy
    SoLoud::WavStream stream;
    unsigned int frames = 0;
    unsigned int channels = 0;
    float rate = 0;

    bool load(const std::string &aName, std::vector<unsigned char> aData) {
        name = aName;
        data = std::move(aData);
        if (data.empty() || stream.loadMem(data.data(), (unsigned int)data.size(), false, false) != SoLoud::SO_NO_ERROR)
            return false;
        frames = stream.mSampleCount;
        channels = stream.mChannels;
        rate = stream.mBaseSamplerate;
        return frames > 0;
    }

    // Test hook for the MP3 fallbacks. Instances bind the table when they are
    // created, so call this before playing. None: as if building the table had
    // failed at load. Broken: every point's byte offset is one byte off, so a
    // seek through the table never lines up with the stream and falls back to
    // decoding from the start.
    void setMp3Table(Mp3Table table) {
        if (table == Mp3Table::None) {
            delete[] stream.mMp3SeekPoints;
            stream.mMp3SeekPoints = nullptr;
            stream.mMp3SeekPointCount = 0;
        } else if (table == Mp3Table::Broken) {
            for (unsigned int i = 0; i < stream.mMp3SeekPointCount; ++i)
                stream.mMp3SeekPoints[i].seekPosInBytes += 1;
        }
    }
};

// Channel-planar frames of a source decoded from the start without seeking,
// for the windows asked for (one sequential pass for all of them).
class Reference {
public:
    explicit Reference(Source &source) : source_(source) {}
    void want(unsigned int from, unsigned int count) {
        windows_.push_back({from, std::min(count, source_.frames - std::min(from, source_.frames)), {}});
    }
    void decode() {
        std::sort(windows_.begin(), windows_.end(), [](const Window &a, const Window &b) { return a.from < b.from; });
        unsigned int end = 0;
        for (const Window &w : windows_)
            end = std::max(end, w.from + w.count);
        SoLoud::AudioSourceInstance *inst = source_.stream.createInstance();
        inst->init(source_.stream, 0);
        std::vector<float> block(kBlock * MAX_CHANNELS);
        for (Window &w : windows_)
            w.data.assign(size_t(w.count) * source_.channels, 0.0f);
        unsigned int at = 0;
        while (at < end) {
            const unsigned int n = inst->getAudio(block.data(), kBlock, kBlock);
            for (Window &w : windows_)
                for (unsigned int i = 0; i < n; ++i)
                    if (at + i >= w.from && at + i < w.from + w.count)
                        for (unsigned int c = 0; c < source_.channels; ++c)
                            w.data[size_t(c) * w.count + (at + i - w.from)] = block[c * kBlock + i];
            at += n;
            if (n < kBlock)
                break;
        }
        delete inst;
    }
    // Sample of channel c at frame f (inside a window asked for).
    float at(unsigned int f, unsigned int c) const {
        for (const Window &w : windows_)
            if (f >= w.from && f < w.from + w.count)
                return w.data[size_t(c) * w.count + (f - w.from)];
        std::fprintf(stderr, "reference frame %u not decoded\n", f);
        std::abort();
    }
    bool silentAt(unsigned int f) const {
        for (unsigned int i = 0; i < kBlock && f + i < source_.frames; ++i)
            for (unsigned int c = 0; c < source_.channels; ++c)
                if (at(f + i, c) != 0.0f)
                    return false;
        return true;
    }

private:
    struct Window {
        unsigned int from;
        unsigned int count;
        std::vector<float> data;
    };
    Source &source_;
    std::vector<Window> windows_;
};

// A target near `frame` whose first block is not digital silence, so the
// first block from the target tells itself apart from the silence played
// while a seek is pending.
unsigned int audibleTarget(Source &source, unsigned int frame) {
    Reference probe(source);
    for (unsigned int k = 0; k < 64; ++k)
        probe.want(frame + k * 4096, kBlock);
    probe.decode();
    for (unsigned int k = 0; k < 64; ++k)
        if (!probe.silentAt(frame + k * 4096))
            return frame + k * 4096;
    return frame;
}

double positionOf(Source &source, unsigned int frame) {
    // As WavStreamInstance::seek() reports it.
    return float(frame / source.rate);
}

// Seek time that lands on `frame` (seek() floors seconds * rate), and a loop
// end time that ends a loop before it (the mixer rounds that one up).
double seekTime(Source &source, unsigned int frame) { return (frame + 0.5) / double(source.rate); }
double loopEndTime(Source &source, unsigned int frame) { return (frame - 0.5) / double(source.rate); }

// Most silent calls (or blocks) a seek with `frames` frames of decoding to do
// can take: one chunk per call at least, plus a few calls of MP3 seek-table
// sync (one MP3 frame per call at least).
unsigned int maxSilentCalls(unsigned int frames) { return frames / kSeekChunk + 64; }

// Sets the per-call time cap of WavStream seeks for a scope.
struct SeekCap {
    explicit SeekCap(unsigned int us) { SoLoud::gWavStreamSeekCallCapUs.store(us); }
    ~SeekCap() { SoLoud::gWavStreamSeekCallCapUs.store(WAVSTREAM_SEEK_CALL_CAP_US); }
};

// ---- Instance level: getAudio() calls around a seek -----------------------------

// Seeks a fresh instance (after some playback) to `target` and calls getAudio()
// until the audio from the target has played for kCompare frames. Checks the
// worst call, the silence before the target, the samples from there and the
// reported position. Returns the number of silent calls. With
// getAudioBoundMs, the worst getAudio() call alone (seek() not counted) must
// stay under it.
unsigned int checkInstanceSeek(Source &source, const Reference &ref, unsigned int playFirst, unsigned int target,
                               const char *what, double getAudioBoundMs = 0) {
    SoLoud::AudioSourceInstance *inst = source.stream.createInstance();
    inst->init(source.stream, 0);
    std::vector<float> block(kBlock * MAX_CHANNELS);
    for (unsigned int played = 0; played < playFirst; played += kBlock)
        inst->getAudio(block.data(), kBlock, kBlock);

    std::vector<float> scratch(SAMPLE_GRANULARITY * MAX_CHANNELS);
    const double t0 = threadCpuMs();
    EXPECT(inst->seek(seekTime(source, target), scratch.data(), (unsigned int)scratch.size()) ==
               SoLoud::SO_NO_ERROR, "%s: seek failed", what);
    const double seekMs = msSince(t0);
    EXPECT_TIME(seekMs, "%s: seek() took %.2f ms", what, seekMs);
    double worst = seekMs;
    double worstGetAudio = 0;

    unsigned int silent = 0;
    unsigned int compared = 0;
    bool landed = false, exact = true, positionKept = true;
    const unsigned int maxCalls = 4 + maxSilentCalls(target + playFirst) + source.frames / 1000;
    for (unsigned int call = 0; call < maxCalls && compared < kCompare; ++call) {
        const double t1 = threadCpuMs();
        const unsigned int n = inst->getAudio(block.data(), kBlock, kBlock);
        const double ms = msSince(t1);
        worst = std::max(worst, ms);
        worstGetAudio = std::max(worstGetAudio, ms);
        bool zero = true;
        for (unsigned int c = 0; c < source.channels && zero; ++c)
            for (unsigned int i = 0; i < n && zero; ++i)
                zero = block[c * kBlock + i] == 0.0f;
        if (!landed && zero && n == kBlock) {
            ++silent;
            positionKept = positionKept && inst->mStreamPosition == positionOf(source, target);
            continue;
        }
        landed = true;
        for (unsigned int i = 0; i < n && compared < kCompare; ++i, ++compared)
            for (unsigned int c = 0; c < source.channels; ++c)
                if (block[c * kBlock + i] != ref.at(target + compared, c) && exact) {
                    exact = false;
                    std::fprintf(stderr, "  %s: frame %u (target + %u) ch %u: %.9g, expected %.9g\n", what,
                                 target + compared, compared, c, block[c * kBlock + i],
                                 ref.at(target + compared, c));
                }
        if (n < kBlock)
            break;
    }
    gWorstGetAudioMs = std::max(gWorstGetAudioMs, worst);
    if (getAudioBoundMs > 0) {
        std::printf("  %-48s worst getAudio %.3f ms (bound %.3f), seek() %.3f ms, %u silent calls\n", what,
                    worstGetAudio, getAudioBoundMs, seekMs, silent);
        if (!SANITIZED)
            EXPECT(worstGetAudio < getAudioBoundMs, "%s: worst getAudio %.3f ms, bound %.3f ms", what, worstGetAudio,
                   getAudioBoundMs);
    } else {
        std::printf("  %-48s worst call %.3f ms, %u silent calls\n", what, worst, silent);
    }
    EXPECT_TIME(worst, "%s: worst seek/getAudio call %.2f ms", what, worst);
    EXPECT(landed && compared == std::min(kCompare, source.frames - target), "%s: never played from the target", what);
    EXPECT(exact, "%s: not sample exact from the target", what);
    EXPECT(positionKept, "%s: the position left the target while seeking", what);
    delete inst;
    return silent;
}

// ---- Engine level: mix() calls ---------------------------------------------------

struct Rig {
    SoLoud::Soloud soloud;
    Source &source;
    std::vector<float> out = std::vector<float>(kBlock * 2);
    double worst = 0;

    explicit Rig(Source &s) : source(s) {
        // Same rate as the source, one source block per mix block and the
        // point resampler: the output is the source, sample for sample (the
        // linear one lags a frame). No round-off clipper, unity post-clip
        // scaler.
        const SoLoud::result r =
            soloud.init(0, SoLoud::Soloud::NULLDRIVER, (unsigned int)source.rate, kBlock, 2);
        EXPECT(r == SoLoud::SO_NO_ERROR, "init failed: %u", r);
        soloud.setPostClipScaler(1.0f);
        soloud.setMainResampler(SoLoud::Soloud::RESAMPLER_POINT);
    }
    ~Rig() {
        soloud.deinit();
        source.stream.mSoloud = nullptr;  // the source outlives this engine
    }

    SoLoud::handle play() {
        const SoLoud::handle h = soloud.play(source.stream);
        soloud.setPanAbsolute(h, 1.0f, 1.0f);  // unity channel gains
        mix();
        mix();  // the start-up gain ramp has settled
        return h;
    }
    void mix() {
        const double t0 = threadCpuMs();
        soloud.mix(out.data(), kBlock);
        const double ms = msSince(t0);
        worst = std::max(worst, ms);
        gWorstMixMs = std::max(gWorstMixMs, ms);
    }
    bool silent() const {
        for (float f : out)
            if (f != 0.0f)
                return false;
        return true;
    }
    // Output frame i equals source frame `frame` (clipped to [-1, 1] by the engine).
    bool matches(const Reference &ref, unsigned int i, unsigned int frame) const {
        for (unsigned int c = 0; c < 2; ++c) {
            const float want = std::max(-1.0f, std::min(1.0f, ref.at(frame, source.channels == 1 ? 0 : c)));
            if (out[i * 2 + c] != want)
                return false;
        }
        return true;
    }
};

// After soloud.seek(h, target): mixes until the voice plays from the target,
// checking the position on the way, and that the next kCompare frames are the
// source from the target. Returns the number of silent blocks.
unsigned int mixToTarget(Rig &rig, SoLoud::handle h, const Reference &ref, unsigned int target, const char *what,
                         unsigned int maxBlocks) {
    const double targetPosition = positionOf(rig.source, target);
    // Until the mixer applies it, the getters report the seek time asked
    // for; then the source's target frame, as before.
    EXPECT(std::fabs(rig.soloud.getStreamPosition(h) - targetPosition) < 1e-3,
           "%s: position right after seek %f, expected %f", what, rig.soloud.getStreamPosition(h), targetPosition);
    unsigned int silent = 0, played = 0;
    bool exact = true, positionKept = true, positionFollows = true;
    for (unsigned int b = 0; b < maxBlocks && played < kCompare; ++b) {
        rig.mix();
        if (played == 0 && rig.silent()) {
            ++silent;
            positionKept = positionKept && rig.soloud.getStreamPosition(h) == targetPosition;
            continue;
        }
        for (unsigned int i = 0; i < kBlock; ++i)
            exact = exact && rig.matches(ref, i, target + played + i);
        played += kBlock;
        // One source read per block, and the first one with audio played the
        // whole block from the target.
        const double expected = targetPosition + double(played) / rig.source.rate;
        if (positionFollows && std::fabs(rig.soloud.getStreamPosition(h) - expected) >= 1e-6) {
            positionFollows = false;
            std::fprintf(stderr, "  %s: position %.9f after %u frames from the target, expected %.9f\n", what,
                         rig.soloud.getStreamPosition(h), played, expected);
        }
    }
    std::printf("  %-48s worst mix %.3f ms, %u silent blocks\n", what, rig.worst, silent);
    EXPECT(played >= kCompare, "%s: never played from the target (%u silent blocks)", what, silent);
    EXPECT(exact, "%s: mix not sample exact from the target", what);
    EXPECT(positionKept, "%s: reported position left the target while seeking", what);
    EXPECT(positionFollows, "%s: position does not follow the audio from the target", what);
    return silent;
}

// ---- Tests -----------------------------------------------------------------------

// A seek with the table, without it and with it broken, at the instance level
// (worst getAudio) and in the engine (worst mix, position).
void testSeeks(Source &source, Mp3Table table, const std::vector<unsigned int> &targets) {
    std::printf("%s, %s: seeks\n", source.name.c_str(), tableName(table));
    Reference ref(source);
    for (unsigned int t : targets)
        ref.want(t, kCompare + kBlock);
    ref.decode();

    for (unsigned int t : targets) {
        char what[96];
        // From the start, and backward from further on.
        std::snprintf(what, sizeof what, "getAudio, seek to %.1f s", t / source.rate);
        const unsigned int silent = checkInstanceSeek(source, ref, 0, t, what);
        std::snprintf(what, sizeof what, "getAudio, seek back to %.1f s", t / source.rate);
        checkInstanceSeek(source, ref, std::min(source.frames - kBlock, t + 20 * kBlock), t, what);
        // With the table a seek is a table spacing plus a few MP3 frames of
        // decoding, well inside one call's budget and time cap; a call that
        // is preempted can run into the time cap, hence a little slack.
        if (table == Mp3Table::Intact)
            EXPECT(silent <= (SANITIZED ? maxSilentCalls(0) : 4u), "with the table: %u silent calls", silent);
        else
            EXPECT(silent <= maxSilentCalls(t), "%u silent calls for %u frames", silent, t);
    }

    Rig rig(source);
    const SoLoud::handle h = rig.play();
    for (unsigned int t : targets) {
        char what[96];
        std::snprintf(what, sizeof what, "mix, seek to %.1f s", t / source.rate);
        rig.worst = 0;
        const double t0 = threadCpuMs();
        EXPECT(rig.soloud.seek(h, seekTime(source, t)) == SoLoud::SO_NO_ERROR, "seek result");
        const double ms = msSince(t0);
        EXPECT(ms < kMaxCallMs, "%s: the seek call took %.2f ms", what, ms);
        mixToTarget(rig, h, ref, t, what, 8 + maxSilentCalls(t) + 2 * kCompare / kBlock);
        EXPECT_TIME(rig.worst, "%s: worst mix %.2f ms", what, rig.worst);
    }
}

// A new seek while one is still pending, backward and forward (without a
// table: the slowest path), and a seek past the end.
void testSeekWhilePending(Source &source) {
    std::printf("%s, %s: a new seek while one is pending, a seek past the end\n", source.name.c_str(),
                tableName(Mp3Table::None));
    const unsigned int far = audibleTarget(source, source.frames / 10 * 7);
    const unsigned int back = audibleTarget(source, source.frames / 20);
    const unsigned int ahead = audibleTarget(source, source.frames / 10 * 8);
    Reference ref(source);
    ref.want(back, kCompare + kBlock);
    ref.want(ahead, kCompare + kBlock);
    ref.decode();

    Rig rig(source);
    SoLoud::handle h = rig.play();
    // Backward: the second seek starts over from the start of the stream.
    rig.worst = 0;
    rig.soloud.seek(h, seekTime(source, far));
    for (int i = 0; i < 5; ++i)
        rig.mix();
    EXPECT(rig.silent(), "the far seek landed within 5 blocks");
    rig.soloud.seek(h, seekTime(source, back));
    unsigned int silent = mixToTarget(rig, h, ref, back, "seek back while pending", maxSilentCalls(back) + 32);
    EXPECT(silent <= maxSilentCalls(back), "the backward seek took %u silent blocks", silent);
    EXPECT_TIME(rig.worst, "worst mix %.2f ms", rig.worst);

    // Forward: decoding goes on from where the pending seek got to.
    rig.worst = 0;
    const unsigned int mid = audibleTarget(source, source.frames / 2);
    rig.soloud.seek(h, seekTime(source, mid));
    const unsigned int steps = 5;
    for (unsigned int i = 0; i < steps; ++i)
        rig.mix();
    rig.soloud.seek(h, seekTime(source, ahead));
    // The decoder is somewhere between `back` and `mid` (seeks from where it
    // was, after `back` played): at most (ahead - back) frames to go, less
    // what the pending seek already did (a chunk per block at least).
    const unsigned int done = steps * kSeekChunk;
    const unsigned int left = ahead - back > done ? ahead - back - done : 0;
    silent = mixToTarget(rig, h, ref, ahead, "seek forward while pending", maxSilentCalls(left) + 32);
    EXPECT(silent <= maxSilentCalls(left), "the forward seek took %u silent blocks (%u frames left)", silent, left);
    EXPECT_TIME(rig.worst, "worst mix %.2f ms", rig.worst);

    // Past the end: the voice ends at once, without decoding anything.
    rig.worst = 0;
    rig.soloud.seek(h, source.frames / double(source.rate) + 5.0);
    EXPECT(std::fabs(rig.soloud.getStreamPosition(h) - (source.frames / double(source.rate) + 5.0)) < 1e-6,
           "position after a seek past the end");
    rig.mix();
    rig.mix();
    EXPECT(!rig.soloud.isValidVoiceHandle(h), "the voice did not end after a seek past the end");
    std::printf("  %-48s worst mix %.3f ms\n", "seek past the end", rig.worst);
    EXPECT_TIME(rig.worst, "past the end: worst mix %.2f ms", rig.worst);
}

// Looping without a table: the loop restart is a seek inside the mix. With a
// loop point far from the start it plays silence until the decoder gets
// there, then the loop from its start; with a loop end point the loop region
// is played exactly, so the silence does not count as source frames.
void testLooping(Source &source) {
    std::printf("%s, %s: looping\n", source.name.c_str(), tableName(Mp3Table::None));
    const unsigned int loopStart = audibleTarget(source, source.frames / 3);
    // Played before the end; its first block must be audible to tell the
    // landing apart from the silence while the seek is pending (the end of
    // the file is quiet).
    unsigned int from = source.frames - (3 * kBlock + 100);
    {
        Reference probe(source);
        for (unsigned int k = 0; k < 64; ++k)
            probe.want(from - k * 4096, kBlock);
        probe.decode();
        for (unsigned int k = 0; k < 64; ++k)
            if (!probe.silentAt(from - k * 4096)) {
                from -= k * 4096;
                break;
            }
    }
    const unsigned int tail = source.frames - from;
    const unsigned int loopEnd = loopStart + 6 * kBlock + 37;
    Reference ref(source);
    ref.want(from, tail + kBlock);
    ref.want(loopStart, kCompare + 8 * kBlock);
    ref.decode();
    const double loopStartTime = seekTime(source, loopStart);

    {
        // Natural end, then the loop point.
        Rig rig(source);
        const SoLoud::handle h = rig.play();
        rig.soloud.setLooping(h, true);
        rig.soloud.setLoopPoint(h, loopStartTime);
        rig.worst = 0;
        rig.soloud.seek(h, seekTime(source, from));
        rig.mix();  // applies the seek: `from` frames on from where the table-less decoder is
        unsigned int blocks = 0;
        while (rig.silent() && blocks++ < 4 + maxSilentCalls(from))
            rig.mix();
        // The tail of the file, then silence to the end of the block.
        bool exact = true;
        unsigned int frame = 0;
        while (frame < tail) {
            for (unsigned int i = 0; i < kBlock; ++i, ++frame) {
                if (frame < tail)
                    exact = exact && rig.matches(ref, i, from + frame);
                else
                    exact = exact && rig.out[i * 2] == 0.0f && rig.out[i * 2 + 1] == 0.0f;
            }
            if (frame < tail)
                rig.mix();
        }
        EXPECT(exact, "loop: the tail before the restart is not the source");
        // Then silence until the decoder reaches the loop point, then the loop.
        unsigned int silent = 0, played = 0;
        bool loopExact = true;
        for (unsigned int b = 0; b < 8 + maxSilentCalls(loopStart) && played < kCompare; ++b) {
            rig.mix();
            if (played == 0 && rig.silent()) {
                ++silent;
                continue;
            }
            for (unsigned int i = 0; i < kBlock; ++i)
                loopExact = loopExact && rig.matches(ref, i, loopStart + played + i);
            played += kBlock;
        }
        std::printf("  %-48s worst mix %.3f ms, %u silent blocks\n", "loop restart to a far loop point", rig.worst,
                    silent);
        EXPECT(played >= kCompare && loopExact, "loop: not sample exact from the loop point");
        EXPECT(rig.soloud.getLoopCount(h) == 1, "loop count %u", rig.soloud.getLoopCount(h));
        EXPECT(silent <= maxSilentCalls(loopStart), "loop restart: %u silent blocks", silent);
        EXPECT_TIME(rig.worst, "loop restart: worst mix %.2f ms", rig.worst);
    }
    {
        // A loop end point: the region [loopStart, loopEnd) plays exactly,
        // every time, with silence while each restart decodes its way back.
        Rig rig(source);
        const SoLoud::handle h = rig.play();
        rig.soloud.setLooping(h, true);
        rig.soloud.setLoopPoint(h, loopStartTime);
        rig.soloud.setLoopEndPoint(h, loopEndTime(source, loopEnd));
        rig.soloud.seek(h, loopStartTime);
        rig.worst = 0;
        // Collect the non-silent frames of the output: each loop pass must be
        // the whole region, sample exact. Three passes, then a few blocks of
        // the silence after the third restart.
        std::vector<float> left;
        const unsigned int region = loopEnd - loopStart;
        const unsigned int maxBlocks = 3 * (8 + maxSilentCalls(loopStart) + region / kBlock);
        unsigned int tailBlocks = 0;
        for (unsigned int b = 0; b < maxBlocks && tailBlocks < 3; ++b) {
            rig.mix();
            for (unsigned int i = 0; i < kBlock; ++i)
                if (rig.out[i * 2] != 0.0f || rig.out[i * 2 + 1] != 0.0f)
                    left.push_back(rig.out[i * 2]);
            if (rig.soloud.getLoopCount(h) >= 3)
                ++tailBlocks;
        }
        // Compare each pass (the region's own silent samples, if any, are
        // dropped from both).
        std::vector<float> pass;
        for (unsigned int f = loopStart; f < loopEnd; ++f) {
            const float l = std::max(-1.0f, std::min(1.0f, ref.at(f, 0)));
            const float r = std::max(-1.0f, std::min(1.0f, ref.at(f, source.channels == 1 ? 0 : 1)));
            if (l != 0.0f || r != 0.0f)
                pass.push_back(l);
        }
        const unsigned int passes = (unsigned int)(left.size() / pass.size());
        bool exact = left.size() % pass.size() == 0 && passes >= 2;
        for (size_t i = 0; exact && i < left.size(); ++i)
            exact = left[i] == pass[i % pass.size()];
        std::printf("  %-48s worst mix %.3f ms, %u loop passes\n", "loop end point", rig.worst, passes);
        EXPECT(exact, "loop end point: the passes are not the region (%zu frames, region %zu)", left.size(),
               pass.size());
        EXPECT(rig.soloud.getLoopCount(h) >= 2, "loop end point: loop count %u", rig.soloud.getLoopCount(h));
        EXPECT_TIME(rig.worst, "loop end point: worst mix %.2f ms", rig.worst);
    }
}

// A mixer thread, seeks and play/stop from this one (for ThreadSanitizer).
void testThreaded(Source &source) {
    std::printf("%s, %s: mixer thread, seeks from another thread\n", source.name.c_str(), tableName(Mp3Table::None));
    Rig rig(source);
    SoLoud::handle h = rig.play();
    std::atomic<bool> running{true};
    std::atomic<double> worst{0};
    std::thread mixer([&] {
        std::vector<float> out(kBlock * 2);
        while (running.load()) {
            const double t0 = threadCpuMs();
            rig.soloud.mix(out.data(), kBlock);
            const double ms = msSince(t0);
            if (ms > worst.load())
                worst.store(ms);
            std::this_thread::sleep_for(std::chrono::milliseconds(1));
        }
    });
    unsigned int x = 12345;
    for (int i = 0; i < 60; ++i) {
        x = x * 1664525u + 1013904223u;
        const double t = (x >> 8) % source.frames / double(source.rate);
        rig.soloud.seek(h, t);
        if (i % 20 == 19) {
            rig.soloud.stop(h);
            h = rig.soloud.play(source.stream);
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(3));
    }
    const double last = source.frames / 4 / double(source.rate);
    rig.soloud.seek(h, last);
    const auto deadline = Clock::now() + std::chrono::seconds(30);
    while (rig.soloud.getStreamPosition(h) <= last + 0.01 && Clock::now() < deadline)
        std::this_thread::sleep_for(std::chrono::milliseconds(2));
    running.store(false);
    mixer.join();
    std::printf("  %-48s worst mix %.3f ms\n", "threaded", worst.load());
    EXPECT(rig.soloud.getStreamPosition(h) > last + 0.01, "the last seek never played: position %f",
           rig.soloud.getStreamPosition(h));
    EXPECT_TIME(worst.load(), "threaded: worst mix %.2f ms", worst.load());
}

// CPU time (ms) of the most decoding a seek does between two checks of its
// time cap: a chunk (kSeekChunk frames), or more where the decoder's own unit
// is larger (MBOggDecoder decodes Opus a whole Ogg page at a time, a second
// of audio as ffmpeg writes it). From plain playback of the first ~4 s on a
// fresh instance: the average cost of a chunk, or the worst single call if
// more.
double decodeUnitMs(Source &source) {
    SoLoud::AudioSourceInstance *inst = source.stream.createInstance();
    inst->init(source.stream, 0);
    std::vector<float> block(kBlock * MAX_CHANNELS);
    inst->getAudio(block.data(), kBlock, kBlock);  // warm up
    const unsigned int calls = std::min((unsigned int)(4 * source.rate) / kBlock, source.frames / kBlock - 1);
    double total = 0, worstCall = 0;
    for (unsigned int i = 0; i < calls; ++i) {
        const double t0 = threadCpuMs();
        inst->getAudio(block.data(), kBlock, kBlock);
        const double ms = msSince(t0);
        total += ms;
        worstCall = std::max(worstCall, ms);
    }
    delete inst;
    return std::max(total * kSeekChunk / (calls * kBlock), worstCall);
}

// The per-call time cap lowered to kSmallCapUs: the seeks take more silent
// calls, still land sample exact, and the worst getAudio() stays under the
// bound described at kSmallCapUs.
void testSmallCap(Source &source, const char *label, const std::vector<unsigned int> &targets) {
    const double unitMs = decodeUnitMs(source);
    const double bound = kSmallCapUs / 1000.0 + kDecodeSlack * 2 * unitMs + kSmallCapMarginMs;
    std::printf("%s, %s: time cap %u us (a unit of decoding takes %.3f ms)\n", source.name.c_str(), label,
                kSmallCapUs, unitMs);
    Reference ref(source);
    for (unsigned int t : targets)
        ref.want(t, kCompare + kBlock);
    ref.decode();
    SeekCap cap(kSmallCapUs);
    for (unsigned int t : targets) {
        char what[96];
        std::snprintf(what, sizeof what, "getAudio, seek to %.1f s", t / source.rate);
        const unsigned int silent = checkInstanceSeek(source, ref, 0, t, what, bound);
        EXPECT(silent <= maxSilentCalls(t), "%s: %u silent calls for %u frames", what, silent, t);
    }
}

// The frame budget alone (cap off), as on the web: a seek takes at most a
// silent call per budget.
void testFrameBudgetOnly(Source &source, unsigned int target) {
    std::printf("%s, %s: time cap off (frame budget only)\n", source.name.c_str(), tableName(Mp3Table::None));
    Reference ref(source);
    ref.want(target, kCompare + kBlock);
    ref.decode();
    SeekCap cap(0);
    char what[96];
    std::snprintf(what, sizeof what, "getAudio, seek to %.1f s", target / source.rate);
    const unsigned int silent = checkInstanceSeek(source, ref, 0, target, what);
    EXPECT(silent <= target / kMp3Budget + 1, "%u silent calls for %u frames", silent, target);
}

// Other formats whose seeks decode: FLAC, Opus, ADPCM WAV.
void testOtherFormat(const std::string &path) {
    Source source;
    const std::string name = path.substr(path.find_last_of('/') + 1);
    if (!source.load(name, readFile(path))) {
        std::printf("%s: could not load, skipped\n", path.c_str());
        return;
    }
    std::printf("%s (%.1f s): seeks\n", name.c_str(), source.frames / source.rate);
    const unsigned int targets[] = {audibleTarget(source, source.frames / 10 * 9),
                                    audibleTarget(source, source.frames / 3), audibleTarget(source, kBlock * 3)};
    Reference ref(source);
    for (unsigned int t : targets)
        ref.want(t, kCompare + kBlock);
    ref.decode();
    for (unsigned int t : targets) {
        char what[96];
        std::snprintf(what, sizeof what, "getAudio, seek to %.1f s", t / source.rate);
        checkInstanceSeek(source, ref, 0, t, what);
        std::snprintf(what, sizeof what, "getAudio, seek back to %.1f s", t / source.rate);
        checkInstanceSeek(source, ref, std::min(source.frames - kBlock, t + 20 * kBlock), t, what);
    }
    testSmallCap(source, "small cap", {targets[1]});
}

}  // namespace

int main(int argc, char **argv) {
    const std::string asset = "example/assets/audio/8_bit_mentality.mp3";
    const std::vector<unsigned char> original = readFile(asset);
    if (original.empty()) {
        std::fprintf(stderr, "cannot read %s (run from the repository root)\n", asset.c_str());
        return 1;
    }
    // The asset as is (ID3 tag, LAME header: encoder delay and padding), and
    // six copies of its audio frames as one plain 20-minute stream (two under
    // a sanitizer, which decodes several times slower).
    const int copies = SANITIZED ? 2 : 6;
    const Mp3Table tables[] = {Mp3Table::Intact, Mp3Table::None, Mp3Table::Broken};
    for (Mp3Table table : tables) {
        Source shortMp3;
        EXPECT(shortMp3.load("8_bit_mentality.mp3", original), "load %s", asset.c_str());
        shortMp3.setMp3Table(table);
        testSeeks(shortMp3, table, {audibleTarget(shortMp3, shortMp3.frames / 10 * 9),
                                    audibleTarget(shortMp3, shortMp3.frames / 2), audibleTarget(shortMp3, 2000)});

        Source longMp3;
        EXPECT(longMp3.load("long MP3", repeatMp3(original, copies)), "load the long MP3");
        if (table == Mp3Table::Intact)
            std::printf("long MP3: %.1f minutes, %u seek points\n", longMp3.frames / longMp3.rate / 60,
                        longMp3.stream.mMp3SeekPointCount);
        longMp3.setMp3Table(table);
        testSeeks(longMp3, table, {audibleTarget(longMp3, longMp3.frames / 10 * 9),
                                   audibleTarget(longMp3, longMp3.frames / 3)});
    }
    {
        Source longMp3;
        EXPECT(longMp3.load("long MP3", repeatMp3(original, copies)), "load the long MP3");
        longMp3.setMp3Table(Mp3Table::None);
        testSeekWhilePending(longMp3);
        testLooping(longMp3);
        testThreaded(longMp3);
        testFrameBudgetOnly(longMp3, audibleTarget(longMp3, longMp3.frames / 3));
    }
    for (Mp3Table table : tables) {
        Source longMp3;
        EXPECT(longMp3.load("long MP3", repeatMp3(original, copies)), "load the long MP3");
        longMp3.setMp3Table(table);
        testSmallCap(longMp3, tableName(table), {audibleTarget(longMp3, longMp3.frames / 3)});
    }

    // FLAC (no seek table) and Opus from the repo, and whatever the run
    // script made (long FLAC with a seek table, Opus, ADPCM WAV).
    testOtherFormat("example/assets/audio/sample-FLAC.flac");
#if !defined(NO_XIPH_LIBS)
    testOtherFormat("example/assets/audio/sample-OPUS.opus");
#endif
    for (int i = 1; i < argc; ++i)
        testOtherFormat(argv[i]);

    std::printf("\nworst getAudio/seek call %.3f ms, worst mix call %.3f ms%s\n", gWorstGetAudioMs, gWorstMixMs,
                SANITIZED ? " (sanitizer build: timing not asserted)" : "");
    if (gFailures == 0) {
        std::printf("All %d assertions passed.\n", gAssertions);
        return 0;
    }
    std::printf("%d assertions, %d failures\n", gAssertions, gFailures);
    return 1;
}
