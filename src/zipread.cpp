#include "bs3d/zipread.h"

#include <cstdint>
#include <cstring>
#include <string.h>
#include <windows.h>

namespace bs3d {
namespace zip {
namespace {

// --- self-contained raw DEFLATE decoder (RFC 1951) for method 8 entries ---
// Zip stores raw deflate streams (no zlib header), which ntdll cannot decode,
// so we implement a compact canonical-Huffman inflate here.

struct BitReader {
    const Byte* data;
    size_t size;
    size_t bytePos = 0;
    int bitPos = 0;
    bool ok = true;
    uint32_t readBits(int count) {
        uint32_t v = 0;
        for (int i = 0; i < count; ++i) {
            if (bytePos >= size) { ok = false; return 0; }
            v |= static_cast<uint32_t>((data[bytePos] >> bitPos) & 1u) << i;
            if (++bitPos == 8) { bitPos = 0; ++bytePos; }
        }
        return v;
    }
    int nextBit() { return static_cast<int>(readBits(1)); }
};

struct Huff {
    uint16_t count[16];
    std::vector<uint16_t> symbol;
};

bool buildHuff(Huff& h, const uint8_t* lengths, int n) {
    for (int i = 0; i <= 15; ++i) h.count[i] = 0;
    for (int i = 0; i < n; ++i) {
        if (lengths[i] > 15) return false;
        h.count[lengths[i]]++;
    }
    int left = 1;
    for (int len = 1; len <= 15; ++len) {
        left <<= 1;
        left -= h.count[len];
        if (left < 0) return false;  // over-subscribed
    }
    uint16_t offs[16];
    offs[1] = 0;
    for (int len = 2; len <= 15; ++len)
        offs[len] = static_cast<uint16_t>(offs[len - 1] + h.count[len - 1]);
    h.symbol.resize(static_cast<size_t>(n - h.count[0]));
    for (int i = 0; i < n; ++i) {
        if (lengths[i]) h.symbol[offs[lengths[i]]++] = static_cast<uint16_t>(i);
    }
    return true;
}

int decodeSymbol(BitReader& br, const Huff& h) {
    int code = 0, first = 0, index = 0;
    for (int len = 1; len <= 15; ++len) {
        code |= br.nextBit();
        const int count = h.count[len];
        if (code < first + count) return h.symbol[index + code - first];
        index += count;
        first += count;
        first <<= 1;
        code <<= 1;
        if (!br.ok) return -1;
    }
    return -1;
}

bool inflateRaw(const Byte* src, size_t srcLen, uint8_t* dst, size_t dstCap,
                size_t& produced) {
    static const uint16_t kLenBase[29] = {3, 4, 5, 6, 7, 8, 9, 10, 11, 13, 15,
                                          17, 19, 23, 27, 31, 35, 43, 51, 59,
                                          67, 83, 99, 115, 131, 163, 195, 227, 258};
    static const uint8_t kLenExtra[29] = {0, 0, 0, 0, 0, 0, 0, 0, 1, 1, 1, 1, 2,
                                          2, 2, 2, 3, 3, 3, 3, 4, 4, 4, 4, 5, 5,
                                          5, 5, 0};
    static const uint16_t kDistBase[30] = {1,    2,    3,    4,    5,    7,
                                           9,    13,   17,   25,   33,   49,
                                           65,   97,   129,  193,  257,  385,
                                           513,  769,  1025, 1537, 2049, 3073,
                                           4097, 6145, 8193, 12289, 16385, 24577};
    static const uint8_t kDistExtra[30] = {0, 0, 0,  0,  1,  1,  2,  2,  3,  3,
                                           4, 4, 5,  5,  6,  6,  7,  7,  8,  8,
                                           9, 9, 10, 10, 11, 11, 12, 12, 13, 13};
    static const uint8_t kClOrder[19] = {16, 17, 18, 0, 8, 7, 9, 6, 10,
                                         5, 11, 4, 12, 3, 13, 2, 14, 1, 15};

    BitReader br;
    br.data = src;
    br.size = srcLen;
    size_t outPos = 0;
    int last = 0;
    while (!last) {
        last = br.readBits(1);
        const uint32_t type = br.readBits(2);
        if (!br.ok || type == 3) return false;
        if (type == 0) {  // stored
            if (br.bitPos) { br.bitPos = 0; ++br.bytePos; }
            const uint32_t len = br.readBits(16);
            const uint32_t nlen = br.readBits(16);
            if (!br.ok || len != (nlen ^ 0xFFFFu)) return false;
            if (br.bytePos + len > br.size || outPos + len > dstCap) return false;
            std::memcpy(dst + outPos, src + br.bytePos, len);
            br.bytePos += len;
            outPos += len;
            continue;
        }
        uint8_t litLens[288] = {0};
        uint8_t distLens[32] = {0};
        int hlit = 288, hdist = 32;
        if (type == 1) {  // fixed Huffman
            for (int i = 0; i < 144; ++i) litLens[i] = 8;
            for (int i = 144; i < 256; ++i) litLens[i] = 9;
            for (int i = 256; i < 280; ++i) litLens[i] = 7;
            for (int i = 280; i < 288; ++i) litLens[i] = 8;
            for (int i = 0; i < 32; ++i) distLens[i] = 5;
        } else {  // dynamic Huffman
            hlit = static_cast<int>(br.readBits(5)) + 257;
            hdist = static_cast<int>(br.readBits(5)) + 1;
            const int hclen = static_cast<int>(br.readBits(4)) + 4;
            if (!br.ok || hlit > 288 || hdist > 32) return false;
            uint8_t clLens[19] = {0};
            for (int i = 0; i < hclen; ++i)
                clLens[kClOrder[i]] = static_cast<uint8_t>(br.readBits(3));
            if (!br.ok) return false;
            Huff cl;
            if (!buildHuff(cl, clLens, 19)) return false;
            int i = 0;
            const int total = hlit + hdist;
            while (i < total) {
                const int sym = decodeSymbol(br, cl);
                if (sym < 0 || !br.ok) return false;
                if (sym < 16) {
                    (i < hlit ? litLens[i] : distLens[i - hlit]) =
                        static_cast<uint8_t>(sym);
                    ++i;
                } else if (sym == 16) {
                    if (i == 0) return false;
                    const uint8_t prev = (i - 1 < hlit) ? litLens[i - 1]
                                                       : distLens[i - 1 - hlit];
                    const int rep = 3 + static_cast<int>(br.readBits(2));
                    for (int k = 0; k < rep && i < total; ++k, ++i)
                        (i < hlit ? litLens[i] : distLens[i - hlit]) = prev;
                } else if (sym == 17) {
                    const int rep = 3 + static_cast<int>(br.readBits(3));
                    for (int k = 0; k < rep && i < total; ++k, ++i)
                        (i < hlit ? litLens[i] : distLens[i - hlit]) = 0;
                } else {
                    const int rep = 11 + static_cast<int>(br.readBits(7));
                    for (int k = 0; k < rep && i < total; ++k, ++i)
                        (i < hlit ? litLens[i] : distLens[i - hlit]) = 0;
                }
                if (!br.ok) return false;
            }
        }
        Huff lit, dist;
        if (!buildHuff(lit, litLens, 288)) return false;
        if (!buildHuff(dist, distLens, 32)) return false;
        while (true) {
            const int sym = decodeSymbol(br, lit);
            if (sym < 0 || !br.ok) return false;
            if (sym < 256) {
                if (outPos >= dstCap) return false;
                dst[outPos++] = static_cast<uint8_t>(sym);
            } else if (sym == 256) {
                break;  // end of block
            } else {
                if (sym > 285) return false;
                const int li = sym - 257;
                const uint32_t len = kLenBase[li] + br.readBits(kLenExtra[li]);
                const int dsym = decodeSymbol(br, dist);
                if (dsym < 0 || dsym > 29 || !br.ok) return false;
                const uint32_t d = kDistBase[dsym] + br.readBits(kDistExtra[dsym]);
                if (!br.ok || d == 0 || d > outPos || outPos + len > dstCap)
                    return false;
                for (uint32_t k = 0; k < len; ++k) {
                    dst[outPos] = dst[outPos - d];
                    ++outPos;
                }
            }
        }
    }
    produced = outPos;
    return true;
}

static uint16_t rd16(const Byte* p) {
    return static_cast<uint16_t>(p[0]) | (static_cast<uint16_t>(p[1]) << 8);
}

static uint32_t rd32(const Byte* p) {
    return static_cast<uint32_t>(p[0])
         | (static_cast<uint32_t>(p[1]) << 8)
         | (static_cast<uint32_t>(p[2]) << 16)
         | (static_cast<uint32_t>(p[3]) << 24);
}

// ISO-HDLC CRC-32 (reflected, poly 0xEDB88320) — the variant ZIP uses.
static uint32_t crc32(const Byte* data, size_t size) {
    static uint32_t table[256];
    static bool init = false;
    if (!init) {
        for (uint32_t i = 0; i < 256; ++i) {
            uint32_t c = i;
            for (int k = 0; k < 8; ++k)
                c = (c & 1u) ? (0xEDB88320u ^ (c >> 1)) : (c >> 1);
            table[i] = c;
        }
        init = true;
    }
    uint32_t crc = 0xFFFFFFFFu;
    for (size_t i = 0; i < size; ++i)
        crc = table[(crc ^ data[i]) & 0xFFu] ^ (crc >> 8);
    return crc ^ 0xFFFFFFFFu;
}

}  // namespace

Reader open(const Byte* data, size_t size) {
    Reader r;
    if (!data || size < 22) {
        r.error = "file too small";
        return r;
    }
    // Locate EOCD by scanning backwards (allowing for a trailing comment up to 65535 bytes).
    const size_t maxBack = 22u + 65536u;
    const size_t scanFrom = size > maxBack ? size - maxBack : 0;
    uint64_t eocdOff = UINT64_MAX;
    for (size_t i = size - 22; i >= scanFrom; --i) {
        if (data[i] == 0x50 && data[i + 1] == 0x4b && data[i + 2] == 0x05 &&
            data[i + 3] == 0x06) {
            eocdOff = i;
            break;
        }
    }
    if (eocdOff == UINT64_MAX) {
        r.error = "no EOCD";
        return r;
    }
    const Byte* eocd = data + eocdOff;
    const uint16_t cdCount = rd16(eocd + 10);
    const uint32_t cdOffset = rd32(eocd + 16);
    if (static_cast<size_t>(cdOffset) + 46 > size) {
        r.error = "central directory out of range";
        return r;
    }
    uint64_t p = cdOffset;
    for (uint16_t i = 0; i < cdCount && p + 46 <= size; ++i) {
        if (!(data[p] == 0x50 && data[p + 1] == 0x4b && data[p + 2] == 0x01 &&
              data[p + 3] == 0x02)) {
            r.error = "bad central directory record";
            return r;
        }
        Entry e;
        e.method = rd16(data + p + 10);
        e.crc = rd32(data + p + 16);
        e.compSize = rd32(data + p + 20);
        e.rawSize = rd32(data + p + 24);
        const uint16_t nameLen = rd16(data + p + 28);
        const uint16_t extraLen = rd16(data + p + 30);
        const uint16_t commentLen = rd16(data + p + 32);
        e.localOffset = rd32(data + p + 42);
        if (p + 46 + nameLen > size) {
            r.error = "entry name out of range";
            return r;
        }
        e.name.assign(reinterpret_cast<const char*>(data + p + 46), nameLen);
        r.entries.push_back(std::move(e));
        p += 46 + nameLen + extraLen + commentLen;
    }
    // Defence-in-depth against zip bombs. Legitimate 3MF parts rarely exceed
    // a few hundred entries and never more than a couple of GB of uncompressed
    // payload; anything past those thresholds is almost certainly hostile.
    constexpr size_t kMaxEntries = 10000;
    if (r.entries.size() > kMaxEntries) {
        r.error = "too many entries";
        return r;
    }
    constexpr uint64_t kMaxTotalRaw = 2ULL << 30;  // 2 GiB
    uint64_t totalRaw = 0;
    for (const auto& e : r.entries) {
        totalRaw += e.rawSize;
        if (totalRaw > kMaxTotalRaw) {
            r.error = "total decompressed size too large";
            return r;
        }
    }
    r.ok = true;
    return r;
}

Reader open(const std::vector<Byte>& data) {
    return open(data.data(), data.size());
}

bool readEntry(const Reader& r, const Byte* data, size_t dataSize, const Entry& e,
               std::vector<Byte>& out, uint64_t limit) {
    if (!r.ok || !data) return false;
    if (e.localOffset + 30 > dataSize) return false;
    const Byte* lh = data + e.localOffset;
    if (!(lh[0] == 0x50 && lh[1] == 0x4b && lh[2] == 0x03 && lh[3] == 0x04)) return false;
    const uint16_t nameLen = rd16(lh + 26);
    const uint16_t extraLen = rd16(lh + 28);
    const uint64_t dataStart = e.localOffset + 30 + nameLen + extraLen;
    if (dataStart + e.compSize > dataSize) return false;
    if (e.rawSize == 0 || e.rawSize > limit) return false;
    const Byte* src = data + dataStart;
    out.clear();
    out.resize(static_cast<size_t>(e.rawSize));
    if (e.method == 0) {
        std::memcpy(out.data(), src, static_cast<size_t>(e.rawSize));
        if (crc32(out.data(), out.size()) != e.crc) return false;
        return true;
    }
    if (e.method == 8) {
        size_t produced = 0;
        if (!inflateRaw(src, static_cast<size_t>(e.compSize), out.data(),
                        out.size(), produced)) {
            return false;
        }
        if (produced < out.size()) out.resize(produced);
        if (crc32(out.data(), out.size()) != e.crc) return false;
        return produced != 0;
    }
    return false;
}

const Entry* findByName(const Reader& r, const char* needle) {
    if (!needle) return nullptr;
    for (const auto& e : r.entries) {
        if (e.name == needle || _stricmp(e.name.c_str(), needle) == 0) return &e;
    }
    return nullptr;
}

const Entry* findBySuffix(const Reader& r, const char* suffix) {
    if (!suffix) return nullptr;
    const size_t slen = std::strlen(suffix);
    for (const auto& e : r.entries) {
        if (e.name.size() < slen) continue;
        const char* tail = e.name.c_str() + (e.name.size() - slen);
        if (_stricmp(tail, suffix) == 0) return &e;
    }
    return nullptr;
}

}  // namespace zip
}  // namespace bs3d
