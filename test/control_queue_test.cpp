// Standalone native tests for non-blocking voice control.
//
// The mixer holds SoLoud's audio mutex for a whole mix block, and a block
// runs every voice's getAudio(): file reads and decoding for streamed
// sources. setVolume, getStreamPosition, seek, isValidVoiceHandle and the
// other voice control calls used to take that mutex, so a slow source read
// (or an MP3 stream seeking by decoding) stalled them on the caller's thread
// for as long as it lasted. They now queue their changes for the mixer and
// read a published voice state instead (see VoiceCommand and
// PublishedVoiceState in soloud.h). These tests pin that down:
//
// - a synthetic source blocks inside getAudio() for 200 ms, in the mix, and
//   every control call on that voice, on another voice and on a voice group
//   returns in under 5 ms meanwhile, with get-after-set returning the new
//   value (plain mix and render-ahead ring);
// - tens of thousands of calls over 100 voices and a group while the mix is
//   stalled all return in under 5 ms (the queue coalesces to one change per
//   voice and type, so it never fills up), and the mixer then applies the
//   last values set;
// - changes reach the voice at the next mix block, a seek before the voice's
//   next getAudio(), in call order, also relative to calls that still lock;
// - invalid and stale handles, voice groups, faders, scheduled pause/stop,
//   voice-ended dispatch, many calls between blocks and teardown keep
//   SoLoud's semantics;
// - with no mixer running (device stopped), calls apply at once instead of
//   waiting in the queue for a block that does not come;
// - queued seeks run only in the mixer at a block start (or on the idle-mixer
//   path): play/stop/fadeVolume/getActiveVoiceCount and the other locking
//   calls right after a slow seek return at once without running it; only the
//   newest of several seeks of a voice runs; thousands of seeks while the
//   mixer runs never wait and never seek on the caller;
// - disposing a sound (stop/stopAudioSource/stopAll/destructor, then delete)
//   with its seek queued or running in the mixer never lets the mixer touch
//   the disposed source afterwards;
// - a concurrent stress run for ThreadSanitizer.
//
// Null backend, no audio hardware: the tests call mix() themselves, from a
// mixer thread where the test needs one. Build and run from the
// flutter_soloud repository root with:
//
//   ./test/run_control_queue_test.sh          (add --tsan for ThreadSanitizer)

#include "soloud.h"
#include "soloud_audiosource.h"

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <functional>
#include <thread>
#include <vector>

namespace {

constexpr unsigned int kSampleRate = 8000;
constexpr unsigned int kBuffer = 512;  // one source block per mix block
constexpr unsigned int kRenderAhead = 2048;
constexpr unsigned int kDevicePeriod = 512;
constexpr int kBlockMs = 200;          // how long the slow source stalls
constexpr double kMaxCallMs = 5.0;     // acceptance bound per control call

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

using Clock = std::chrono::steady_clock;

double msSince(Clock::time_point t0) {
    return std::chrono::duration<double, std::milli>(Clock::now() - t0).count();
}

template <typename Predicate>
bool waitFor(Predicate predicate, int limitMs) {
    const auto deadline = Clock::now() + std::chrono::milliseconds(limitMs);
    while (!predicate()) {
        if (Clock::now() >= deadline)
            return false;
        std::this_thread::sleep_for(std::chrono::microseconds(200));
    }
    return true;
}

bool near(double a, double b, double eps = 1e-6) { return std::fabs(a - b) < eps; }

// ---- Test source ----------------------------------------------------------

// Mono source at kSampleRate. Emits a constant level, or a ramp whose value
// encodes the source frame, for `frames` frames (0 = endless). When armed,
// the next getAudio() call blocks for blockMs, like a slow disk read inside
// the mix. Seeks are exact and never block. Not restorable (no checkpoint
// snapshot), so with the render-ahead ring parameter changes take the
// immediate path, not a rollback.
class TestSource;

class TestInstance final : public SoLoud::AudioSourceInstance {
public:
    explicit TestInstance(TestSource *parent) : parent_(parent) {}
    unsigned int getAudio(float *buffer, unsigned int samples, unsigned int bufferSize) override;
    bool hasEnded() override;
    SoLoud::result seek(double seconds, float *, unsigned int) override;
    SoLoud::result rewind() override {
        offset_ = 0;
        mStreamPosition = 0;
        return SoLoud::SO_NO_ERROR;
    }
    unsigned int offset() const { return offset_; }

private:
    TestSource *parent_;
    unsigned int offset_ = 0;
};

class TestSource final : public SoLoud::AudioSource {
public:
    explicit TestSource(float level = 0.5f, bool ramp = false, unsigned int frames = 0)
        : level(level), ramp(ramp), frames(frames) {
        mBaseSamplerate = static_cast<float>(kSampleRate);
        mChannels = 1;
    }
    SoLoud::AudioSourceInstance *createInstance() override { return new TestInstance(this); }

    static float rampValue(unsigned int frame) { return 0.0005f * static_cast<float>((frame % 1000) + 1); }

    const float level;
    const bool ramp;
    const unsigned int frames;
    std::atomic<bool> armed{false};
    std::atomic<bool> inside{false};
    std::atomic<unsigned int> reads{0};
    std::atomic<unsigned int> seeks{0};
    std::atomic<unsigned int> lastReadOffset{0};
};

unsigned int TestInstance::getAudio(float *buffer, unsigned int samples, unsigned int bufferSize) {
    parent_->lastReadOffset.store(offset_);
    parent_->reads.fetch_add(1);
    if (parent_->armed.exchange(false)) {
        parent_->inside.store(true);
        std::this_thread::sleep_for(std::chrono::milliseconds(kBlockMs));
        parent_->inside.store(false);
    }
    unsigned int count = samples;
    if (parent_->frames != 0)
        count = std::min(samples, parent_->frames - std::min(offset_, parent_->frames));
    for (unsigned int ch = 0; ch < mChannels; ++ch)
        for (unsigned int i = 0; i < count; ++i)
            buffer[ch * bufferSize + i] = parent_->ramp ? TestSource::rampValue(offset_ + i) : parent_->level;
    offset_ += count;
    return count;
}

bool TestInstance::hasEnded() { return parent_->frames != 0 && offset_ >= parent_->frames; }

SoLoud::result TestInstance::seek(double seconds, float *, unsigned int) {
    offset_ = static_cast<unsigned int>(std::floor(seconds * mBaseSamplerate + 1e-9));
    mStreamPosition = seconds;
    parent_->seeks.fetch_add(1);
    return SoLoud::SO_NO_ERROR;
}

// ---- Engine helpers --------------------------------------------------------

bool initEngine(SoLoud::Soloud &soloud, bool renderAhead = false) {
    if (renderAhead)
        soloud.setRenderAheadConfig(kDevicePeriod, kRenderAhead);
    // No round-off clipper and a unity post-clip scaler: output = mix.
    const SoLoud::result r =
        soloud.init(0, SoLoud::Soloud::NULLDRIVER, kSampleRate, kBuffer, 2);
    EXPECT(r == SoLoud::SO_NO_ERROR, "init failed: %u", r);
    soloud.setPostClipScaler(1.0f);
    if (renderAhead)
        EXPECT(soloud.isRenderAheadEnabled(), "render-ahead ring must be enabled");
    return r == SoLoud::SO_NO_ERROR;
}

// Voice instance behind a handle. Only for single-threaded checks: it reads
// the voice table without the audio mutex, and taking the mutex would apply
// the queued commands under test.
SoLoud::AudioSourceInstance *voiceOf(SoLoud::Soloud &soloud, SoLoud::handle h) {
    const int v = soloud.getVoiceFromHandle_internal(h);
    return v >= 0 ? soloud.mVoice[v] : nullptr;
}

std::vector<float> mixBlock(SoLoud::Soloud &soloud) {
    std::vector<float> out(kBuffer * 2);
    soloud.mix(out.data(), kBuffer);
    return out;
}

// Plays a source at unity channel gains, so the left output equals source
// sample * voice volume once the start-up ramp has settled.
SoLoud::handle playUnity(SoLoud::Soloud &soloud, TestSource &source, float volume = 1.0f) {
    const SoLoud::handle h = soloud.play(source, volume);
    EXPECT(h != 0, "play failed");
    soloud.setPanAbsolute(h, 1.0f, 1.0f);
    return h;
}

// Times one control call and checks the bound.
template <typename Call>
void timed(const char *what, Call call) {
    const auto t0 = Clock::now();
    call();
    const double ms = msSince(t0);
    EXPECT(ms < kMaxCallMs, "%s took %.3f ms while the mix was blocked", what, ms);
}

// ---- Tests -----------------------------------------------------------------

// The acceptance scenario: a source blocks inside getAudio() for 200 ms in
// the middle of a mix block; control calls on that voice, on another voice
// and on a voice group must not wait for it.
void testCallsDoNotWaitForBlockedMix(bool renderAhead) {
    std::printf("%s: control calls do not wait for a mix block stalled in getAudio()\n",
                renderAhead ? "render-ahead" : "direct");
    SoLoud::Soloud soloud;
    if (!initEngine(soloud, renderAhead)) return;
    TestSource slow(0.25f);
    TestSource other(0.25f, true);
    const SoLoud::handle a = playUnity(soloud, slow);
    const SoLoud::handle b = playUnity(soloud, other);
    const SoLoud::handle group = soloud.createVoiceGroup();
    EXPECT(soloud.addVoiceToGroup(group, a) == SoLoud::SO_NO_ERROR, "add a");
    EXPECT(soloud.addVoiceToGroup(group, b) == SoLoud::SO_NO_ERROR, "add b");

    std::atomic<bool> running{true};
    std::atomic<unsigned int> blocks{0};
    std::thread mixer([&] {
        std::vector<float> out(kBuffer * 2);
        while (running.load()) {
            if (renderAhead) {
                soloud.renderRingTopUp_internal();
                soloud.mRenderRing.read(out.data(), kDevicePeriod);
            } else {
                soloud.mix(out.data(), kBuffer);
            }
            blocks.fetch_add(1);
            std::this_thread::sleep_for(std::chrono::milliseconds(1));
        }
    });
    EXPECT(waitFor([&] { return blocks.load() >= 4; }, 2000), "mixer did not start");

    slow.armed.store(true);
    EXPECT(waitFor([&] { return slow.inside.load(); }, 2000), "the mix never reached the slow source");
    const auto blockStart = Clock::now();

    const SoLoud::handle handles[] = {a, b};
    for (SoLoud::handle h : handles) {
        const char *who = h == a ? "stalled voice" : "other voice";
        std::printf("  %s\n", who);
        timed("setVolume", [&] { soloud.setVolume(h, 0.5f); });
        timed("getVolume", [&] { EXPECT(near(soloud.getVolume(h), 0.5), "%s: getVolume after set", who); });
        timed("setPan", [&] { soloud.setPan(h, 0.25f); });
        timed("getPan", [&] { EXPECT(near(soloud.getPan(h), 0.25), "%s: getPan after set", who); });
        timed("setPause", [&] { soloud.setPause(h, true); });
        timed("getPause", [&] { EXPECT(soloud.getPause(h), "%s: getPause after pausing", who); });
        timed("setPause", [&] { soloud.setPause(h, false); });
        timed("getPause", [&] { EXPECT(!soloud.getPause(h), "%s: getPause after resuming", who); });
        timed("seek", [&] { EXPECT(soloud.seek(h, 1.5) == SoLoud::SO_NO_ERROR, "%s: seek result", who); });
        timed("getStreamPosition", [&] {
            EXPECT(near(soloud.getStreamPosition(h), 1.5), "%s: position after seek: %f", who,
                   soloud.getStreamPosition(h));
        });
        timed("isValidVoiceHandle", [&] { EXPECT(soloud.isValidVoiceHandle(h), "%s: handle valid", who); });
    }
    std::printf("  voice group\n");
    timed("isVoiceGroup", [&] { EXPECT(soloud.isVoiceGroup(group), "group exists"); });
    timed("isValidVoiceHandle(group)", [&] { EXPECT(!soloud.isValidVoiceHandle(group), "a group is no voice"); });
    timed("setVolume(group)", [&] { soloud.setVolume(group, 0.75f); });
    timed("getVolume(group)", [&] { EXPECT(near(soloud.getVolume(group), 0.75), "group volume"); });
    timed("getVolume(a)", [&] { EXPECT(near(soloud.getVolume(a), 0.75), "member a volume"); });
    timed("getVolume(b)", [&] { EXPECT(near(soloud.getVolume(b), 0.75), "member b volume"); });
    timed("seek(group)", [&] { soloud.seek(group, 0.5); });
    timed("getStreamPosition(b)", [&] { EXPECT(near(soloud.getStreamPosition(b), 0.5), "member b position"); });
    timed("isValidVoiceHandle(stale)", [&] {
        EXPECT(!soloud.isValidVoiceHandle(a + (7 << 12)), "a handle with another play index");
    });

    const double elapsed = msSince(blockStart);
    EXPECT(slow.inside.load(), "the calls outlasted the stalled block (%.1f ms) -- they waited for it", elapsed);
    std::printf("  all calls done %.2f ms into the %d ms stall\n", elapsed, kBlockMs);

    // After the stall the mixer applies everything, in order.
    EXPECT(waitFor([&] { return !slow.inside.load(); }, 2000), "stall never ended");
    const unsigned int after = blocks.load();
    EXPECT(waitFor([&] { return blocks.load() >= after + 3; }, 2000), "mixer stopped");
    running.store(false);
    mixer.join();

    for (SoLoud::handle h : handles) {
        SoLoud::AudioSourceInstance *v = voiceOf(soloud, h);
        EXPECT(v != nullptr, "voice gone");
        if (!v) continue;
        EXPECT(near(v->mSetVolume, 0.75), "applied volume %f", v->mSetVolume);
        EXPECT(near(v->mPan, 0.25), "applied pan %f", v->mPan);
        EXPECT(!(v->mFlags & SoLoud::AudioSourceInstance::PAUSED), "applied pause state");
        EXPECT(soloud.getVolume(h) == v->mSetVolume, "published volume %f", soloud.getVolume(h));
        EXPECT(soloud.getStreamPosition(h) == v->mStreamPosition, "published position");
        EXPECT(v->mStreamPosition > 0.5 && v->mStreamPosition < 1.0, "applied seek: position %f",
               v->mStreamPosition);
    }
    // Both seeks of each voice were queued before the same block start: only
    // the newer one (the group's) runs.
    EXPECT(slow.seeks.load() == 1 && other.seeks.load() == 1, "each voice seeked once (%u, %u)",
           slow.seeks.load(), other.seeks.load());
    soloud.deinit();
}

// Saturation: far more control calls than the voices have fields, over many
// voices and a voice group, with the mix stalled inside a getAudio(). The
// queue holds one change per voice and type (newest value wins), so it
// cannot fill up: every call returns at once, the getters report the newest
// values meanwhile, and once the mixer runs again every voice holds the
// last values set, each voice seeked once.
void testSaturationWhileMixBlocked(bool renderAhead) {
    std::printf("%s: tens of thousands of control calls while the mix is stalled\n",
                renderAhead ? "render-ahead" : "direct");
    SoLoud::Soloud soloud;
    if (!initEngine(soloud, renderAhead)) return;
    TestSource slow(0.25f);
    TestSource level(0.5f);
    playUnity(soloud, slow);
    constexpr int kVoices = 100;
    std::vector<SoLoud::handle> voices;
    for (int i = 0; i < kVoices; ++i)
        voices.push_back(playUnity(soloud, level));
    const SoLoud::handle group = soloud.createVoiceGroup();
    for (int i = 0; i < kVoices; i += 2)
        soloud.addVoiceToGroup(group, voices[i]);

    std::atomic<bool> running{true};
    std::atomic<unsigned int> blocks{0};
    std::thread mixer([&] {
        std::vector<float> out(kBuffer * 2);
        while (running.load()) {
            if (renderAhead) {
                soloud.renderRingTopUp_internal();
                soloud.mRenderRing.read(out.data(), kDevicePeriod);
            } else {
                soloud.mix(out.data(), kBuffer);
            }
            blocks.fetch_add(1);
            std::this_thread::sleep_for(std::chrono::milliseconds(1));
        }
    });
    EXPECT(waitFor([&] { return blocks.load() >= 4; }, 2000), "mixer did not start");
    const unsigned int seeksBefore = level.seeks.load();
    slow.armed.store(true);
    EXPECT(waitFor([&] { return slow.inside.load(); }, 2000), "the mix never reached the slow source");

    auto volumeOf = [](int r, int k) { return float((r * 7 + k) % 100) / 100.0f; };
    auto panOf = [](int r, int k) { return float((r * 3 + k) % 21) / 10.0f - 1.0f; };
    auto seekOf = [](int r, int k) { return 0.25 + 0.001 * double(r * kVoices + k); };
    const int rounds = 60;  // 60 x 100 voices x 4 calls, plus group calls; even: ends paused
    double worst = 0;
    unsigned int calls = 0;
    auto call = [&](const std::function<void()> &f) {
        const auto t0 = Clock::now();
        f();
        worst = std::max(worst, msSince(t0));
        ++calls;
    };
    const auto burstStart = Clock::now();
    for (int r = 0; r < rounds; ++r) {
        // Group calls first, overwritten by this round's per-voice calls.
        call([&] { soloud.setVolume(group, 0.123f); });
        call([&] { soloud.setPan(group, 0.5f); });
        call([&] { soloud.seek(group, 9.0); });
        call([&] { soloud.setPause(group, r % 2 == 0); });
        for (int k = 0; k < kVoices; ++k) {
            const SoLoud::handle h = voices[k];
            call([&] { soloud.setVolume(h, volumeOf(r, k)); });
            call([&] { soloud.setPan(h, panOf(r, k)); });
            call([&] { soloud.setPause(h, r % 2 == 1); });
            call([&] { soloud.seek(h, seekOf(r, k)); });
        }
    }
    const double burstMs = msSince(burstStart);
    EXPECT(slow.inside.load(), "the calls outlasted the stalled block -- they waited for it");
    EXPECT(worst < kMaxCallMs, "worst control call %.3f ms while the mix was stalled", worst);
    std::printf("  %u calls in %.1f ms, worst call %.3f ms\n", calls, burstMs, worst);

    bool newest = true;
    for (int k = 0; k < kVoices; ++k) {
        const SoLoud::handle h = voices[k];
        newest = newest && near(soloud.getVolume(h), volumeOf(rounds - 1, k)) &&
                 near(soloud.getPan(h), panOf(rounds - 1, k)) && soloud.getPause(h) &&
                 near(soloud.getStreamPosition(h), seekOf(rounds - 1, k));
    }
    EXPECT(newest, "the getters report the newest values while the mix is stalled");

    EXPECT(waitFor([&] { return !slow.inside.load(); }, 2000), "stall never ended");
    const unsigned int after = blocks.load();
    EXPECT(waitFor([&] { return blocks.load() >= after + 3; }, 2000), "mixer stopped");
    running.store(false);
    mixer.join();

    bool applied = true;
    for (int k = 0; k < kVoices && applied; ++k) {
        const SoLoud::handle h = voices[k];
        SoLoud::AudioSourceInstance *v = voiceOf(soloud, h);
        applied = v != nullptr && near(v->mSetVolume, volumeOf(rounds - 1, k)) &&
                  near(v->mPan, panOf(rounds - 1, k)) && (v->mFlags & SoLoud::AudioSourceInstance::PAUSED) &&
                  near(v->mStreamPosition, seekOf(rounds - 1, k)) &&
                  soloud.getVolume(h) == v->mSetVolume && soloud.getStreamPosition(h) == v->mStreamPosition;
        if (!applied && v)
            std::fprintf(stderr, "  voice %d: volume %f pan %f paused %d position %f\n", k, v->mSetVolume, v->mPan,
                         int(v->mFlags & SoLoud::AudioSourceInstance::PAUSED), v->mStreamPosition);
    }
    EXPECT(applied, "the mixer applied the last values set");
    EXPECT(level.seeks.load() - seeksBefore == (unsigned int)kVoices, "%u source seeks for %d voices",
           level.seeks.load() - seeksBefore, kVoices);
    soloud.deinit();
}

// Changes reach the voice at the next mix block, not before: the voice
// instance is untouched until the mixer applies them, the getters report
// them meanwhile, and a seek lands before the voice's next getAudio().
void testChangesApplyAtNextBlock() {
    std::printf("changes apply at the next mix block\n");
    SoLoud::Soloud soloud;
    if (!initEngine(soloud)) return;
    TestSource level(0.5f);
    TestSource ramp(0.0f, true);
    const SoLoud::handle h = playUnity(soloud, level);
    const SoLoud::handle r = playUnity(soloud, ramp);
    soloud.setVolume(r, 0.0f);
    mixBlock(soloud);
    mixBlock(soloud);  // start-up ramps settled

    std::vector<float> out = mixBlock(soloud);
    EXPECT(near(out[(kBuffer - 1) * 2], 0.5, 1e-5), "level before: %f", out[(kBuffer - 1) * 2]);

    soloud.setVolume(h, 0.25f);
    EXPECT(near(voiceOf(soloud, h)->mSetVolume, 1.0), "applied before the next block");
    EXPECT(near(soloud.getVolume(h), 0.25), "getVolume reports the queued value");
    out = mixBlock(soloud);
    EXPECT(near(voiceOf(soloud, h)->mSetVolume, 0.25), "not applied by the next block");
    // panAndExpand ramps across the block, reaching the new gain at its end.
    EXPECT(near(out[(kBuffer - 1) * 2], 0.125, 1e-5), "level after: %f", out[(kBuffer - 1) * 2]);
    EXPECT(near(soloud.getVolume(h), 0.25), "published volume");

    soloud.setPause(h, true);
    EXPECT(!(voiceOf(soloud, h)->mFlags & SoLoud::AudioSourceInstance::PAUSED), "paused early");
    EXPECT(soloud.getPause(h), "getPause reports the queued value");
    mixBlock(soloud);
    EXPECT(voiceOf(soloud, h)->mFlags & SoLoud::AudioSourceInstance::PAUSED, "not paused by the next block");
    soloud.setPause(h, false);

    // Seek: the next getAudio() of the voice reads from the target.
    const unsigned int seeksBefore = ramp.seeks.load();
    soloud.seek(r, 0.5);
    EXPECT(ramp.seeks.load() == seeksBefore, "seeked before the next block");
    EXPECT(near(soloud.getStreamPosition(r), 0.5), "getStreamPosition reports the target");
    const unsigned int readsBefore = ramp.reads.load();
    mixBlock(soloud);
    EXPECT(ramp.seeks.load() == seeksBefore + 1, "seek not applied by the next block");
    EXPECT(ramp.reads.load() == readsBefore + 1, "one source read per block");
    EXPECT(ramp.lastReadOffset.load() == kSampleRate / 2,
           "the read after the seek started at frame %u, expected %u", ramp.lastReadOffset.load(),
           kSampleRate / 2);
    EXPECT(near(soloud.getStreamPosition(r), 0.5 + double(kBuffer) / kSampleRate),
           "position after the block: %f", soloud.getStreamPosition(r));
    soloud.deinit();
}

// Commands apply in call order, also around calls that still take the
// audio mutex (they apply everything queued before them first).
void testCommandOrder() {
    std::printf("commands apply in call order\n");
    SoLoud::Soloud soloud;
    if (!initEngine(soloud)) return;
    TestSource ramp(0.0f, true);
    const SoLoud::handle h = playUnity(soloud, ramp);
    mixBlock(soloud);

    soloud.setVolume(h, 0.1f);
    soloud.setVolume(h, 0.2f);
    soloud.setVolume(h, 0.3f);
    EXPECT(near(soloud.getVolume(h), 0.3), "newest queued volume");
    mixBlock(soloud);
    EXPECT(near(voiceOf(soloud, h)->mSetVolume, 0.3), "last set wins: %f", voiceOf(soloud, h)->mSetVolume);

    // A queued set followed by a locking fade: the fade must start from the
    // set value and keep running. Applied the other way round, the late
    // setVolume would cancel the fader.
    soloud.setVolume(h, 0.8f);
    soloud.fadeVolume(h, 0.0f, 1.0);
    SoLoud::AudioSourceInstance *v = voiceOf(soloud, h);
    EXPECT(v->mVolumeFader.mActive > 0, "the fade was cancelled by an earlier setVolume");
    EXPECT(near(v->mVolumeFader.mFrom, 0.8), "fade starts from %f, expected 0.8", v->mVolumeFader.mFrom);
    mixBlock(soloud);
    EXPECT(v->mVolumeFader.mActive > 0 && v->mSetVolume < 0.8f && v->mSetVolume > 0.0f,
           "fading: active %d volume %f", v->mVolumeFader.mActive, v->mSetVolume);
    EXPECT(near(soloud.getVolume(h), v->mSetVolume), "getVolume follows the fader");

    // The other way round: a locking call, then queued sets.
    soloud.setVolume(h, 0.6f);
    mixBlock(soloud);
    EXPECT(v->mVolumeFader.mActive == 0 && near(v->mSetVolume, 0.6), "setVolume cancels a fade");

    soloud.setPause(h, true);
    soloud.setPause(h, false);
    soloud.setPause(h, true);
    soloud.seek(h, 0.2);
    soloud.seek(h, 0.4);
    EXPECT(soloud.getPause(h) && near(soloud.getStreamPosition(h), 0.4), "newest queued values");
    const unsigned int readsBefore = ramp.reads.load();
    mixBlock(soloud);
    EXPECT(v->mFlags & SoLoud::AudioSourceInstance::PAUSED, "pause/resume/pause ends paused");
    EXPECT(near(v->mStreamPosition, 0.4), "last seek wins: %f", v->mStreamPosition);
    EXPECT(ramp.reads.load() == readsBefore, "a paused voice is not read");
    soloud.setPause(h, false);
    mixBlock(soloud);
    EXPECT(ramp.lastReadOffset.load() == (unsigned int)(0.4 * kSampleRate), "read from frame %u",
           ramp.lastReadOffset.load());
    soloud.deinit();
}

// Invalid, stale and reused handles keep SoLoud's answers.
void testHandleSemantics() {
    std::printf("invalid, stale and reused handles\n");
    SoLoud::Soloud soloud;
    if (!initEngine(soloud)) return;
    const SoLoud::handle bogus[] = {0u, 0xfffu | (5u << 12), 0x400u | (3u << 12), 0xfffff123u};
    for (SoLoud::handle h : bogus) {
        EXPECT(!soloud.isValidVoiceHandle(h), "0x%x valid", h);
        EXPECT(soloud.getVolume(h) == 0 && soloud.getPan(h) == 0 && !soloud.getPause(h) &&
               soloud.getStreamPosition(h) == 0, "0x%x defaults", h);
        soloud.setVolume(h, 0.5f);
        soloud.setPan(h, 0.5f);
        soloud.setPause(h, true);
        EXPECT(soloud.seek(h, 1.0) == SoLoud::SO_NO_ERROR, "seek on 0x%x", h);
    }

    TestSource level(0.5f);
    const SoLoud::handle h = playUnity(soloud, level);
    EXPECT(soloud.isValidVoiceHandle(h) && near(soloud.getVolume(h), 1.0), "fresh voice");
    soloud.setVolume(h, 0.3f);
    soloud.stop(h);
    EXPECT(!soloud.isValidVoiceHandle(h), "stopped voice still valid");
    EXPECT(soloud.getVolume(h) == 0, "stopped voice volume %f", soloud.getVolume(h));

    // The slot is reused: the old voice's queued value is not the new one's.
    const SoLoud::handle h2 = playUnity(soloud, level, 0.9f);
    EXPECT((h2 & 0xfff) == (h & 0xfff) && h2 != h, "slot reused (0x%x, 0x%x)", h, h2);
    soloud.setVolume(h, 0.1f);  // stale: dropped
    EXPECT(near(soloud.getVolume(h2), 0.9), "new voice volume %f", soloud.getVolume(h2));
    mixBlock(soloud);
    EXPECT(near(voiceOf(soloud, h2)->mSetVolume, 0.9), "stale command applied to the new voice");

    // A command for a voice that ends in the very block that applies it.
    TestSource shortSource(0.5f, false, 100);
    const SoLoud::handle s = playUnity(soloud, shortSource);
    soloud.setVolume(s, 0.2f);
    EXPECT(near(soloud.getVolume(s), 0.2), "queued volume");
    static std::atomic<int> ended{0};
    ended = 0;
    soloud.setVoiceEndedCallback([](unsigned int *) { ended++; });
    mixBlock(soloud);
    EXPECT(!soloud.isValidVoiceHandle(s), "ended voice still valid");
    EXPECT(soloud.getVolume(s) == 0, "ended voice volume");
    EXPECT(ended.load() == 1, "ended callback fired %d times", ended.load());
    soloud.setVoiceEndedCallback(nullptr);
    soloud.deinit();
}

// Voice groups expand to all their voices, the getters answer for the first
// voice, and the group table stays consistent across the two locks.
void testVoiceGroups() {
    std::printf("voice groups\n");
    SoLoud::Soloud soloud;
    if (!initEngine(soloud)) return;
    TestSource level(0.5f);
    const SoLoud::handle a = playUnity(soloud, level);
    const SoLoud::handle b = playUnity(soloud, level);
    const SoLoud::handle g = soloud.createVoiceGroup();
    EXPECT(soloud.isVoiceGroup(g) && soloud.isVoiceGroupEmpty(g), "new group");
    EXPECT(soloud.addVoiceToGroup(g, a) == SoLoud::SO_NO_ERROR, "add a");
    EXPECT(soloud.addVoiceToGroup(g, b) == SoLoud::SO_NO_ERROR, "add b");
    EXPECT(soloud.addVoiceToGroup(g, 0x7777) == SoLoud::SO_NO_ERROR, "adding an invalid voice is no error");
    EXPECT(soloud.addVoiceToGroup(0x1234, a) == SoLoud::INVALID_PARAMETER, "not a group");
    EXPECT(!soloud.isVoiceGroupEmpty(g) && !soloud.isValidVoiceHandle(g), "group with voices");

    soloud.setVolume(g, 0.4f);
    soloud.setPan(g, -0.5f);
    soloud.seek(g, 0.25);
    soloud.setPause(g, true);
    EXPECT(near(soloud.getVolume(g), 0.4) && near(soloud.getVolume(a), 0.4) && near(soloud.getVolume(b), 0.4),
           "queued group volume");
    EXPECT(near(soloud.getPan(b), -0.5) && near(soloud.getStreamPosition(b), 0.25) && soloud.getPause(b),
           "queued group pan/position/pause");
    mixBlock(soloud);
    const SoLoud::handle members[] = {a, b};
    for (SoLoud::handle h : members) {
        SoLoud::AudioSourceInstance *v = voiceOf(soloud, h);
        EXPECT(near(v->mSetVolume, 0.4) && near(v->mPan, -0.5) && near(v->mStreamPosition, 0.25) &&
                   (v->mFlags & SoLoud::AudioSourceInstance::PAUSED),
               "group command applied to 0x%x", h);
    }

    // Two groups, many voices: more voices than one group allocation holds.
    const SoLoud::handle big = soloud.createVoiceGroup();
    std::vector<SoLoud::handle> many;
    for (int i = 0; i < 40; ++i) {
        many.push_back(soloud.play(level, 1.0f, 0.0f, true));
        soloud.addVoiceToGroup(big, many.back());
    }
    soloud.setVolume(big, 0.05f);
    mixBlock(soloud);
    bool all = true;
    for (SoLoud::handle h : many)
        all = all && near(voiceOf(soloud, h)->mSetVolume, 0.05);
    EXPECT(all, "volume reached every voice of a 40-voice group");

    soloud.stop(a);
    EXPECT(!soloud.isVoiceGroupEmpty(g), "b is still in the group");
    EXPECT(near(soloud.getVolume(g), 0.4), "group reads its first live voice after trimming");
    soloud.stop(b);
    EXPECT(soloud.isVoiceGroupEmpty(g), "group empty after its voices stopped");
    EXPECT(soloud.getVolume(g) == 0, "empty group volume");
    EXPECT(soloud.destroyVoiceGroup(g) == SoLoud::SO_NO_ERROR, "destroy");
    EXPECT(!soloud.isVoiceGroup(g), "destroyed group");
    EXPECT(soloud.destroyVoiceGroup(g) == SoLoud::INVALID_PARAMETER, "destroy twice");
    const SoLoud::handle reused = soloud.createVoiceGroup();
    EXPECT(reused == g, "group slot reused (0x%x vs 0x%x)", reused, g);
    soloud.deinit();
}

// Faders, scheduled operations and voice-ended dispatch still run in the
// mixer, and the getters follow them.
void testFadersAndScheduledOps() {
    std::printf("faders and scheduled operations\n");
    SoLoud::Soloud soloud;
    if (!initEngine(soloud)) return;
    static std::atomic<int> ended{0};
    static std::atomic<unsigned int> endedHandle{0};
    ended = 0;
    soloud.setVoiceEndedCallback([](unsigned int *h) {
        endedHandle = *h;
        ended++;
    });
    TestSource level(0.5f);
    const SoLoud::handle f = playUnity(soloud, level);
    const SoLoud::handle p = playUnity(soloud, level);
    const SoLoud::handle s = playUnity(soloud, level);
    const double blockTime = double(kBuffer) / kSampleRate;

    soloud.fadeVolume(f, 0.0f, 4 * blockTime);
    soloud.schedulePause(p, 2 * blockTime);
    soloud.scheduleStop(s, 2 * blockTime);
    float last = 1.0f;
    bool decreasing = true;
    for (int i = 0; i < 6; ++i) {
        mixBlock(soloud);
        const float v = soloud.getVolume(f);
        decreasing = decreasing && v <= last;
        last = v;
    }
    EXPECT(decreasing && near(last, 0.0), "getVolume follows the fade (last %f)", last);
    EXPECT(soloud.getPause(p), "scheduled pause not visible to getPause");
    EXPECT(!soloud.isValidVoiceHandle(s), "scheduled stop not visible to isValidVoiceHandle");
    EXPECT(ended.load() == 1 && endedHandle.load() == s, "ended dispatch: %d calls, handle 0x%x",
           ended.load(), endedHandle.load());

    // setPan cancels a pan fade, as before.
    soloud.fadePan(p, 1.0f, 1.0);
    soloud.setPan(p, -1.0f);
    mixBlock(soloud);
    EXPECT(voiceOf(soloud, p)->mPanFader.mActive == 0 && near(soloud.getPan(p), -1.0), "pan fade cancelled");
    soloud.setVoiceEndedCallback(nullptr);
    soloud.deinit();
}

// A fade retargeted while getAudio() is blocked must use the live value
// already advanced by that block, not the last published getter snapshot.
void testRetargetFadeDuringMix() {
    std::printf("retarget a fade during an in-progress mix\n");
    for (int kind = 0; kind < 3; ++kind) {
        SoLoud::Soloud soloud;
        if (!initEngine(soloud)) return;
        TestSource source;
        const auto h = playUnity(soloud, source);
        const double blockTime = double(kBuffer) / kSampleRate;
        const bool pan = kind == 1;
        if (pan) {
            soloud.setPan(h, 1.0f);
            soloud.fadePan(h, 0.0f, 4 * blockTime);
        } else {
            soloud.fadeVolume(h, 0.0f, 4 * blockTime);
        }
        mixBlock(soloud);
        EXPECT(near(pan ? soloud.getPan(h) : soloud.getVolume(h), 0.75),
               "retarget: first published value should be 0.75");
        source.armed.store(true);
        std::thread mixer([&] { mixBlock(soloud); });
        EXPECT(waitFor([&] { return source.inside.load(); }, 2000),
               "retarget: mixer should enter the blocked source");
        if (kind == 2)
            soloud.scheduleFadeAt(h, 10.0, 0.75f, 1.0, false);
        else if (pan)
            soloud.fadePan(h, 0.75f, 1.0);
        else
            soloud.fadeVolume(h, 0.75f, 1.0);
        mixer.join();
        // Drain any setter the erroneous equality fast path queued.
        soloud.getActiveVoiceCount();
        auto *voice = voiceOf(soloud, h);
        const auto &fader = pan ? voice->mPanFader : voice->mVolumeFader;
        EXPECT(fader.mActive > 0 && near(fader.mFrom, 0.5),
               "retarget kind %d: expected active fade from 0.5, got active=%d from=%f",
               kind, fader.mActive, fader.mFrom);
        mixBlock(soloud);
        const float value = pan ? soloud.getPan(h) : soloud.getVolume(h);
        EXPECT(value >= 0.5f - 1e-6f && value < 0.75f,
               "retarget kind %d: should interpolate rather than jump (value=%f)", kind, value);
        soloud.deinit();
    }

    // Each voice in a group supplies its own starting value.
    SoLoud::Soloud soloud;
    if (!initEngine(soloud)) return;
    TestSource source;
    const auto a = playUnity(soloud, source, 0.25f);
    const auto b = playUnity(soloud, source, 0.5f);
    const auto group = soloud.createVoiceGroup();
    soloud.addVoiceToGroup(group, a);
    soloud.addVoiceToGroup(group, b);
    soloud.fadeVolume(group, 0.75f, 1.0);
    EXPECT(near(voiceOf(soloud, a)->mVolumeFader.mFrom, 0.25) &&
               near(voiceOf(soloud, b)->mVolumeFader.mFrom, 0.5),
           "group fades must start from each voice's own volume");
    soloud.deinit();
}

// Many more calls than voices between two blocks coalesce (one queued
// change per voice and type, newest value wins) without losing or
// reordering anything, and teardown with commands still queued is safe.
void testManyCommandsAndTeardown() {
    std::printf("many commands between blocks, and teardown with queued commands\n");
    {
        SoLoud::Soloud soloud;
        if (!initEngine(soloud)) return;
        TestSource level(0.5f);
        const SoLoud::handle h = playUnity(soloud, level);
        mixBlock(soloud);  // the mixer runs: calls queue
        const int n = 5000;
        for (int i = 1; i <= n; ++i)
            soloud.setVolume(h, 0.5f * float(i) / n);
        EXPECT(near(soloud.getVolume(h), 0.5), "newest volume: %f", soloud.getVolume(h));
        EXPECT(near(voiceOf(soloud, h)->mSetVolume, 1.0), "applied before the next block");
        mixBlock(soloud);
        EXPECT(near(voiceOf(soloud, h)->mSetVolume, 0.5), "applied: %f", voiceOf(soloud, h)->mSetVolume);

        for (int i = 0; i < 100; ++i) {
            soloud.setVolume(h, 0.5f);
            soloud.seek(h, 0.1 * i);
            soloud.setPause(h, i % 2 == 0);
        }
        soloud.deinit();  // with 300 commands still queued
        EXPECT(!soloud.isValidVoiceHandle(h), "voice valid after deinit");
        EXPECT(soloud.getVolume(h) == 0, "volume after deinit");
        soloud.setVolume(h, 0.25f);  // after deinit: nothing to do

        EXPECT(initEngine(soloud), "re-init");
        const SoLoud::handle h2 = playUnity(soloud, level, 0.7f);
        EXPECT(near(soloud.getVolume(h2), 0.7), "fresh voice after re-init");
        soloud.setVolume(h2, 0.35f);
        mixBlock(soloud);
        EXPECT(near(voiceOf(soloud, h2)->mSetVolume, 0.35), "commands work after re-init");
        for (int i = 0; i < 50; ++i)
            soloud.seek(h2, 0.01 * i);
    }  // destructor with commands still queued
    EXPECT(true, "destroyed");
}

// With the render-ahead ring and the mutex free, setVolume still applies
// right away (the retroactive path), without waiting for a mix.
void testRenderAheadAppliesImmediatelyWhenIdle() {
    std::printf("render-ahead: volume/pan/pause apply immediately when no mix runs\n");
    SoLoud::Soloud soloud;
    if (!initEngine(soloud, true)) return;
    TestSource level(0.5f);
    const SoLoud::handle h = playUnity(soloud, level);
    soloud.renderRingTopUp_internal();
    soloud.setVolume(h, 0.3f);
    soloud.setPan(h, 0.5f);
    soloud.setPause(h, true);
    SoLoud::AudioSourceInstance *v = voiceOf(soloud, h);
    EXPECT(near(v->mSetVolume, 0.3) && near(v->mPan, 0.5) && (v->mFlags & SoLoud::AudioSourceInstance::PAUSED),
           "applied without a mix: volume %f pan %f", v->mSetVolume, v->mPan);
    // While the mixer runs, seeks are queued in both modes.
    const unsigned int seeks = level.seeks.load();
    soloud.seek(h, 0.75);
    EXPECT(level.seeks.load() == seeks && near(soloud.getStreamPosition(h), 0.75), "seek queued");
    // A queued seek no longer holds volume/pan/pause back: they still apply
    // right away, and leave the seek queued for the mixer (taking the mutex
    // never runs a queued seek on the caller).
    soloud.setVolume(h, 0.6f);
    soloud.setPan(h, -0.25f);
    EXPECT(level.seeks.load() == seeks, "setVolume/setPan ran the queued seek");
    EXPECT(near(v->mSetVolume, 0.6) && near(v->mPan, -0.25), "applied immediately: volume %f pan %f",
           v->mSetVolume, v->mPan);
    EXPECT(near(soloud.getStreamPosition(h), 0.75), "seek target still reported");
    soloud.lockAudioMutex_internal();
    soloud.unlockAudioMutex_internal();
    EXPECT(level.seeks.load() == seeks, "a locking call ran the queued seek");
    // The next mix block applies it.
    std::vector<float> out(kDevicePeriod * 2);
    soloud.mRenderRing.read(out.data(), kDevicePeriod);
    soloud.renderRingTopUp_internal();
    // (The voice is paused, so it stays at the target.)
    EXPECT(level.seeks.load() == seeks + 1 && near(v->mStreamPosition, 0.75),
           "the mixer applied the seek: %u seeks, position %f", level.seeks.load() - seeks, v->mStreamPosition);
    soloud.deinit();
}

// With no mixer running (device stopped, or a backend nobody drives), no
// block would apply a queued command. Calls then apply at once under the free
// mutex, after everything queued before them, in both modes; while the mutex
// is held they still queue.
void testIdleMixerAppliesAtOnce(bool renderAhead) {
    std::printf("%s: with the mixer idle, calls apply at once\n", renderAhead ? "render-ahead" : "direct");
    SoLoud::Soloud soloud;
    if (!initEngine(soloud, renderAhead)) return;
    TestSource level(0.5f);
    const SoLoud::handle h = playUnity(soloud, level);
    EXPECT(soloud.isMixerIdle_internal(), "idle before the first block");
    soloud.setVolume(h, 0.4f);
    SoLoud::AudioSourceInstance *v = voiceOf(soloud, h);
    EXPECT(near(v->mSetVolume, 0.4), "applied before the first block: %f", v->mSetVolume);

    // The mixer runs: a seek is queued.
    if (renderAhead)
        soloud.renderRingTopUp_internal();
    else
        mixBlock(soloud);
    EXPECT(!soloud.isMixerIdle_internal(), "mixer running");
    const unsigned int seeks = level.seeks.load();
    soloud.seek(h, 0.5);
    EXPECT(level.seeks.load() == seeks, "seek queued while the mixer runs");

    // The mixer stops. The next call applies the queued seek, then its own.
    const int idleMs = std::max(VOICE_COMMAND_MIXER_IDLE_MS, int(4 * 1000 * kBuffer / kSampleRate));
    std::this_thread::sleep_for(std::chrono::milliseconds(idleMs + 50));
    EXPECT(soloud.isMixerIdle_internal(), "mixer idle");
    EXPECT(soloud.seek(h, 0.25) == SoLoud::SO_NO_ERROR, "seek result");
    EXPECT(level.seeks.load() == seeks + 2 && near(v->mStreamPosition, 0.25),
           "both seeks applied in order: %u seeks, position %f", level.seeks.load() - seeks, v->mStreamPosition);
    EXPECT(near(soloud.getStreamPosition(h), 0.25), "published position %f", soloud.getStreamPosition(h));
    // (Not checked after the pause: with the render-ahead ring, a pause
    // right after a seek is placed retroactively at the playhead, which
    // rewinds the voice to before the seek. Same as before this change.)
    soloud.setPause(h, true);
    soloud.setPan(h, -0.5f);
    soloud.setVolume(h, 0.2f);
    EXPECT((v->mFlags & SoLoud::AudioSourceInstance::PAUSED) && near(v->mPan, -0.5) && near(v->mSetVolume, 0.2),
           "applied without a mix: volume %f pan %f", v->mSetVolume, v->mPan);
    EXPECT(soloud.getPause(h) && near(soloud.getPan(h), -0.5) && near(soloud.getVolume(h), 0.2),
           "published: pause %d pan %f volume %f", soloud.getPause(h), soloud.getPan(h), soloud.getVolume(h));

    // Idle, but another thread holds the mutex: the call still does not wait.
    std::atomic<bool> held{false}, release{false};
    std::thread holder([&] {
        soloud.lockAudioMutex_internal();
        held.store(true);
        while (!release.load())
            std::this_thread::sleep_for(std::chrono::milliseconds(1));
        soloud.unlockAudioMutex_internal();
    });
    while (!held.load())
        std::this_thread::sleep_for(std::chrono::milliseconds(1));
    timed("setVolume (idle mixer, mutex held)", [&] { soloud.setVolume(h, 0.7f); });
    timed("seek (idle mixer, mutex held)", [&] { soloud.seek(h, 0.1); });
    EXPECT(near(soloud.getVolume(h), 0.7) && near(soloud.getStreamPosition(h), 0.1), "queued values reported");
    release.store(true);
    holder.join();
    EXPECT(near(v->mSetVolume, 0.2), "queued, not applied, while the mutex was held");
    soloud.setPan(h, 0.0f);  // applies the queued ones first, then itself
    EXPECT(near(v->mSetVolume, 0.7) && near(v->mStreamPosition, 0.1) && near(v->mPan, 0.0),
           "applied by the next call: volume %f position %f", v->mSetVolume, v->mStreamPosition);
    soloud.deinit();
}

// ---- Slow seeks and disposal ------------------------------------------------

// A source whose seek() takes seekMs, like an MP3 stream seeking by decoding,
// and records the thread it ran on. Sources are registered by id, and an
// instance checks its source is still alive (without touching it) on every
// getAudio()/seek(): touching a voice of a disposed source is a violation.
constexpr int kMaxSlowSources = 64;
std::atomic<bool> gSlowAlive[kMaxSlowSources];
std::atomic<int> gSlowNextId{0};
std::atomic<int> gDisposedAccess{0};

class SlowSeekSource;

class SlowSeekInstance final : public SoLoud::AudioSourceInstance {
public:
    SlowSeekInstance(SlowSeekSource *parent, int id, bool cheap) : parent_(parent), id_(id), cheap_(cheap) {}
    bool hasCheapSeek() const override { return cheap_; }
    unsigned int getAudio(float *buffer, unsigned int samples, unsigned int bufferSize) override;
    bool hasEnded() override { return false; }
    SoLoud::result seek(double seconds, float *, unsigned int) override;

private:
    SlowSeekSource *parent_;
    const int id_;
    const bool cheap_;
};

class SlowSeekSource final : public SoLoud::AudioSource {
public:
    explicit SlowSeekSource(int seekMs, bool cheap = false)
        : seekMs(seekMs), cheap(cheap), id(gSlowNextId.fetch_add(1) % kMaxSlowSources) {
        mBaseSamplerate = static_cast<float>(kSampleRate);
        mChannels = 1;
        gSlowAlive[id].store(true);
    }
    ~SlowSeekSource() override {
        // Disposal as the plugin does it: stop the voices, then free.
        stop();
        gSlowAlive[id].store(false);
    }
    SoLoud::AudioSourceInstance *createInstance() override { return new SlowSeekInstance(this, id, cheap); }

    std::atomic<int> seekMs;
    const bool cheap;
    const int id;
    std::atomic<unsigned int> seeks{0};
    std::atomic<bool> seeking{false};
    std::atomic<bool> seekedOnCaller{false};
    std::thread::id callerThread = std::this_thread::get_id();
};

unsigned int SlowSeekInstance::getAudio(float *buffer, unsigned int samples, unsigned int bufferSize) {
    if (!gSlowAlive[id_].load()) {
        gDisposedAccess++;
        return 0;
    }
    for (unsigned int ch = 0; ch < mChannels; ++ch)
        for (unsigned int i = 0; i < samples; ++i)
            buffer[ch * bufferSize + i] = 0.25f;
    return samples;
}

SoLoud::result SlowSeekInstance::seek(double seconds, float *, unsigned int) {
    if (!gSlowAlive[id_].load()) {
        gDisposedAccess++;
        return SoLoud::SO_NO_ERROR;
    }
    parent_->seeking.store(true);
    if (std::this_thread::get_id() == parent_->callerThread)
        parent_->seekedOnCaller.store(true);
    std::this_thread::sleep_for(std::chrono::milliseconds(parent_->seekMs.load()));
    mStreamPosition = seconds;
    parent_->seeks.fetch_add(1);
    parent_->seeking.store(false);
    return SoLoud::SO_NO_ERROR;
}

// Mixes one block on a thread of its own, as the device callback does.
void mixBlockOnMixerThread(SoLoud::Soloud &soloud) {
    std::thread mixer([&] { mixBlock(soloud); });
    mixer.join();
}

// A seek queued while the mixer runs is applied by the mixer only. Calls that
// still take the audio mutex right after it -- play(), stop(), fadeVolume(),
// getActiveVoiceCount() (which the plugin calls under its device-lifecycle
// mutex), stopAudioSource() of another sound -- do not run it: they return
// at once, and the seek is still pending when they do.
void testLockingCallsLeaveSeeksToMixer(bool renderAhead) {
    std::printf("%s: locking calls after a queued slow seek do not run it\n",
                renderAhead ? "render-ahead" : "direct");
    SoLoud::Soloud soloud;
    if (!initEngine(soloud, renderAhead)) return;
    SlowSeekSource slow(100);
    TestSource level(0.5f);
    TestSource other(0.5f);
    const SoLoud::handle h = soloud.play(slow);
    const SoLoud::handle o = playUnity(soloud, level);
    mixBlockOnMixerThread(soloud);  // the mixer runs: seeks queue

    struct Call { const char *name; std::function<void()> run; };
    std::vector<SoLoud::handle> played;
    const Call calls[] = {
        {"play", [&] { played.push_back(soloud.play(other)); }},
        {"stop", [&] { soloud.stop(o); }},
        {"fadeVolume", [&] { soloud.fadeVolume(h, 0.5f, 1.0); }},
        {"getActiveVoiceCount", [&] { EXPECT(soloud.getActiveVoiceCount() >= 1, "voice count"); }},
        {"stopAudioSource(other sound)", [&] { soloud.stopAudioSource(other); }},
        {"setVolume", [&] { soloud.setVolume(h, 0.7f); }},
        {"setPause", [&] { soloud.setPause(h, false); }},
        {"getStreamTime", [&] { soloud.getStreamTime(h); }},
    };
    double target = 0.5;
    for (const Call &call : calls) {
        const unsigned int seeks = slow.seeks.load();
        EXPECT(!soloud.isMixerIdle_internal(), "mixer counted as idle before %s", call.name);
        EXPECT(soloud.seek(h, target) == SoLoud::SO_NO_ERROR, "seek result");
        timed(call.name, call.run);
        EXPECT(slow.seeks.load() == seeks && !slow.seekedOnCaller.load(), "%s ran the queued seek", call.name);
        EXPECT(near(soloud.getStreamPosition(h), target), "%s: position %f, expected the target %f", call.name,
               soloud.getStreamPosition(h), target);
        // The next block applies it, on the mixer thread.
        mixBlockOnMixerThread(soloud);
        EXPECT(slow.seeks.load() == seeks + 1, "the mixer did not apply the seek queued before %s", call.name);
        EXPECT(near(soloud.getStreamPosition(h), target + double(kBuffer) / kSampleRate, 1e-4),
               "position after the block: %f", soloud.getStreamPosition(h));
        target += 0.5;
    }
    EXPECT(!slow.seekedOnCaller.load(), "a seek ran on the calling thread");
    soloud.deinit();
}

// A source that opts in with hasCheapSeek() (NativeStream does) has its
// queued seeks applied by the next call that takes the audio mutex as well;
// the others stay queued for the mixer, in order around the cheap ones.
void testCheapSeeksAppliedByLockingCalls() {
    std::printf("cheap seeks (hasCheapSeek) are applied by locking calls too\n");
    SoLoud::Soloud soloud;
    if (!initEngine(soloud)) return;
    SlowSeekSource cheap(0, true);
    SlowSeekSource slow(100);
    const SoLoud::handle c = soloud.play(cheap);
    const SoLoud::handle s1 = soloud.play(slow);
    mixBlockOnMixerThread(soloud);
    soloud.seek(s1, 0.5);   // left queued
    soloud.seek(c, 0.25);   // applied in place, behind it
    soloud.seek(c, 0.75);   // supersedes the one before
    timed("getActiveVoiceCount", [&] { soloud.getActiveVoiceCount(); });
    EXPECT(cheap.seeks.load() == 1 && cheap.seekedOnCaller.load(), "cheap seeks: %u on the caller: %d",
           cheap.seeks.load(), (int)cheap.seekedOnCaller.load());
    EXPECT(near(voiceOf(soloud, c)->mStreamPosition, 0.75), "cheap target %f", voiceOf(soloud, c)->mStreamPosition);
    EXPECT(near(soloud.getStreamPosition(c), 0.75), "published cheap position %f", soloud.getStreamPosition(c));
    EXPECT(slow.seeks.load() == 0 && !slow.seekedOnCaller.load(), "a slow seek ran on the caller");
    EXPECT(near(soloud.getStreamPosition(s1), 0.5), "slow target still reported");
    mixBlockOnMixerThread(soloud);
    EXPECT(slow.seeks.load() == 1 && cheap.seeks.load() == 1, "after the block: slow %u cheap %u",
           slow.seeks.load(), cheap.seeks.load());
    EXPECT(!slow.seekedOnCaller.load(), "a slow seek ran on the caller");
    soloud.deinit();
}

// Several seeks of one voice before a block: only the newest runs, and the
// getters report it throughout.
void testSupersededSeeksSkipped() {
    std::printf("only the newest of several queued seeks runs\n");
    SoLoud::Soloud soloud;
    if (!initEngine(soloud)) return;
    SlowSeekSource slow(5);
    const SoLoud::handle a = soloud.play(slow);
    const SoLoud::handle b = soloud.play(slow);
    mixBlock(soloud);
    for (int i = 1; i <= 20; ++i)
        soloud.seek(a, 0.1 * i);  // a slider drag
    soloud.seek(b, 0.3);
    EXPECT(near(soloud.getStreamPosition(a), 2.0) && near(soloud.getStreamPosition(b), 0.3), "queued targets");
    const auto t0 = Clock::now();
    mixBlock(soloud);
    const double ms = msSince(t0);
    EXPECT(slow.seeks.load() == 2, "%u source seeks for 21 queued", slow.seeks.load());
    EXPECT(ms < 5 * 3 + 20, "the block took %.1f ms", ms);
    const double block = double(kBuffer) / kSampleRate;
    EXPECT(near(voiceOf(soloud, a)->mStreamPosition, 2.0 + block, 1e-4) &&
               near(voiceOf(soloud, b)->mStreamPosition, 0.3 + block, 1e-4),
           "positions %f %f", voiceOf(soloud, a)->mStreamPosition, voiceOf(soloud, b)->mStreamPosition);
    EXPECT(near(soloud.getStreamPosition(a), 2.0 + block, 1e-4), "published position %f",
           soloud.getStreamPosition(a));
    soloud.deinit();
}

// Many seeks while a mixer runs: a voice has at most one seek queued (the
// newest target), so no seek call ever waits for room in the queue, none
// runs on the caller, and the last target is where each voice ends up.
void testManySeeksWhileMixing() {
    std::printf("many seeks while the mixer runs never wait\n");
    SoLoud::Soloud soloud;
    if (!initEngine(soloud)) return;
    SlowSeekSource slow(0);
    std::vector<SoLoud::handle> handles;
    for (int i = 0; i < 8; ++i)
        handles.push_back(soloud.play(slow));
    std::atomic<bool> running{true};
    std::thread mixer([&] {
        std::vector<float> out(kBuffer * 2);
        while (running.load()) {
            soloud.mix(out.data(), kBuffer);
            std::this_thread::sleep_for(std::chrono::milliseconds(2));
        }
    });
    while (soloud.isMixerIdle_internal())
        std::this_thread::sleep_for(std::chrono::milliseconds(1));
    const int n = 5000;
    double worst = 0;
    for (int i = 0; i < n; ++i) {
        const auto t0 = Clock::now();
        soloud.seek(handles[i % handles.size()], 0.001 * (i + 1));
        worst = std::max(worst, msSince(t0));
    }
    EXPECT(worst < kMaxCallMs, "worst seek call %.3f ms", worst);
    EXPECT(near(soloud.getStreamPosition(handles[(n - 1) % handles.size()]), 0.001 * n), "newest target");
    EXPECT(waitFor([&] { return soloud.getStreamPosition(handles.back()) > 0.001 * n; }, 2000),
           "last seek never applied");
    running.store(false);
    mixer.join();
    EXPECT(!slow.seekedOnCaller.load(), "a seek ran on the calling thread");
    EXPECT(slow.seeks.load() <= (unsigned int)n, "%u seeks", slow.seeks.load());
    std::printf("  %d seeks, worst call %.3f ms, %u source seeks\n", n, worst, slow.seeks.load());
    soloud.deinit();
}

// Disposing a sound around a seek, as Player::disposeSound (stop, or
// stopAudioSource, then delete) and disposeAllSound (stopAll, then delete
// everything) do while the device keeps mixing: once the stop returns,
// nothing may touch the voice's instance or its source again.
// - A seek still queued: the stop drops it.
// - A seek the mixer is running right now: the stop waits for that block
//   (the seek's own cost, bounded by the per-source seek budget, plus the
//   rest of the block), as it waits for any block.
void testDisposeMidSeek() {
    std::printf("dispose a sound mid-seek\n");
    SoLoud::Soloud soloud;
    if (!initEngine(soloud)) return;
    gDisposedAccess = 0;

    std::atomic<bool> running{true};
    std::atomic<bool> park{false};
    std::atomic<bool> parked{false};
    std::atomic<unsigned int> blocks{0};
    std::thread mixer([&] {
        std::vector<float> out(kBuffer * 2);
        while (running.load()) {
            parked.store(park.load());
            if (!parked.load()) {
                soloud.mix(out.data(), kBuffer);
                blocks.fetch_add(1);
            }
            std::this_thread::sleep_for(std::chrono::milliseconds(1));
        }
    });
    EXPECT(waitFor([&] { return blocks.load() >= 2; }, 2000), "mixer did not start");
    auto afterBlocks = [&](unsigned int n) {
        const unsigned int b = blocks.load();
        EXPECT(waitFor([&] { return blocks.load() >= b + n; }, 2000), "mixer stopped");
    };

    enum class How { Stop, StopAudioSource, StopAll, Destructor };
    const How hows[] = {How::Stop, How::StopAudioSource, How::StopAll, How::Destructor};
    const char *names[] = {"stop", "stopAudioSource", "stopAll", "destructor"};
    for (int k = 0; k < 4; ++k) {
        const How how = hows[k];
        // 1. Queued, not yet applied: a 2 s seek, so one that ran would show.
        {
            auto *source = new SlowSeekSource(2000);
            const SoLoud::handle h = soloud.play(*source);
            afterBlocks(1);
            // Park the mixer between blocks (for well under the idle
            // threshold), so the seek is certainly still queued when the
            // voice is stopped.
            park.store(true);
            EXPECT(waitFor([&] { return parked.load(); }, 2000), "mixer did not park");
            EXPECT(!soloud.isMixerIdle_internal(), "parked mixer counted as idle");
            soloud.seek(h, 1.0);
            EXPECT(source->seeks.load() == 0, "seek not queued");
            const auto t0 = Clock::now();
            switch (how) {
            case How::Stop: soloud.stop(h); break;
            case How::StopAudioSource: soloud.stopAudioSource(*source); break;
            case How::StopAll: soloud.stopAll(); break;
            case How::Destructor: break;
            }
            delete source;
            const double ms = msSince(t0);
            EXPECT(ms < kMaxCallMs, "%s with a queued seek took %.1f ms", names[k], ms);
            park.store(false);
            afterBlocks(3);
            EXPECT(gDisposedAccess.load() == 0, "%s: disposed source touched after a queued seek", names[k]);
        }
        // 2. In progress: the mixer is inside the source's seek.
        {
            auto *source = new SlowSeekSource(40);
            const SoLoud::handle h = soloud.play(*source);
            afterBlocks(1);
            soloud.seek(h, 1.0);
            EXPECT(waitFor([&] { return source->seeking.load(); }, 2000), "the mixer never ran the seek");
            const auto t0 = Clock::now();
            switch (how) {
            case How::Stop: soloud.stop(h); break;
            case How::StopAudioSource: soloud.stopAudioSource(*source); break;
            case How::StopAll: soloud.stopAll(); break;
            case How::Destructor: break;
            }
            if (how != How::Destructor)
                EXPECT(!source->seeking.load(), "%s returned while the seek was running", names[k]);
            delete source;  // the destructor stops (and waits) too
            const double ms = msSince(t0);
            EXPECT(ms < 40 + 100, "%s waited %.1f ms for a 40 ms seek", names[k], ms);
            EXPECT(!soloud.isValidVoiceHandle(h), "%s: voice still valid", names[k]);
            afterBlocks(3);
            EXPECT(gDisposedAccess.load() == 0, "%s: disposed source touched after an in-progress seek", names[k]);
        }
    }
    running.store(false);
    mixer.join();
    soloud.deinit();
}

// Concurrent producers, readers, play/stop and the mixer, for
// ThreadSanitizer. Values are checked for plausibility only.
void testConcurrentStress() {
    std::printf("concurrent stress (run under --tsan)\n");
    SoLoud::Soloud soloud;
    if (!initEngine(soloud)) return;
    TestSource ramp(0.0f, true);
    TestSource finite(0.5f, false, 3000);
    std::vector<SoLoud::handle> handles;
    for (int i = 0; i < 6; ++i)
        handles.push_back(playUnity(soloud, ramp, 0.5f));
    const SoLoud::handle group = soloud.createVoiceGroup();
    for (SoLoud::handle h : handles)
        soloud.addVoiceToGroup(group, h);

    std::atomic<bool> running{true};
    std::atomic<int> bad{0};
    std::thread mixer([&] {
        std::vector<float> out(kBuffer * 2);
        while (running.load()) {
            soloud.mix(out.data(), kBuffer);
            std::this_thread::sleep_for(std::chrono::microseconds(300));
        }
    });
    auto producer = [&](int seed) {
        unsigned int x = 1234567u * (seed + 1);
        while (running.load()) {
            x = x * 1664525u + 1013904223u;
            const SoLoud::handle h = (x >> 8) % 7 == 6 ? group : handles[(x >> 8) % 6];
            const float value = float((x >> 4) % 1000) / 1000.0f;
            switch ((x >> 16) % 9) {
            case 0: soloud.setVolume(h, value); break;
            case 1: if (soloud.getVolume(h) < 0 || soloud.getVolume(h) > 1) bad++; break;
            case 2: soloud.setPan(h, value - 0.5f); break;
            case 3: if (std::fabs(soloud.getPan(h)) > 0.5f) bad++; break;
            case 4: soloud.seek(h, value); break;
            case 5: if (soloud.getStreamPosition(h) < 0) bad++; break;
            case 6: soloud.setPause(h, false); break;
            case 7: soloud.getPause(h); soloud.isValidVoiceHandle(h); soloud.isVoiceGroup(group); break;
            case 8: soloud.fadeVolume(h, value, 0.01); break;  // a call that still locks
            }
        }
    };
    std::thread p1(producer, 1), p2(producer, 2);
    std::thread churn([&] {
        while (running.load()) {
            const SoLoud::handle h = soloud.play(finite, 0.5f);
            soloud.setVolume(h, 0.25f);
            soloud.seek(h, 0.01);
            if (h & 1)
                soloud.stop(h);
            std::this_thread::sleep_for(std::chrono::microseconds(500));
        }
    });
    std::this_thread::sleep_for(std::chrono::milliseconds(400));
    running.store(false);
    p1.join();
    p2.join();
    churn.join();
    mixer.join();
    EXPECT(bad.load() == 0, "%d implausible values", bad.load());

    // Quiescent: the last values set are what the voices hold.
    for (SoLoud::handle h : handles)
        soloud.setVolume(h, 0.33f);
    mixBlock(soloud);
    bool all = true;
    for (SoLoud::handle h : handles)
        all = all && near(voiceOf(soloud, h)->mSetVolume, 0.33) && near(soloud.getVolume(h), 0.33);
    EXPECT(all, "final volumes");
    soloud.deinit();
}

}  // namespace

int main() {
    testCallsDoNotWaitForBlockedMix(false);
    testCallsDoNotWaitForBlockedMix(true);
    testSaturationWhileMixBlocked(false);
    testSaturationWhileMixBlocked(true);
    testChangesApplyAtNextBlock();
    testCommandOrder();
    testHandleSemantics();
    testVoiceGroups();
    testFadersAndScheduledOps();
    testRetargetFadeDuringMix();
    testManyCommandsAndTeardown();
    testRenderAheadAppliesImmediatelyWhenIdle();
    testIdleMixerAppliesAtOnce(false);
    testIdleMixerAppliesAtOnce(true);
    testLockingCallsLeaveSeeksToMixer(false);
    testLockingCallsLeaveSeeksToMixer(true);
    testCheapSeeksAppliedByLockingCalls();
    testSupersededSeeksSkipped();
    testManySeeksWhileMixing();
    testDisposeMidSeek();
    testConcurrentStress();

    if (gFailures == 0) {
        std::printf("\nAll %d assertions passed.\n", gAssertions);
        return 0;
    }
    std::printf("\n%d assertions, %d failures\n", gAssertions, gFailures);
    return 1;
}
