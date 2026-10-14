#include "bs3d/thumbnailcore.h"
#include "bs3d/parsers.h"
#include "bs3d/rasterizer.h"
#include "bs3d/formats.h"

#include <cstdio>
#include <cstring>
#include <string>
#include <vector>

// 把 32bpp BGRA 位图写成 BMP，方便肉眼验收渲染结果（只看返回码不够）。
static bool writeBmp32(const std::wstring& path, const bs3d::Bitmap& bm) {
    if (bm.w <= 0 || bm.h <= 0) return false;
    const DWORD pixBytes = (DWORD)bm.w * (DWORD)bm.h * 4u;
    BITMAPFILEHEADER fh = {};
    BITMAPINFOHEADER ih = {};
    fh.bfType = 0x4D42;  // 'BM'
    fh.bfOffBits = sizeof(fh) + sizeof(ih);
    fh.bfSize = fh.bfOffBits + pixBytes;
    ih.biSize = sizeof(ih);
    ih.biWidth = bm.w;
    ih.biHeight = bm.h;          // 正数 = 自下而上
    ih.biPlanes = 1;
    ih.biBitCount = 32;
    ih.biCompression = BI_RGB;
    ih.biSizeImage = pixBytes;

    FILE* f = nullptr;
    if (_wfopen_s(&f, path.c_str(), L"wb") != 0 || !f) return false;
    fwrite(&fh, sizeof(fh), 1, f);
    fwrite(&ih, sizeof(ih), 1, f);
    // 源是自上而下，BMP 要求自下而上 -> 逐行倒序写
    for (int y = bm.h - 1; y >= 0; --y) {
        fwrite(bm.px.data() + (size_t)y * (size_t)bm.w * 4u, 4u, (size_t)bm.w, f);
    }
    fclose(f);
    return true;
}

int wmain(int argc, wchar_t** argv) {
    if (argc < 2) {
        wprintf(L"usage: thumb_probe <file> [out.bmp]\n");
        return 2;
    }
    const std::wstring p = argv[1];
    const std::wstring dumpPath = argc >= 3 ? argv[2] : std::wstring();

    const bs3d::FormatDef* fd = bs3d::formatOfPath(p);
    wprintf(L"formatDef=%p kind=%d thumbnailable=%d mayEmbed=%d\n",
            (const void*)fd, fd ? (int)fd->kind : -1,
            fd ? (fd->thumbnailable ? 1 : 0) : 0,
            fd ? (fd->mayEmbedImage ? 1 : 0) : 0);

    if (fd && fd->mayEmbedImage) {
        std::vector<bs3d::Byte> raw;
        bs3d::Rc er = bs3d::extractEmbeddedImage(p, raw);
        wprintf(L"extractEmbeddedImage rc=%d bytes=%zu\n", (int)er, raw.size());
    }

    bs3d::ParseOpts po;
    po.deadlineMs = GetTickCount64() + 8000;
    po.maxTriangles = 4000000;
    bs3d::Mesh m;
    bs3d::Kind kind = bs3d::Kind::Unknown;
    bs3d::Rc r = bs3d::parseMeshFor(p, m, kind, po);
    wprintf(L"parseMeshFor rc=%d(%S) tris=%zu verts=%zu hasBounds=%d bounds=[%.2f %.2f %.2f]..[%.2f %.2f %.2f]\n",
            (int)r, bs3d::rcToString(r), m.triangleCount(), m.vertexCount(),
            m.hasBounds ? 1 : 0,
            m.bmin.x, m.bmin.y, m.bmin.z, m.bmax.x, m.bmax.y, m.bmax.z);

    bs3d::Bitmap bm;
    bs3d::RenderStyle st;
    bool rr = bs3d::renderMesh(m, 128, bm, st, GetTickCount64() + 2000);
    int opaque = 0;
    for (size_t i = 3; i < bm.px.size(); i += 4)
        if (bm.px[i] > 0) ++opaque;
    wprintf(L"renderMesh ok=%d w=%d h=%d opaquePx=%d\n", rr ? 1 : 0, bm.w, bm.h, opaque);

    bs3d::ThumbOpts to;
    to.px = 128;
    to.deadlineMs = 2000;
    to.useCache = false;
    to.allowFallbackIcon = false;
    bs3d::ThumbResult tr = bs3d::makeThumbnail(p, to);
    wprintf(L"makeThumbnail ok=%d rc=%d reason=[%s] embedded=%d px=%d bgra=%zu\n",
            tr.ok ? 1 : 0, (int)tr.parseRc, tr.reason.c_str(),
            tr.fromEmbeddedImage ? 1 : 0, tr.px, tr.bgra.size());

    bs3d::Bitmap bm2;
    bool rp = bs3d::renderPreview(p, 128, bm2);
    wprintf(L"renderPreview ok=%d w=%d h=%d\n", rp ? 1 : 0, bm2.w, bm2.h);

    // 导出一张 BMP 供人工验收：优先预览图，退回缩略图，再退回网格渲染图
    if (!dumpPath.empty()) {
        const bs3d::Bitmap* src = nullptr;
        if (rp && bm2.w > 0) {
            src = &bm2;
        } else if (tr.ok && tr.px > 0 && !tr.bgra.empty()) {
            static bs3d::Bitmap tbm;
            tbm.w = tr.px;
            tbm.h = tr.px;
            tbm.px = tr.bgra;
            src = &tbm;
        } else if (rr && bm.w > 0) {
            src = &bm;
        }
        const bool ok = src && writeBmp32(dumpPath, *src);
        wprintf(L"dumpBmp ok=%d -> %s\n", ok ? 1 : 0, dumpPath.c_str());
    }
    return 0;
}
