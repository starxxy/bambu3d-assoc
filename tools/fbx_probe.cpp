#include "bs3d/parsers.h"
#include "bs3d/formats.h"
#include "bs3d/rasterizer.h"
#include "bs3d/thumbnailcore.h"

#include <cstdio>
#include <string>

int wmain(int argc, wchar_t** argv) {
    if (argc < 2) {
        wprintf(L"usage: fbx_probe <file.fbx>\n");
        return 2;
    }
    const std::wstring p = argv[1];

    const bs3d::FormatDef* fd = bs3d::formatOfPath(p);
    wprintf(L"file=%s\nformatDef=%p ext=%s kind=%d thumbnailable=%d\n",
            p.c_str(), (const void*)fd, fd ? fd->ext : L"?",
            fd ? (int)fd->kind : -1, fd ? (fd->thumbnailable ? 1 : 0) : 0);

    bs3d::ParseOpts po;
    po.deadlineMs = GetTickCount64() + 5000;
    po.maxTriangles = 400000;
    po.maxBytes = 256ULL << 20;
    bs3d::Mesh m;
    bs3d::Kind kind = bs3d::Kind::Unknown;
    bs3d::Rc r = bs3d::parseMeshFor(p, m, kind, po);
    wprintf(L"parseMeshFor rc=%d(%S) kind=%d tris=%zu verts=%zu hasBounds=%d hasNormals=%d\n",
            (int)r, bs3d::rcToString(r), (int)kind, m.triangleCount(), m.vertexCount(),
            m.hasBounds ? 1 : 0, m.hasNormals ? 1 : 0);
    if (m.hasBounds) {
        wprintf(L"  bounds=[%.3f %.3f %.3f]..[%.3f %.3f %.3f]\n",
                m.bmin.x, m.bmin.y, m.bmin.z, m.bmax.x, m.bmax.y, m.bmax.z);
    }
    if (m.vertexCount() > 0 && m.vertexCount() <= 8) {
        for (size_t i = 0; i < m.vertexCount(); ++i) {
            wprintf(L"  v%d = %.3f %.3f %.3f\n", (int)i, m.positions[i*3], m.positions[i*3+1], m.positions[i*3+2]);
        }
    }
    if (m.triangleCount() > 0 && m.triangleCount() <= 8) {
        for (size_t i = 0; i < m.triangleCount(); ++i) {
            wprintf(L"  t%d = %u %u %u\n", (int)i, m.indices[i*3], m.indices[i*3+1], m.indices[i*3+2]);
        }
    }

    bs3d::Bitmap bm;
    bs3d::RenderStyle st;
    bool rr = bs3d::renderMesh(m, 128, bm, st, GetTickCount() + 2000);
    int opaque = 0;
    for (size_t i = 3; i < bm.px.size(); i += 4)
        if (bm.px[i] > 0) ++opaque;
    wprintf(L"renderMesh ok=%d w=%d h=%d opaquePx=%d\n", rr ? 1 : 0, bm.w, bm.h, opaque);

    return (r == bs3d::Rc::Ok && m.triangleCount() > 0) ? 0 : 1;
}
