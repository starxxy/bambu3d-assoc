#pragma once

#include "bs3d.h"
#include "winutil.h"

namespace bs3d {

// On-disk thumbnail cache.
//
// Key  : sha256(path | size | mtime)
// Value: "B3T1" magic, uint32 px, uint32 reserved, then px*px*4 bytes of BGRA.
// Files live in %ProgramData%\Bambu3DAssoc\thumbcache and are evicted oldest
// first when the total size crosses the configured ceiling.
namespace thumbcache {

struct Config {
    uint64_t maxBytes = 512ULL << 20;   // 512 MB
};

std::wstring cacheDir();
std::wstring keyOf(const std::wstring& path, const FileStamp& stamp);

bool get(const std::wstring& path, const FileStamp& stamp, int wantPx,
         std::vector<Byte>& out, int& outPx);
bool put(const std::wstring& path, const FileStamp& stamp, int px, const Byte* bgra,
         size_t bytes);

void enforceLimit(const Config& cfg);
int64_t totalBytes();
void wipeAll();

}  // namespace thumbcache
}  // namespace bs3d
