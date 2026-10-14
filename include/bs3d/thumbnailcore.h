#pragma once

#include "bs3d.h"
#include "formats.h"
#include "parsers.h"
#include "rasterizer.h"
#include "thumbcache.h"

namespace bs3d {

struct ThumbOpts {
    int px = 256;
    uint32_t deadlineMs = 1200;          // whole pipeline budget
    bool useCache = true;
    bool forceRender = false;
    bool allowFallbackIcon = true;
    thumbcache::Config cacheCfg;
};

struct ThumbResult {
    bool ok = false;
    bool fromEmbeddedImage = false;
    bool fromCache = false;
    bool fallbackIcon = false;
    bool usedOcc = false;
    int px = 0;
    std::vector<Byte> bgra;   // px*px*4, top-down, straight alpha
    Rc parseRc = Rc::Ok;
    std::wstring reason;
};

// One-stop entry point used by both the Shell extension and the manager UI:
// embedded image -> cached render -> parse + render + cache.
ThumbResult makeThumbnail(const std::wstring& path, const ThumbOpts& opts = {});

// Render the thumbnail of a file synchronously and report where the pixels came
// from. Used by the manager UI to show a live preview column.
bool renderPreview(const std::wstring& path, int px, Bitmap& out);

}  // namespace bs3d
