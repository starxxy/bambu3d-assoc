#include "bs3d/thumbcache.h"
#include "bs3d/winutil.h"
#include "bs3d/registry.h"

#include <algorithm>
#include <cstdio>
#include <cstring>
#include <sys/stat.h>

namespace bs3d {
namespace {

constexpr wchar_t kCacheSub[] = L"thumbcache";
constexpr const wchar_t kMagic[4] = {'B', '3', 'T', '1'};

std::wstring keyPath(const std::wstring& key) {
    return cacheDir() + L"\\" + key + L".b3t";
}

std::wstring keyOfImpl(const std::wstring& path, const FileStamp& stamp) {
    std::wstring joined = path;
    joined.push_back(L'|');
    wchar_t sizeBuf[64];
    wsprintfW(sizeBuf, L"%lld", (long long)stamp.size);
    joined += sizeBuf;
    joined.push_back(L'|');
    wsprintfW(sizeBuf, L"%llu", (unsigned long long)stamp.mtime.QuadPart);
    joined += sizeBuf;
    // Bake in the 3MF/AMF thumbnail-source mode so that switching between
    // "prefer embedded" (1) and "always self-render" (2) does not return a
    // stale thumbnail produced under the other mode. The embedded branch in
    // makeThumbnail stores its own cached entry only via the render path, so
    // mode 2 (always self-render) is keyed separately from mode 1.
    const DWORD mode = getThumbMode3mf();
    joined.push_back(L'|');
    wsprintfW(sizeBuf, L"%lu", mode);
    joined += sizeBuf;
    const std::string hex = sha256Hex(joined);
    return std::wstring(hex.begin(), hex.end());
}

// Cache header on disk:
//   4 bytes   magic "B3T1"
//   4 bytes   pixel size (uint32)
//   4 bytes   reserved
//   px*px*4   BGRA pixels (top-down, straight alpha)
}  // namespace

namespace thumbcache {

std::wstring cacheDir() {
    static std::wstring dir = [] {
        std::wstring d = programDataDir();
        return d + L"\\" + kCacheSub;
    }();
    return dir;
}

std::wstring keyOf(const std::wstring& path, const FileStamp& stamp) {
    return keyOfImpl(path, stamp);
}

bool get(const std::wstring& path, const FileStamp& stamp, int wantPx,
         std::vector<Byte>& out, int& outPx) {
    out.clear();
    outPx = 0;

    const std::wstring key = keyOfImpl(path, stamp);
    const std::wstring full = keyPath(key);

    WIN32_FILE_ATTRIBUTE_DATA fd;
    if (!GetFileAttributesExW(full.c_str(), GetFileExInfoStandard, &fd)) return false;

    int64_t fileSize = ((int64_t)fd.nFileSizeHigh << 32) | fd.nFileSizeLow;
    const int headerLen = 12;
    if (fileSize < headerLen) return false;

    std::vector<Byte> raw;
    if (!readFileBytes(full, raw, static_cast<uint64_t>(fileSize))) return false;
    if (raw.size() < static_cast<size_t>(headerLen)) return false;

    const Byte* p = raw.data();
    if (p[0] != kMagic[0] || p[1] != kMagic[1] || p[2] != kMagic[2] || p[3] != kMagic[3]) {
        return false;
    }
    uint32_t px = *(const uint32_t*)(p + 4);
    uint32_t reserved = *(const uint32_t*)(p + 8);
    (void)reserved;

    if (px == 0 || px > 4096) return false;
    const size_t need = static_cast<size_t>(px) * px * 4;
    if (raw.size() < headerLen + need) return false;

    if (wantPx <= 0) wantPx = (int)px;

    const Byte* src = p + headerLen;
    out.resize(static_cast<size_t>(wantPx) * wantPx * 4);

    if ((int)px == wantPx) {
        std::memcpy(out.data(), src, need);
    } else {
        // Nearest-neighbour downscale. We could do area-average here for nicer
        // quality, but the pipeline already supersamples in the rasterizer so
        // caching at full res and doing one cheap decimate step is enough.
        const float sx = (float)px / wantPx;
        const float sy = (float)px / wantPx;
        for (int y = 0; y < wantPx; ++y) {
            const int sy_ = (int)((y + 0.5f) * sy);
            const Byte* sRow = src + (size_t)sy_ * px * 4;
            Byte* dRow = out.data() + (size_t)y * wantPx * 4;
            for (int x = 0; x < wantPx; ++x) {
                const int sx_ = (int)((x + 0.5f) * sx);
                const Byte* s = sRow + (size_t)sx_ * 4;
                dRow[x * 4]     = s[0];
                dRow[x * 4 + 1] = s[1];
                dRow[x * 4 + 2] = s[2];
                dRow[x * 4 + 3] = s[3];
            }
        }
    }
    outPx = wantPx;

    // Mark file recently-used (best-effort, ignore errors)
    FILETIME now;
    GetSystemTimeAsFileTime(&now);
    HANDLE h = CreateFileW(full.c_str(), GENERIC_WRITE, 0, nullptr,
                           OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (h != INVALID_HANDLE_VALUE) {
        SetFileTime(h, nullptr, nullptr, &now);
        CloseHandle(h);
    }
    return true;
}

bool put(const std::wstring& path, const FileStamp& stamp, int px, const Byte* bgra,
         size_t bytes) {
    if (px <= 0 || px > 4096) return false;
    const size_t need = (size_t)px * px * 4;
    if (bytes != need) return false;
    if (!ensureDirectory(cacheDir())) return false;

    const std::wstring key = keyOfImpl(path, stamp);
    const std::wstring full = keyPath(key);

    std::vector<Byte> tmp;
    tmp.resize(12 + need);
    std::memcpy(tmp.data(), kMagic, 4);
    uint32_t px32 = (uint32_t)px;
    uint32_t zero = 0;
    std::memcpy(tmp.data() + 4, &px32, 4);
    std::memcpy(tmp.data() + 8, &zero, 4);
    std::memcpy(tmp.data() + 12, bgra, need);

    // Write to temp then rename so a torn write never leaves a half file.
    const std::wstring tmpPath = full + L".tmp";
    if (!writeFileBytes(tmpPath, tmp.data(), tmp.size())) return false;
    if (!removeFileSafe(full)) return false;
    return renameFile(tmpPath, full);
}

void enforceLimit(const Config& cfg) {
    const std::wstring dir = cacheDir();
    WIN32_FIND_DATAW fd;
    HANDLE h = FindFirstFileW((dir + L"\\*.b3t").c_str(), &fd);
    if (h == INVALID_HANDLE_VALUE) return;

    struct Item {
        std::wstring name;
        FILETIME mtime;
        int64_t size;
    };
    std::vector<Item> items;
    int64_t total = 0;

    do {
        int64_t sz = ((int64_t)fd.nFileSizeHigh << 32) | fd.nFileSizeLow;
        items.push_back({fd.cFileName, fd.ftLastWriteTime, sz});
        total += sz;
    } while (FindNextFileW(h, &fd));
    FindClose(h);

    const int64_t maxBytes = (int64_t)cfg.maxBytes;
    if (total <= maxBytes) return;

    // Sort by mtime ascending so we delete the oldest first.
    std::sort(items.begin(), items.end(), [](const Item& a, const Item& b) {
        return CompareFileTime(&a.mtime, &b.mtime) < 0;
    });

    int64_t toDelete = total - maxBytes;
    for (const Item& it : items) {
        if (toDelete <= 0) break;
        if (removeFileSafe(dir + L"\\" + it.name)) {
            toDelete -= it.size;
        }
    }
}

int64_t totalBytes() {
    const std::wstring dir = cacheDir();
    WIN32_FIND_DATAW fd;
    HANDLE h = FindFirstFileW((dir + L"\\*.b3t").c_str(), &fd);
    if (h == INVALID_HANDLE_VALUE) return 0;
    int64_t total = 0;
    do {
        total += ((int64_t)fd.nFileSizeHigh << 32) | fd.nFileSizeLow;
    } while (FindNextFileW(h, &fd));
    FindClose(h);
    return total;
}

void wipeAll() {
    const std::wstring dir = cacheDir();
    WIN32_FIND_DATAW fd;
    HANDLE h = FindFirstFileW((dir + L"\\*.*").c_str(), &fd);
    if (h == INVALID_HANDLE_VALUE) return;
    do {
        if (wcscmp(fd.cFileName, L".") == 0 || wcscmp(fd.cFileName, L"..") == 0) continue;
        removeFileSafe(dir + L"\\" + fd.cFileName);
    } while (FindNextFileW(h, &fd));
    FindClose(h);
}

}  // namespace thumbcache
}  // namespace bs3d
