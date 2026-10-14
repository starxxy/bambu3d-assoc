#pragma once

#include "bs3d.h"

namespace bs3d {
namespace zip {

struct Entry {
    std::string name;
    uint16_t method = 0;        // 0 = stored, 8 = deflate
    uint64_t compSize = 0;
    uint64_t rawSize = 0;
    uint64_t localOffset = 0;
    uint32_t crc = 0;
};

struct Reader {
    std::vector<Entry> entries;
    bool ok = false;
    std::string error;
};

// Parse the central directory of a zip held in memory. Never throws.
Reader open(const Byte* data, size_t size);
Reader open(const std::vector<Byte>& data);

// Decompress one entry into `out`. `limit` caps the decompressed size (DoS guard).
bool readEntry(const Reader& r, const Byte* data, size_t dataSize, const Entry& e,
               std::vector<Byte>& out, uint64_t limit);

// First entry whose name contains `needle` (case insensitive, backslashes normalised).
const Entry* findByName(const Reader& r, const char* needle);
// First entry whose name ends with `suffix` (case insensitive), e.g. ".png".
const Entry* findBySuffix(const Reader& r, const char* suffix);

}  // namespace zip
}  // namespace bs3d
