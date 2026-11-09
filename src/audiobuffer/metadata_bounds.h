#pragma once
#include <cstddef>
#include <cstdint>

// Lengths read from media can use the full unsigned range. Check the
// remainder before adding an offset, including on 32-bit targets.
inline bool metadataHasBytes(size_t size, size_t offset, uint64_t length) {
    return offset <= size && length <= static_cast<uint64_t>(size - offset);
}
