// Robustness tests for the metadata parsers that read lengths straight out of
// stream data: the M4A box parser (src/audiobuffer/m4a_metadata.cpp), the
// ID3v2 frame parsers of the MP3 and AAC stream decoders, and the OpusTags
// parser. Their input is the first bytes of a network stream
// (StreamDecoder::decode, used by buffer and pull streams). Malformed or
// hostile lengths must make them stop, never read past the data or exhaust
// the stack.
//
//   ./test/run_metadata_parsers_test.sh [--asan|--wasm32] [scenario]
//
// Scenarios:
//   valid        well-formed tags still parse, for every parser below
//   m4a-nested   4 MiB of nested 'moov' boxes, parsed on a thread with a
//                1 MiB stack (the iOS main thread and Android's default)
//   m4a-wrap     a box size that wraps `offset + size` on a 32-bit size_t
//   mp3-id3-wrap an ID3v2.3 frame size that wraps `pos + size` (MP3 stream)
//   aac-id3-wrap the same frame in front of an AAC stream
//   opus-wrap    an OpusTags vendor length that wraps `pos + length`
//
// The *-wrap scenarios can only fail with a 32-bit size_t, as in the web
// build and on armeabi-v7a: run them with --wasm32. On a 64-bit host the sums
// do not wrap and they pass either way.

#include "audiobuffer/aac_stream_decoder.h"
#include "audiobuffer/m4a_metadata.h"
#include "audiobuffer/mp3_stream_decoder.h"
#include "audiobuffer/flac_stream_decoder.h"
#include <FLAC/stream_encoder.h>
#include "audiobuffer/stream_decoder.h"

#include <ogg/ogg.h>
#include <pthread.h>

#include <cstdint>
#include <cstdio>
#include <cstring>
#include <string>
#include <vector>

#if !defined(__EMSCRIPTEN__)
#include <sys/wait.h>
#include <unistd.h>
#endif

using Bytes = std::vector<unsigned char>;

static int gFailures = 0;

#define EXPECT(cond, ...)                                       \
    do {                                                        \
        if (!(cond)) {                                          \
            std::printf("  FAIL [%s:%d] ", __FILE__, __LINE__); \
            std::printf(__VA_ARGS__);                           \
            std::printf("\n");                                  \
            ++gFailures;                                        \
        }                                                       \
    } while (0)

static void putBe32(Bytes &out, uint32_t v) {
    out.push_back((v >> 24) & 0xFF);
    out.push_back((v >> 16) & 0xFF);
    out.push_back((v >> 8) & 0xFF);
    out.push_back(v & 0xFF);
}

static void putLe32(Bytes &out, uint32_t v) {
    out.push_back(v & 0xFF);
    out.push_back((v >> 8) & 0xFF);
    out.push_back((v >> 16) & 0xFF);
    out.push_back((v >> 24) & 0xFF);
}

static void putText(Bytes &out, const char *text) {
    out.insert(out.end(), text, text + std::strlen(text));
}

static Bytes cat(Bytes a, const Bytes &b) {
    a.insert(a.end(), b.begin(), b.end());
    return a;
}

// ------------------------------------------------------------------ M4A
static Bytes box(const char *type, const Bytes &payload) {
    Bytes out;
    putBe32(out, static_cast<uint32_t>(8 + payload.size()));
    out.insert(out.end(), type, type + 4);
    out.insert(out.end(), payload.begin(), payload.end());
    return out;
}

static Bytes ftyp() { return box("ftyp", {'M', '4', 'A', ' ', 0, 0, 0, 0}); }

static Bytes m4aWithTitle(const char *title) {
    Bytes data = {0, 0, 0, 1, 0, 0, 0, 0};
    putText(data, title);
    Bytes meta = {0, 0, 0, 0};
    meta = cat(meta, box("ilst", box("\xa9nam", box("data", data))));
    return cat(ftyp(), box("moov", box("udta", box("meta", meta))));
}

struct ParseJob {
    const Bytes *data;
    bool found;
};

static void *parseOnThread(void *arg) {
    auto *job = static_cast<ParseJob *>(arg);
    M4aMetadata meta;
    job->found = parseM4aMetadata(job->data->data(), job->data->size(), meta);
    return nullptr;
}

static void runM4aNested() {
    // ftyp, then a chain of 'moov' boxes, each the only child of the one
    // before it: one recursive call per 8 bytes of input.
    const size_t levels = (4u << 20) / 8;
    Bytes file = ftyp();
    const size_t base = file.size();
    file.resize(base + levels * 8);
    for (size_t i = 0; i < levels; ++i) {
        const uint32_t size = static_cast<uint32_t>((levels - i) * 8);
        unsigned char *p = file.data() + base + i * 8;
        p[0] = (size >> 24) & 0xFF;
        p[1] = (size >> 16) & 0xFF;
        p[2] = (size >> 8) & 0xFF;
        p[3] = size & 0xFF;
        std::memcpy(p + 4, "moov", 4);
    }

    ParseJob job{&file, true};
    pthread_attr_t attr;
    pthread_attr_init(&attr);
    pthread_attr_setstacksize(&attr, 1u << 20);
    pthread_t thread;
    if (pthread_create(&thread, &attr, parseOnThread, &job) != 0) {
        EXPECT(false, "pthread_create failed");
        return;
    }
    pthread_join(thread, nullptr);
    pthread_attr_destroy(&attr);
    EXPECT(!job.found, "a file of empty containers has no metadata");
}

static void runM4aWrap() {
    // ftyp (16 bytes), then a 'moov' that declares 2^32 - 16 bytes. With a
    // 32-bit size_t, 16 + (2^32 - 16) wraps to 0 and passes the bound check;
    // inside it a 1 GiB 'free' box moves the read offset far past the data.
    Bytes file = ftyp();
    putBe32(file, 0xFFFFFFF0u);
    putText(file, "moov");
    putBe32(file, 0x40000000u);
    putText(file, "free");
    file.resize(file.size() + 32, 0);

    M4aMetadata meta;
    EXPECT(!parseM4aMetadata(file.data(), file.size(), meta),
           "a box larger than the data must stop the parse");
}

// ------------------------------------------------------------------ ID3
// An ID3v2.3 tag (plain 32-bit frame sizes) holding one TIT2 frame. The tag
// header's own size covers only what is really there.
static Bytes id3Tag(uint32_t frameSize, const Bytes &frameBody) {
    Bytes frames;
    putText(frames, "TIT2");
    putBe32(frames, frameSize);
    frames.push_back(0);
    frames.push_back(0);
    frames.insert(frames.end(), frameBody.begin(), frameBody.end());
    frames.resize(frames.size() + 16, 0); // padding

    Bytes tag = {'I', 'D', '3', 3, 0, 0};
    const uint32_t size = static_cast<uint32_t>(frames.size());
    tag.push_back((size >> 21) & 0x7F);
    tag.push_back((size >> 14) & 0x7F);
    tag.push_back((size >> 7) & 0x7F);
    tag.push_back(size & 0x7F);
    return cat(tag, frames);
}

static Bytes latin1Body(const char *text) {
    Bytes body = {0}; // ISO-8859-1
    putText(body, text);
    return body;
}

// A wrapping TIT2: 0xFFFFFFF0 bytes declared, 4 present. 20 + 0xFFFFFFF0
// wraps to 16 with a 32-bit size_t.
static Bytes wrappingId3Tag() { return id3Tag(0xFFFFFFF0u, latin1Body("abc")); }

static void runMp3Id3Wrap() {
    // The tag, then zeros: no MP3 or ADTS frames, so detection settles on
    // "MP3 with ID3" once 512 bytes follow the tag. dr_mp3 reports the tag to
    // the wrapper's metadata callback before it looks for frames.
    Bytes stream = cat(wrappingId3Tag(), Bytes(1024, 0));
    StreamDecoder decoder;
    int sampleRate = 44100, channels = 2;
    decoder.decode(stream, &sampleRate, &channels, [](AudioMetadata) {}, 0);
}

static void runAacId3Wrap() {
    Bytes tag = wrappingId3Tag();
    AacMetadata meta;
    size_t id3Size = 0;
    AACDecoderWrapper::parseId3Tags(tag.data(), tag.size(), meta, id3Size);
    EXPECT(meta.title.empty(), "a frame larger than the tag must not be read");
}

// ----------------------------------------------------------------- Opus
static Bytes oggPages(const std::vector<Bytes> &packets) {
    ogg_stream_state os;
    ogg_stream_init(&os, 0x50f1);
    Bytes out;
    for (size_t i = 0; i < packets.size(); ++i) {
        ogg_packet op{};
        op.packet = const_cast<unsigned char *>(packets[i].data());
        op.bytes = static_cast<long>(packets[i].size());
        op.b_o_s = i == 0;
        op.granulepos = 0;
        op.packetno = static_cast<ogg_int64_t>(i);
        ogg_stream_packetin(&os, &op);
        ogg_page og;
        while (ogg_stream_flush(&os, &og)) {
            out.insert(out.end(), og.header, og.header + og.header_len);
            out.insert(out.end(), og.body, og.body + og.body_len);
        }
    }
    ogg_stream_clear(&os);
    return out;
}

static Bytes opusHead() {
    Bytes head;
    putText(head, "OpusHead");
    head.push_back(1);              // version
    head.push_back(2);              // channels
    head.push_back(0x38);           // pre-skip 312, LE
    head.push_back(0x01);
    putLe32(head, 48000);           // input sample rate
    head.push_back(0);              // output gain
    head.push_back(0);
    head.push_back(0);              // channel mapping family
    return head;
}

static Bytes opusTags(uint32_t vendorLength, const char *vendor,
                      const char *comment) {
    Bytes tags;
    putText(tags, "OpusTags");
    putLe32(tags, vendorLength);
    putText(tags, vendor);
    putLe32(tags, comment ? 1 : 0);
    if (comment) {
        putLe32(tags, static_cast<uint32_t>(std::strlen(comment)));
        putText(tags, comment);
    }
    return tags;
}

static void decodeOpus(const Bytes &tags, std::vector<AudioMetadata> &reported) {
    Bytes stream = oggPages({opusHead(), tags});
    StreamDecoder decoder;
    int sampleRate = 48000, channels = 2;
    decoder.decode(stream, &sampleRate, &channels,
                   [&](AudioMetadata meta) { reported.push_back(meta); }, 0);
}

static void runOpusWrap() {
    // 12 + 0xFFFFFFF8 wraps to 4 with a 32-bit size_t.
    std::vector<AudioMetadata> reported;
    decodeOpus(opusTags(0xFFFFFFF8u, "x", nullptr), reported);
    for (const auto &meta : reported)
        EXPECT(meta.oggMetadata.vendor.size() <= 1,
               "a vendor string longer than its packet must not be read");
}

// ---------------------------------------------------------------- valid
static void testValid() {
    std::printf("valid: well-formed tags still parse\n");
    {
        Bytes file = m4aWithTitle("Box Title");
        M4aMetadata meta;
        EXPECT(parseM4aMetadata(file.data(), file.size(), meta), "M4A: no metadata");
        EXPECT(meta.title == "Box Title", "M4A title '%s'", meta.title.c_str());
    }
    {
        Bytes body = latin1Body("Frame Title");
        Bytes tag = id3Tag(static_cast<uint32_t>(body.size()), body);
        AacMetadata meta;
        size_t id3Size = 0;
        EXPECT(AACDecoderWrapper::parseId3Tags(tag.data(), tag.size(), meta, id3Size),
               "AAC ID3: not parsed");
        EXPECT(meta.title == "Frame Title", "AAC ID3 title '%s'", meta.title.c_str());
        EXPECT(id3Size == tag.size(), "AAC ID3 size %zu, expected %zu", id3Size, tag.size());
    }
#if !defined(NO_XIPH_LIBS)
    {
        std::vector<AudioMetadata> reported;
        decodeOpus(opusTags(6, "vendor", "TITLE=Opus Title"), reported);
        EXPECT(reported.size() == 1, "Opus: %zu metadata reports", reported.size());
        if (!reported.empty()) {
            const auto &ogg = reported[0].oggMetadata;
            EXPECT(ogg.vendor == "vendor", "Opus vendor '%s'", ogg.vendor.c_str());
            auto it = ogg.comments.find("TITLE");
            EXPECT(it != ogg.comments.end() && it->second == "Opus Title",
                   "Opus TITLE comment missing or wrong");
        }
    }
#endif
}

// Runs a scenario that can crash the process in a child, so a stack overflow
// or wild read is reported as a failure instead of ending the run. Under
// Emscripten (no fork) it runs in-process: a trap or abort ends the run.
static void isolated(const char *name, const char *what, void (*fn)()) {
    std::printf("%s: %s\n", name, what);
#if defined(__EMSCRIPTEN__)
    fn();
#else
    std::fflush(stdout);
    const pid_t pid = fork();
    if (pid == 0) {
        gFailures = 0;
        fn();
        std::fflush(stdout);
        _exit(gFailures == 0 ? 0 : 1);
    }
    int status = 0;
    waitpid(pid, &status, 0);
    if (WIFSIGNALED(status)) {
        EXPECT(false, "%s: the parser crashed (signal %d)", name, WTERMSIG(status));
    } else if (!WIFEXITED(status) || WEXITSTATUS(status) != 0) {
        EXPECT(false, "%s: exit status %d", name, WEXITSTATUS(status));
    }
#endif
}

static void checkId3Text(const Bytes &body, const std::string &expected) {
    const Bytes tag = id3Tag(static_cast<uint32_t>(body.size()), body);
    AacMetadata aac;
    size_t tagSize = 0;
    EXPECT(AACDecoderWrapper::parseId3Tags(tag.data(), tag.size(), aac, tagSize),
           "AAC text tag parsed");
    EXPECT(aac.title == expected, "AAC ID3 text differs from UTF-8 expectation");

    MP3DecoderWrapper mp3;
    mp3.detectedType = DetectedType::BUFFER_MP3_WITH_ID3;
    std::string title;
    unsigned reports = 0;
    mp3.setTrackChangeCallback([&](AudioMetadata metadata) {
        title = metadata.mp3Metadata.title;
        ++reports;
    });
    EXPECT(mp3.initializeDecoder(44100, 1), "MP3 text decoder init");
    // Four complete mono MPEG-1 layer-III silence frames let the real decoder
    // initialize and report its parsed ID3 metadata without an external file.
    Bytes frames(417 * 4, 0);
    for (size_t i = 0; i < frames.size(); i += 417) {
        frames[i] = 0xff; frames[i + 1] = 0xfb;
        frames[i + 2] = 0x90; frames[i + 3] = 0xc0;
    }
    Bytes stream = cat(tag, frames);
    int rate = 44100, channels = 1;
    mp3.decode(stream, &rate, &channels);
    EXPECT(reports != 0, "MP3 text metadata reported");
    EXPECT(title == expected, "MP3 ID3 text differs from UTF-8 expectation");
}

static void runId3Latin1Utf8() {
    checkId3Text({0, 'C', 'a', 'f', 0xe9}, "Caf\xc3\xa9");
    checkId3Text({3, 'C', 'a', 'f', 0xc3, 0xa9}, "Caf\xc3\xa9");
}

static Bytes utf16Text(bool little, bool bom, uint8_t encoding,
                       std::initializer_list<uint16_t> units) {
    Bytes body = {encoding};
    if (bom) {
        body.push_back(little ? 0xff : 0xfe);
        body.push_back(little ? 0xfe : 0xff);
    }
    for (uint16_t unit : units) {
        body.push_back(little ? unit & 0xff : unit >> 8);
        body.push_back(little ? unit >> 8 : unit & 0xff);
    }
    return body;
}

static void runId3Utf16NonBmp() {
    const std::string expected = "A\xf0\x9f\x98\x80" "B";
    for (bool little : {false, true}) {
        for (bool bom : {false, true}) {
            checkId3Text(utf16Text(little, bom, little || bom ? 1 : 2,
                                   {0x41, 0xd83d, 0xde00, 0x42}), expected);
        }
    }
    checkId3Text(utf16Text(false, true, 2, {0x41, 0xd83d, 0xde00, 0x42}), expected);
    for (bool little : {false, true}) {
        const uint8_t encoding = little ? 1 : 2;
        checkId3Text(utf16Text(little, false, encoding, {0x41, 0xd83d, 0x42}),
                     "A\xef\xbf\xbd" "B");
        checkId3Text(utf16Text(little, false, encoding, {0x41, 0xde00, 0x42}),
                     "A\xef\xbf\xbd" "B");
        checkId3Text(utf16Text(little, false, encoding, {0x41, 0xd83d}),
                     "A\xef\xbf\xbd");
    }
}

static FLAC__StreamEncoderWriteStatus collectFlac(
    const FLAC__StreamEncoder *, const FLAC__byte bytes[], size_t length,
    unsigned, unsigned, void *opaque) {
    auto &out = *static_cast<Bytes *>(opaque);
    out.insert(out.end(), bytes, bytes + length);
    return FLAC__STREAM_ENCODER_WRITE_STATUS_OK;
}

static Bytes flacWithPadding() {
    Bytes encoded;
    auto *encoder = FLAC__stream_encoder_new();
    EXPECT(encoder != nullptr, "FLAC encoder allocation");
    if (!encoder) return {};
    FLAC__stream_encoder_set_channels(encoder, 1);
    FLAC__stream_encoder_set_bits_per_sample(encoder, 16);
    FLAC__stream_encoder_set_sample_rate(encoder, 44100);
    FLAC__stream_encoder_set_total_samples_estimate(encoder, 512);
    const auto status = FLAC__stream_encoder_init_stream(
        encoder, collectFlac, nullptr, nullptr, nullptr, &encoded);
    EXPECT(status == FLAC__STREAM_ENCODER_INIT_STATUS_OK, "FLAC encoder init");
    FLAC__int32 samples[512];
    for (unsigned i = 0; i < 512; ++i) samples[i] = int(i * 113 % 32768) - 16384;
    EXPECT(FLAC__stream_encoder_process_interleaved(encoder, samples, 512),
           "FLAC encoder samples");
    EXPECT(FLAC__stream_encoder_finish(encoder), "FLAC encoder finish");
    FLAC__stream_encoder_delete(encoder);
    if (encoded.size() < 42) return {};
    size_t audio = 4;
    for (;;) {
        if (audio + 4 > encoded.size()) return {};
        const bool last = (encoded[audio] & 0x80) != 0;
        const size_t length = (size_t(encoded[audio + 1]) << 16) |
                              (size_t(encoded[audio + 2]) << 8) | encoded[audio + 3];
        audio += 4 + length;
        if (audio > encoded.size()) return {};
        if (last) break;
    }
    // Keep STREAMINFO and the encoded frames, replacing optional metadata
    // with one final padding block. Its header plus sixteen bytes ends at 62.
    Bytes file(encoded.begin(), encoded.begin() + 42);
    file[4] &= 0x7f;
    file.insert(file.end(), {0x81, 0x01, 0x00, 0x00});
    file.resize(file.size() + 65536, 0);
    file.insert(file.end(), encoded.begin() + audio, encoded.end());
    return file;
}

static std::vector<float> decodeFlacParts(const Bytes &file, bool split,
                                          bool initialized) {
    FlacDecoderWrapper decoder;
    if (initialized) EXPECT(decoder.initializeDecoder(44100, 1), "FLAC preinit");
    int rate = 44100, channels = 1;
    std::vector<float> output;
    const size_t first = split ? 62 : file.size();
    Bytes chunk(file.begin(), file.begin() + first);
    auto result = decoder.decode(chunk, &rate, &channels);
    EXPECT(result.second == DecoderError::NoError, "FLAC first decode status");
    output.insert(output.end(), result.first.begin(), result.first.end());
    if (split) {
        EXPECT(output.empty(), "partial FLAC metadata must not produce samples");
        chunk.assign(file.begin() + first, file.end());
        result = decoder.decode(chunk, &rate, &channels);
        EXPECT(result.second == DecoderError::NoError, "FLAC remainder status");
        output.insert(output.end(), result.first.begin(), result.first.end());
    }
    decoder.setDataEnded();
    for (int i = 0; i < 8 && decoder.hasPendingData(); ++i) {
        chunk.clear();
        result = decoder.decode(chunk, &rate, &channels);
        output.insert(output.end(), result.first.begin(), result.first.end());
    }
    return output;
}

static void runFlacPartialFinalHeader() {
    const Bytes file = flacWithPadding();
    EXPECT(file.size() > 65582, "FLAC fixture must include padding and frames");
    if (file.size() <= 65582) return;
    const auto reference = decodeFlacParts(file, false, true);
    EXPECT(reference.size() == 512, "unsplit FLAC sample count %zu", reference.size());
    for (bool initialized : {false, true}) {
        const auto split = decodeFlacParts(file, true, initialized);
        EXPECT(split == reference,
               "split FLAC differs from unsplit (preinitialized=%d): %zu versus %zu",
               initialized, split.size(), reference.size());
    }
}

// The engine initializes the FLAC decoder before any audio arrives. A first
// delivery that ends inside the metadata must still leave the whole header
// cached, because a later seek replays it ahead of the new frame bytes.
static void runFlacSeekAfterSplitHeader() {
    const Bytes file = flacWithPadding();
    EXPECT(file.size() > 65582, "FLAC fixture must include padding and frames");
    if (file.size() <= 65582) return;
    const auto reference = decodeFlacParts(file, false, true);
    FlacDecoderWrapper decoder;
    EXPECT(decoder.initializeDecoder(44100, 1), "FLAC preinit");
    int rate = 44100, channels = 1;
    Bytes chunk(file.begin(), file.begin() + 62);
    decoder.decode(chunk, &rate, &channels);
    chunk.assign(file.begin() + 62, file.end());
    decoder.decode(chunk, &rate, &channels);
    // Seek back to the first frame as a pull stream does: the new delivery
    // holds frame bytes only, starting after the 65582 metadata bytes.
    decoder.prepareForSeek(0);
    chunk.assign(file.begin() + 65582, file.end());
    auto result = decoder.decode(chunk, &rate, &channels);
    EXPECT(result.second == DecoderError::NoError, "FLAC post-seek status");
    std::vector<float> output = result.first;
    decoder.setDataEnded();
    for (int i = 0; i < 8 && decoder.hasPendingData(); ++i) {
        chunk.clear();
        result = decoder.decode(chunk, &rate, &channels);
        output.insert(output.end(), result.first.begin(), result.first.end());
    }
    EXPECT(output == reference,
           "FLAC after a seek differs from the reference: %zu versus %zu samples",
           output.size(), reference.size());
}

// A stream can end inside its metadata: a truncated file. Once the data has
// ended, the decoder must report that nothing more is coming, or a drain loop
// that waits on hasPendingData() never finishes.
static void runFlacTruncatedHeaderEnd() {
    const Bytes file = flacWithPadding();
    EXPECT(file.size() > 65582, "FLAC fixture must include padding and frames");
    if (file.size() <= 65582) return;
    for (bool initialized : {false, true}) {
        FlacDecoderWrapper decoder;
        if (initialized) EXPECT(decoder.initializeDecoder(44100, 1), "FLAC preinit");
        int rate = 44100, channels = 1;
        Bytes chunk(file.begin(), file.begin() + 62);
        auto result = decoder.decode(chunk, &rate, &channels);
        EXPECT(result.first.empty(), "a partial header produces no samples");
        decoder.setDataEnded();
        for (int i = 0; i < 8 && decoder.hasPendingData(); ++i) {
            chunk.clear();
            result = decoder.decode(chunk, &rate, &channels);
            EXPECT(result.first.empty(), "a truncated header produces no samples");
        }
        EXPECT(!decoder.hasPendingData(),
               "a stream that ended inside its metadata must finish draining "
               "(preinitialized=%d)", initialized);
    }
}

static void runM4aLargeWrap() {
    // An extended-size child at offset 8 must not wrap its 64-bit end.
    const Bytes file = {0,0,0,8, 'f','r','e','e',
                        0,0,0,1, 'm','o','o','v',
                        255,255,255,255,255,255,255,255};
    M4aMetadata meta;
    EXPECT(!parseM4aMetadata(file.data(), file.size(), meta),
           "an extended box larger than the input must be rejected");
}

int main(int argc, char **argv) {
    const std::string only = argc > 1 ? argv[1] : "";
    const auto want = [&](const char *name) { return only.empty() || only == name; };

    if (want("valid")) testValid();
    if (want("id3-latin1-utf8")) runId3Latin1Utf8();
    if (want("id3-utf16-nonbmp")) runId3Utf16NonBmp();
    if (want("flac-partial-final-header"))
        isolated("flac-partial-final-header", "a split final FLAC metadata block", runFlacPartialFinalHeader);
    if (want("flac-seek-split-header"))
        isolated("flac-seek-split-header", "a seek after a split FLAC header", runFlacSeekAfterSplitHeader);
    if (want("flac-truncated-header-end"))
        isolated("flac-truncated-header-end", "data that ends inside the FLAC metadata", runFlacTruncatedHeaderEnd);
    if (want("m4a-nested"))
        isolated("m4a-nested", "4 MiB of nested boxes on a 1 MiB stack", runM4aNested);
    if (want("m4a-large-wrap"))
        isolated("m4a-large-wrap", "an extended box whose end wraps", runM4aLargeWrap);
    if (want("m4a-wrap"))
        isolated("m4a-wrap", "a box size that wraps a 32-bit offset", runM4aWrap);
    if (want("mp3-id3-wrap"))
        isolated("mp3-id3-wrap", "an ID3 frame size that wraps (MP3 stream)", runMp3Id3Wrap);
    if (want("aac-id3-wrap"))
        isolated("aac-id3-wrap", "an ID3 frame size that wraps (AAC stream)", runAacId3Wrap);
#if !defined(NO_XIPH_LIBS)
    if (want("opus-wrap"))
        isolated("opus-wrap", "an OpusTags vendor length that wraps", runOpusWrap);
#endif

    if (gFailures == 0) {
        std::printf("All metadata parser tests passed.\n");
        return 0;
    }
    std::printf("%d metadata parser check(s) failed.\n", gFailures);
    return 1;
}
