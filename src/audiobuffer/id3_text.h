#pragma once

#include "metadata_bounds.h"
#include <cstdint>
#include <string>

namespace id3_text_detail {
inline void appendUtf8(std::string &out, uint32_t codepoint) {
    if (codepoint < 0x80) {
        out.push_back(static_cast<char>(codepoint));
    } else if (codepoint < 0x800) {
        out.push_back(static_cast<char>(0xc0 | (codepoint >> 6)));
        out.push_back(static_cast<char>(0x80 | (codepoint & 0x3f)));
    } else if (codepoint < 0x10000) {
        out.push_back(static_cast<char>(0xe0 | (codepoint >> 12)));
        out.push_back(static_cast<char>(0x80 | ((codepoint >> 6) & 0x3f)));
        out.push_back(static_cast<char>(0x80 | (codepoint & 0x3f)));
    } else {
        out.push_back(static_cast<char>(0xf0 | (codepoint >> 18)));
        out.push_back(static_cast<char>(0x80 | ((codepoint >> 12) & 0x3f)));
        out.push_back(static_cast<char>(0x80 | ((codepoint >> 6) & 0x3f)));
        out.push_back(static_cast<char>(0x80 | (codepoint & 0x3f)));
    }
}
}

// Metadata crosses the native boundary as UTF-8 regardless of the encoding
// byte in its ID3 frame. In particular, UTF-16 code units are not independent
// Unicode characters when they form a surrogate pair.
inline std::string id3TextToUtf8(const unsigned char *data, size_t frameSize) {
    if (frameSize <= 1) return {};
    const uint8_t encoding = data[0];
    const unsigned char *text = data + 1;
    size_t length = frameSize - 1;
    std::string out;
    if (encoding == 1 || encoding == 2) {
        bool little = encoding == 1;
        if (length >= 2 && ((text[0] == 0xff && text[1] == 0xfe) ||
                            (text[0] == 0xfe && text[1] == 0xff))) {
            little = text[0] == 0xff;
            text += 2;
            length -= 2;
        }
        const auto unitAt = [&](size_t pos) -> uint16_t {
            return little ? uint16_t(text[pos] | (uint16_t(text[pos + 1]) << 8))
                          : uint16_t((uint16_t(text[pos]) << 8) | text[pos + 1]);
        };
        size_t pos = 0;
        while (metadataHasBytes(length, pos, 2)) {
            uint32_t value = unitAt(pos);
            pos += 2;
            if (value == 0) break;
            if (value >= 0xd800 && value <= 0xdbff) {
                if (metadataHasBytes(length, pos, 2) &&
                    unitAt(pos) >= 0xdc00 && unitAt(pos) <= 0xdfff) {
                    value = 0x10000 + ((value - 0xd800) << 10) +
                            (unitAt(pos) - 0xdc00);
                    pos += 2;
                } else {
                    value = 0xfffd;
                }
            } else if (value >= 0xdc00 && value <= 0xdfff) {
                value = 0xfffd;
            }
            id3_text_detail::appendUtf8(out, value);
        }
        return out;
    }
    // Preserve the UTF-8 payload unchanged. Encoding zero is Latin-1, whose
    // non-ASCII bytes need explicit two-byte UTF-8 sequences.
    while (length > 0 && text[length - 1] == 0) --length;
    if (encoding == 0) {
        for (size_t i = 0; i < length; ++i)
            id3_text_detail::appendUtf8(out, text[i]);
    } else {
        out.assign(reinterpret_cast<const char *>(text), length);
    }
    return out;
}
