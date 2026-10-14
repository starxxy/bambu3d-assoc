#include "bs3d/thumbnailcore.h"
#include "bs3d/cadmesh.h"
#include "bs3d/thumbcache.h"
#include "bs3d/winutil.h"
#include "bs3d/registry.h"

#include <cstring>

namespace bs3d {
namespace {

bool isSupported(const FormatDef* f) {
    return f && f->thumbnailable;
}

Rc tryRender(Mesh& mesh, const std::wstring& path, Bitmap& out, int px,
             uint64_t deadlineMs, bool* usedOcc, const FormatDef* fmt) {
    if (fmt->kind == Kind::Cad) {
        bool occ = false;
        CadOpts co;
        ParseOpts po;
        po.deadlineMs = deadlineMs;
        po.maxTriangles = 400000;
        Rc r = meshCad(path, mesh, po, co, &occ);
        if (usedOcc) *usedOcc = occ;
        if (r != Rc::Ok) return r;
    } else {
        Kind kind = Kind::Unknown;
        ParseOpts po;
        po.deadlineMs = deadlineMs;
        po.maxTriangles = 400000;
        Rc r = parseMeshFor(path, mesh, kind, po);
        if (r != Rc::Ok) return r;
    }
    if (mesh.triangleCount() == 0) return Rc::EmptyFile;

    RenderStyle style;
    if (!renderMesh(mesh, px, out, style, deadlineMs)) {
        return Rc::Timeout;
    }
    return Rc::Ok;
}

}  // namespace

ThumbResult makeThumbnail(const std::wstring& path, const ThumbOpts& opts) {
    ThumbResult res;
    if (path.empty()) {
        res.reason = L"empty path";
        return res;
    }

    const FileStamp stamp = stampOf(path);
    if (!stamp.ok) {
        res.reason = L"file missing";
        return res;
    }

    const FormatDef* fmt = formatOfPath(path);
    if (!isSupported(fmt)) {
        res.reason = L"unsupported format";
        return res;
    }

    const int px = opts.px > 0 ? opts.px : 256;
    const uint64_t deadline = GetTickCount64() + (uint64_t)opts.deadlineMs;

    // v1.3.0 起软件完全免费，付费/试用闸门已移除：所有文件一律真实渲染。

    // 1) Try the cache first.
    if (opts.useCache && !opts.forceRender) {
        std::vector<Byte> cached;
        int cachedPx = 0;
        if (thumbcache::get(path, stamp, px, cached, cachedPx)) {
            res.ok = true;
            res.fromCache = true;
            res.px = cachedPx;
            res.bgra = std::move(cached);
            return res;
        }
    }

    // 2) Embedded preview image (3MF / AMF / STL with binary extension) wins.
    //    Only when the configured 3MF mode prefers embedded images (mode 1).
    //    Mode 2 always self-renders, so we skip this branch entirely.
    const DWORD thumbMode = getThumbMode3mf();
    if (fmt->mayEmbedImage && thumbMode == 1) {
        std::vector<Byte> raw;
        if (extractEmbeddedImage(path, raw, 16ULL << 20) == Rc::Ok && !raw.empty()) {
            Bitmap decoded;
            if (decodeImageBytes(raw.data(), raw.size(), px, decoded)) {
                res.ok = true;
                res.fromEmbeddedImage = true;
                res.px = decoded.w;
                res.bgra.assign(decoded.px.begin(), decoded.px.end());
                return res;
            }
        }
    }

    // 3) Mesh or CAD render.
    Bitmap bm;
    Rc r = Rc::Ok;
    bool usedOcc = false;

    if (fmt->kind == Kind::Toolpath) {
        std::vector<Seg2> segs;
        ParseOpts po;
        po.deadlineMs = deadline;
        po.maxBytes = opts.cacheCfg.maxBytes / 4;  // generous but bounded
        r = parseToolpath(path, segs, po);
        if (r == Rc::Ok && !segs.empty()) {
            RenderStyle style;
            if (renderToolpath(segs, px, bm, style)) {
                r = Rc::Ok;
            } else {
                r = Rc::ParseError;
            }
        }
    } else if (fmt->kind == Kind::EmbeddedImage) {
        // Container where we already tried above; skip rendering a mesh.
        r = Rc::Unsupported;
    } else {
        Mesh mesh;
        r = tryRender(mesh, path, bm, px, deadline, &usedOcc, fmt);
    }

    if (r != Rc::Ok) {
        res.parseRc = r;
        if (opts.allowFallbackIcon) {
            static const wchar_t kLabel[] = L"\u6682\u65E0\u9884\u89C8";  // "no preview"
            drawFallbackBadge(bm, kLabel);
            if (bm.w > 0) {
                res.ok = true;
                res.fallbackIcon = true;
                res.px = bm.w;
                res.bgra.assign(bm.px.begin(), bm.px.end());
                res.reason = r == Rc::Timeout ? L"timeout" : L"parse failed";
                return res;
            }
        }
        const char* rcName = rcToString(r);
        res.reason.assign(rcName, rcName + strlen(rcName));
        return res;
    }

    res.ok = true;
    res.px = bm.w;
    res.usedOcc = usedOcc;
    res.bgra.assign(bm.px.begin(), bm.px.end());

    // 4) Cache the freshly rendered thumbnail.
    if (opts.useCache && !opts.forceRender) {
        thumbcache::put(path, stamp, bm.w, bm.px.data(), bm.px.size());
        // Opportunistic GC: run after every successful put. Cheap at low usage.
        if ((stamp.size % 4096) == 0) thumbcache::enforceLimit(opts.cacheCfg);
    }

    return res;
}

bool renderPreview(const std::wstring& path, int px, Bitmap& out) {
    ThumbOpts opts;
    opts.px = px > 0 ? px : 256;
    opts.deadlineMs = 2000;
    opts.useCache = false;
    ThumbResult r = makeThumbnail(path, opts);
    if (!r.ok || r.bgra.empty()) return false;
    out = Bitmap(r.px, r.px);
    if (r.bgra.size() != (size_t)out.w * out.h * 4) return false;
    std::memcpy(out.px.data(), r.bgra.data(), r.bgra.size());
    return true;
}

}  // namespace bs3d
