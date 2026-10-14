#include "bs3d/rasterizer.h"
#include "bs3d/parsers.h"

#include <algorithm>
#include <cmath>
#include <cstring>
#include <map>
#include <mutex>
#include <vector>

#include <windows.h>
#include <objidl.h>
#include <gdiplus.h>
#pragma comment(lib, "gdiplus.lib")

using namespace Gdiplus;

#ifndef M_PI
#define M_PI 3.14159265358979323846f
#endif

namespace bs3d {
namespace {

inline bool isFinite3(Vec3 v) {
    return std::isfinite(v.x) && std::isfinite(v.y) && std::isfinite(v.z);
}

struct FaceColor {
    Byte b = 0, g = 0, r = 0;
};

// --- camera basis builder ---------------------------------------------------
// fwd points FROM camera TO target (the "look" direction). We use the
// convention that the world's +Y is up after autoOrientUp.
struct CameraBasis {
    Vec3 fwd, right, up;
};

CameraBasis makeCamera(float azimuth, float elevation) {
    const float ce = cosf(elevation), se = sinf(elevation);
    const float ca = cosf(azimuth),   sa = sinf(azimuth);
    CameraBasis cb;
    cb.fwd   = {-ce * sa, -se, -ce * ca};
    cb.right = normalize(cross({0, 1, 0}, cb.fwd));
    cb.up    = cross(cb.fwd, cb.right);
    return cb;
}

// --- face colour ------------------------------------------------------------
// Lambert term from the key light, a softer fill light from the opposite side
// and a rim term that brightens silhouette edges — the three-light recipe used
// by f3d's default "physical" look, kept cheap enough for a thumbnail.
FaceColor computeFaceColor(const RenderStyle& s, float dotL, float fillL,
                           float rimL, bool front) {
    float intensity = s.ambient + s.diffuse * dotL + s.fill * fillL + s.rim * rimL;
    if (intensity > 1.f) intensity = 1.f;
    if (intensity < 0.f) intensity = 0.f;
    float k = intensity;
    FaceColor c;
    c.r = static_cast<Byte>(s.shadowR + (s.baseR - s.shadowR) * k);
    c.g = static_cast<Byte>(s.shadowG + (s.baseG - s.shadowG) * k);
    c.b = static_cast<Byte>(s.shadowB + (s.baseB - s.shadowB) * k);
    if (!front) {
        c.r = static_cast<Byte>(c.r * 0.55f);
        c.g = static_cast<Byte>(c.g * 0.55f);
        c.b = static_cast<Byte>(c.b * 0.55f);
    }
    return c;
}

// Fill a supersample buffer with the neutral top-to-bottom gradient. When
// gradientBg is off the buffer stays fully transparent so Explorer composites
// the model on whatever surface it likes.
void fillBackground(std::vector<Byte>& buf, int s, const RenderStyle& style) {
    if (!style.gradientBg) return;
    const float denom = (s > 1) ? static_cast<float>(s - 1) : 1.f;
    for (int y = 0; y < s; ++y) {
        const float t = static_cast<float>(y) / denom;
        const Byte r = static_cast<Byte>(style.bgTopR + (style.bgBotR - style.bgTopR) * t);
        const Byte g = static_cast<Byte>(style.bgTopG + (style.bgBotG - style.bgTopG) * t);
        const Byte b = static_cast<Byte>(style.bgTopB + (style.bgBotB - style.bgTopB) * t);
        Byte* row = &buf[static_cast<size_t>(y) * s * 4];
        for (int x = 0; x < s; ++x, row += 4) {
            row[0] = b; row[1] = g; row[2] = r; row[3] = 255;
        }
    }
}

}  // namespace

// ===========================================================================
// autoOrientUp / normaliseMesh
// ===========================================================================
int autoOrientUp(Mesh& mesh) {
    if (mesh.vertexCount() < 3) return -1;

    if (!mesh.hasNormals) computeVertexNormals(mesh);

    // Accumulate average |normal| along each axis. The axis with the highest
    // value is the one where the mesh's faces are most "vertical", which is
    // normally the up axis of the object as it sits on the build plate.
    double ax = 0, ay = 0, az = 0;
    const size_t n = mesh.vertexCount();
    for (size_t i = 0; i < n; ++i) {
        Vec3 v = mesh.nor(i);
        ax += fabs((double)v.x);
        ay += fabs((double)v.y);
        az += fabs((double)v.z);
    }

    int upAxis = 1;
    if (ax > ay && ax > az) upAxis = 0;
    else if (az > ay)       upAxis = 2;

    if (upAxis == 1) return 1;

    Mat3 R;
    if (upAxis == 0) {
        // +X -> +Y : rotate -90 deg about Z
        R = rotZ(-static_cast<float>(M_PI) / 2.f);
    } else {
        // +Z -> +Y : rotate +90 deg about X
        R = rotX(static_cast<float>(M_PI) / 2.f);
    }

    for (size_t i = 0; i < n; ++i) {
        Vec3 p = mesh.pos(i);
        Vec3 np = transformVec3(R, p);
        mesh.positions[i * 3]     = np.x;
        mesh.positions[i * 3 + 1] = np.y;
        mesh.positions[i * 3 + 2] = np.z;
        if (mesh.hasNormals) {
            Vec3 q = mesh.nor(i);
            Vec3 nq = transformVec3(R, q);
            mesh.normals[i * 3]     = nq.x;
            mesh.normals[i * 3 + 1] = nq.y;
            mesh.normals[i * 3 + 2] = nq.z;
        }
    }
    mesh.hasBounds = false;
    return upAxis;
}

void normaliseMesh(Mesh& mesh) {
    if (mesh.vertexCount() == 0) return;
    recomputeBounds(mesh);
    Vec3 mn = mesh.bmin, mx = mesh.bmax;
    Vec3 ctr = {0.5f * (mn.x + mx.x), 0.5f * (mn.y + mx.y), 0.5f * (mn.z + mx.z)};
    float ex = mx.x - mn.x, ey = mx.y - mn.y, ez = mx.z - mn.z;
    float maxE = fmaxf(fmaxf(ex, ey), ez);
    if (maxE < 1e-8f) maxE = 1.f;
    float k = 1.f / maxE;

    for (size_t i = 0; i < mesh.vertexCount(); ++i) {
        Vec3 p = mesh.pos(i);
        p.x = (p.x - ctr.x) * k;
        p.y = (p.y - ctr.y) * k;
        p.z = (p.z - ctr.z) * k;
        mesh.positions[i * 3]     = p.x;
        mesh.positions[i * 3 + 1] = p.y;
        mesh.positions[i * 3 + 2] = p.z;
    }
    mesh.hasBounds = false;
}

// ===========================================================================
// renderMesh - Lambert-shaded isometric render, 2x supersampled
// ===========================================================================
bool renderMesh(const Mesh& mesh, int px, Bitmap& out, const RenderStyle& style,
                uint64_t deadlineMs) {
    if (px <= 0) px = 128;
    if (px > 1024) px = 1024;
    if (mesh.triangleCount() == 0 || mesh.vertexCount() < 3) return false;
    if (mesh.vertexCount() > 300000) return false;

    Mesh m = mesh;
    autoOrientUp(m);
    normaliseMesh(m);
    if (!m.hasNormals) computeVertexNormals(m);

    const CameraBasis cb = makeCamera(style.azimuth, style.elevation);
    // 光源定义在相机坐标系里（相机怎么转，光就怎么跟），这样任意角度的模型
    // 都能拿到同一套“左上主光 + 右下补光”，不会出现某个方位全黑的情况。
    const Vec3 light = normalize(cb.right * -0.42f + cb.up * 0.60f + cb.fwd * -0.68f);
    const Vec3 fillLight = normalize(cb.right * 0.52f + cb.up * -0.30f + cb.fwd * -0.40f);
    const Vec3 rimDir = cb.fwd;  // 轮廓光：背对相机的面
    const Vec3 towardCam = {-cb.fwd.x, -cb.fwd.y, -cb.fwd.z};

    // ---- 相机自动取景：先量出投影包围盒，再缩放居中（对应 f3d 的 auto frame）--
    float minU = 1e30f, maxU = -1e30f, minV = 1e30f, maxV = -1e30f;
    for (size_t i = 0; i < m.vertexCount(); ++i) {
        const Vec3 p = m.pos(i);
        if (!isFinite3(p)) continue;
        const float u = dot(p, cb.right);
        const float v = dot(p, cb.up);
        if (u < minU) minU = u;
        if (u > maxU) maxU = u;
        if (v < minV) minV = v;
        if (v > maxV) maxV = v;
    }
    if (minU > maxU || minV > maxV) return false;
    const float centerU = 0.5f * (minU + maxU);
    const float centerV = 0.5f * (minV + maxV);
    float span = fmaxf(maxU - minU, maxV - minV);
    if (span < 1e-6f) span = 1.f;
    float fillFrac = style.frameFill;
    if (fillFrac < 0.2f) fillFrac = 0.2f;
    if (fillFrac > 1.0f) fillFrac = 1.0f;
    const float kFrame = 2.f * fillFrac / span;

    // Projected triangle: screen coords in [-1, 1], depth, world normal.
    struct PTri {
        float sx[3], sy[3];
        float depth;
        float dotL;
        float fillL;
        float rimL;
        bool front;
    };

    std::vector<PTri> tris;
    tris.reserve(m.triangleCount());

    const size_t triCount = m.triangleCount();

    for (size_t t = 0; t < triCount; ++t) {
        if (deadlineMs && (t & 0x1FF) == 0 && !withinBudget(deadlineMs)) return false;

        const unsigned int a = m.indices[t * 3];
        const unsigned int b = m.indices[t * 3 + 1];
        const unsigned int c = m.indices[t * 3 + 2];
        if (a >= m.vertexCount() || b >= m.vertexCount() || c >= m.vertexCount()) continue;

        Vec3 pa = m.pos(a), pb = m.pos(b), pc = m.pos(c);
        if (!isFinite3(pa) || !isFinite3(pb) || !isFinite3(pc)) continue;

        Vec3 n = normalize(cross(pb - pa, pc - pa));
        if (vlength(n) < 1e-6f) continue;

        PTri pt;
        pt.sx[0] = (dot(pa, cb.right) - centerU) * kFrame;
        pt.sy[0] = -(dot(pa, cb.up) - centerV) * kFrame;
        pt.sx[1] = (dot(pb, cb.right) - centerU) * kFrame;
        pt.sy[1] = -(dot(pb, cb.up) - centerV) * kFrame;
        pt.sx[2] = (dot(pc, cb.right) - centerU) * kFrame;
        pt.sy[2] = -(dot(pc, cb.up) - centerV) * kFrame;
        pt.depth = (dot(pa, cb.fwd) + dot(pb, cb.fwd) + dot(pc, cb.fwd)) * 0.333333f;

        float dl = dot(n, light); if (dl < 0.f) dl = 0.f;
        pt.dotL = dl;
        float fl = dot(n, fillLight); if (fl < 0.f) fl = 0.f;
        pt.fillL = fl;
        float rd = 1.f - fabsf(dot(n, rimDir));
        pt.rimL = rd * rd * rd;
        pt.front = (dot(n, towardCam) > 0.f);

        tris.push_back(pt);
    }

    if (tris.empty()) return false;

    // Back-to-front
    std::sort(tris.begin(), tris.end(),
              [](const PTri& x, const PTri& y) { return x.depth < y.depth; });

    // 2x supersample buffer
    const int s = px * 2;
    const float ss = static_cast<float>(s) * 0.5f;
    std::vector<Byte> buf(static_cast<size_t>(s) * s * 4, 0);
    fillBackground(buf, s, style);

    for (size_t i = 0; i < tris.size(); ++i) {
        const PTri& t = tris[i];
        if (deadlineMs && (i & 0x1FF) == 0 && !withinBudget(deadlineMs)) break;

        // Bounding box in supersample pixel coords
        int minx = s, maxx = -1, miny = s, maxy = -1;
        for (int k = 0; k < 3; ++k) {
            int x = (int)floorf((t.sx[k] + 1.f) * ss);
            int y = (int)floorf((t.sy[k] + 1.f) * ss);
            if (x < minx) minx = x;
            if (x > maxx) maxx = x;
            if (y < miny) miny = y;
            if (y > maxy) maxy = y;
        }
        if (maxx < minx || maxy < miny) continue;
        if (maxx >= s) maxx = s - 1;
        if (maxy >= s) maxy = s - 1;
        if (minx < 0) minx = 0;
        if (miny < 0) miny = 0;

        const float x0 = (t.sx[0] + 1.f) * ss, y0 = (t.sy[0] + 1.f) * ss;
        const float x1 = (t.sx[1] + 1.f) * ss, y1 = (t.sy[1] + 1.f) * ss;
        const float x2 = (t.sx[2] + 1.f) * ss, y2 = (t.sy[2] + 1.f) * ss;
        const float area = (x1 - x0) * (y2 - y0) - (x2 - x0) * (y1 - y0);
        if (fabsf(area) < 1e-10f) continue;

        const FaceColor fc = computeFaceColor(style, t.dotL, t.fillL, t.rimL, t.front);
        const float alpha = t.front ? 1.0f : style.backFaceAlpha;

        // Precompute barycentric edge functions
        const float x1mx0 = x1 - x0, y1my0 = y1 - y0;
        const float x2mx1 = x2 - x1, y2my1 = y2 - y1;
        const float invA = 1.f / area;

        for (int y = miny; y <= maxy; ++y) {
            const float py = static_cast<float>(y) + 0.5f;
            const float py_my0 = py - y0;
            const float py_my1 = py - y1;
            Byte* dstRow = &buf[(static_cast<size_t>(y) * s) * 4];
            for (int x = minx; x <= maxx; ++x) {
                const float px_ = static_cast<float>(x) + 0.5f;
                const float px_mx0 = px_ - x0;
                const float px_mx1 = px_ - x1;

                float w0 = (x1mx0 * py_my0 - y1my0 * px_mx0) * invA;
                float w1 = (x2mx1 * py_my1 - y2my1 * px_mx1) * invA;
                float w2 = 1.f - w0 - w1;
                if (w0 < 0.f || w1 < 0.f || w2 < 0.f) continue;

                Byte* dst = dstRow + static_cast<size_t>(x) * 4;
                if (t.front) {
                    dst[0] = fc.b; dst[1] = fc.g; dst[2] = fc.r; dst[3] = 255;
                } else {
                    const float a = alpha;
                    const float inv = 1.f - a;
                    dst[0] = static_cast<Byte>(fc.b * a + dst[0] * inv);
                    dst[1] = static_cast<Byte>(fc.g * a + dst[1] * inv);
                    dst[2] = static_cast<Byte>(fc.r * a + dst[2] * inv);
                    const int newA = (int)dst[3] + (int)(255 * alpha);
                    dst[3] = static_cast<Byte>(newA < 255 ? newA : 255);
                }
            }
        }
    }

    // Downsample 2x2 -> 1x1
    out = Bitmap(px, px);
    for (int y = 0; y < px; ++y) {
        Byte* outRow = out.row(y);
        for (int x = 0; x < px; ++x) {
            int sum[4] = {0, 0, 0, 0};
            for (int dy = 0; dy < 2; ++dy) {
                const Byte* p = &buf[((static_cast<size_t>(y * 2 + dy)) * s + (x * 2)) * 4];
                for (int dx = 0; dx < 2; ++dx, p += 4) {
                    sum[0] += p[0]; sum[1] += p[1]; sum[2] += p[2]; sum[3] += p[3];
                }
            }
            Byte* d = outRow + static_cast<size_t>(x) * 4;
            d[0] = static_cast<Byte>(sum[0] >> 2);
            d[1] = static_cast<Byte>(sum[1] >> 2);
            d[2] = static_cast<Byte>(sum[2] >> 2);
            d[3] = static_cast<Byte>(sum[3] >> 2);
        }
    }
    return true;
}

// ===========================================================================
// renderToolpath - top-down orthographic view, layered by Z
// ===========================================================================
bool renderToolpath(const std::vector<Seg2>& segs, int px, Bitmap& out,
                    const RenderStyle& style) {
    if (px <= 0) px = 128;
    if (segs.empty()) return false;

    out = Bitmap(px, px);
    out.clear();

    float minx = segs[0].x0, maxx = segs[0].x0;
    float miny = segs[0].y0, maxy = segs[0].y0;
    float minz = segs[0].z,  maxz = segs[0].z;
    for (const Seg2& s : segs) {
        minx = fminf(minx, fminf(s.x0, s.x1));
        maxx = fmaxf(maxx, fmaxf(s.x0, s.x1));
        miny = fminf(miny, fminf(s.y0, s.y1));
        maxy = fmaxf(maxy, fmaxf(s.y0, s.y1));
        minz = fminf(minz, s.z);
        maxz = fmaxf(maxz, s.z);
    }
    float ex = maxx - minx;
    float ey = maxy - miny;
    if (ex < 1e-6f) ex = 1.f;
    if (ey < 1e-6f) ey = 1.f;
    const float ez = fmaxf(1e-6f, maxz - minz);

    const float scale = fminf(ex, ey);
    const float k = (px * 0.42f) / scale;
    const float cx = 0.5f * (minx + maxx);
    const float cy = 0.5f * (miny + maxy);

    // Supersample for AA
    const int s = px * 2;
    std::vector<Byte> buf(static_cast<size_t>(s) * s * 4, 0);
    fillBackground(buf, s, style);

    // Bucket into a fixed number of layers so we don't allocate per unique Z.
    const int LAYERS = 48;
    std::vector<std::vector<int>> buckets(LAYERS);
    for (size_t i = 0; i < segs.size(); ++i) {
        int li = (int)((segs[i].z - minz) / ez * (LAYERS - 1));
        if (li < 0) li = 0;
        if (li >= LAYERS) li = LAYERS - 1;
        buckets[li].push_back(static_cast<int>(i));
    }

    const float half = static_cast<float>(s) * 0.5f;

    // Back to front: low-Z drawn first
    for (int li = 0; li < LAYERS; ++li) {
        if (buckets[li].empty()) continue;
        const float t = static_cast<float>(li) / static_cast<float>(LAYERS - 1);
        const Byte r = static_cast<Byte>(style.baseR * (0.55f + 0.45f * t));
        const Byte g = static_cast<Byte>(style.baseG * (0.55f + 0.45f * t));
        const Byte b = static_cast<Byte>(style.baseB * (0.70f + 0.30f * t));

        for (int idx : buckets[li]) {
            const Seg2& s2 = segs[idx];
            const float x0 = (s2.x0 - cx) * k + half;
            const float y0 = -(s2.y0 - cy) * k + half;
            const float x1 = (s2.x1 - cx) * k + half;
            const float y1 = -(s2.y1 - cy) * k + half;

            const float dx = x1 - x0, dy = y1 - y0;
            int steps = std::max(1, std::max((int)std::ceil(fabsf(dx)), (int)std::ceil(fabsf(dy))));
            if (steps > 8192) steps = 8192;
            const float stepx = dx / steps;
            const float stepy = dy / steps;

            float fx = x0, fy = y0;
            for (int i = 0; i <= steps; ++i) {
                const int ix = (int)std::floor(fx);
                const int iy = (int)std::floor(fy);
                // 2x2 brush for AA
                for (int oy = 0; oy < 2; ++oy) {
                    const int yy = iy + oy;
                    if (yy < 0 || yy >= s) continue;
                    Byte* dstRow = &buf[(static_cast<size_t>(yy) * s) * 4];
                    for (int ox = 0; ox < 2; ++ox) {
                        const int xx = ix + ox;
                        if (xx < 0 || xx >= s) continue;
                        Byte* dst = dstRow + static_cast<size_t>(xx) * 4;
                        const float a = 0.85f;
                        dst[0] = static_cast<Byte>(b * a + dst[0] * (1.f - a));
                        dst[1] = static_cast<Byte>(g * a + dst[1] * (1.f - a));
                        dst[2] = static_cast<Byte>(r * a + dst[2] * (1.f - a));
                        const int na = (int)dst[3] + 64;
                        dst[3] = static_cast<Byte>(na < 255 ? na : 255);
                    }
                }
                fx += stepx;
                fy += stepy;
            }
        }
    }

    // Downsample
    for (int y = 0; y < px; ++y) {
        Byte* outRow = out.row(y);
        for (int x = 0; x < px; ++x) {
            int sum[4] = {0, 0, 0, 0};
            for (int dy = 0; dy < 2; ++dy) {
                const Byte* p = &buf[((static_cast<size_t>(y * 2 + dy)) * s + (x * 2)) * 4];
                for (int dx = 0; dx < 2; ++dx, p += 4) {
                    sum[0] += p[0]; sum[1] += p[1]; sum[2] += p[2]; sum[3] += p[3];
                }
            }
            Byte* d = outRow + static_cast<size_t>(x) * 4;
            d[0] = static_cast<Byte>(sum[0] >> 2);
            d[1] = static_cast<Byte>(sum[1] >> 2);
            d[2] = static_cast<Byte>(sum[2] >> 2);
            d[3] = static_cast<Byte>(sum[3] >> 2);
        }
    }
    return true;
}

// ===========================================================================
// drawFallbackBadge - a soft placeholder card with a centred label
// ===========================================================================
void drawFallbackBadge(Bitmap& out, const wchar_t* text) {
    if (out.w <= 0 || out.h <= 0) return;

    for (size_t i = 0; i < out.px.size(); i += 4) {
        out.px[i]     = 236;  // B
        out.px[i + 1] = 238;  // G
        out.px[i + 2] = 242;  // R
        out.px[i + 3] = 255;
    }

    static std::once_flag once;
    static ULONG_PTR token = 0;
    std::call_once(once, [] {
        GdiplusStartupInput in;
        GdiplusStartup(&token, &in, nullptr);
    });
    if (!token) return;

    Gdiplus::Bitmap gbmp(out.w, out.h, out.w * 4, PixelFormat32bppPARGB, out.px.data());
    Graphics g(&gbmp);
    g.SetSmoothingMode(SmoothingModeAntiAlias);
    g.SetTextRenderingHint(TextRenderingHintClearTypeGridFit);

    FontFamily ff(L"Segoe UI");
    const float fontPx = static_cast<float>(out.h) * 0.16f;
    Font font(&ff, fontPx, FontStyleRegular, UnitPixel);

    SolidBrush soft(Color(120, 255, 255, 255));
    SolidBrush fill(Color(255, 100, 108, 118));

    RectF layout(0, 0, static_cast<REAL>(out.w), static_cast<REAL>(out.h));
    RectF shadowLayout = layout;
    shadowLayout.Offset(1.0f, 1.0f);
    StringFormat sf;
    sf.SetAlignment(StringAlignmentCenter);
    sf.SetLineAlignment(StringAlignmentCenter);

    const int n = text ? (int)wcslen(text) : 0;
    if (n > 0) {
        g.DrawString(text, n, &font, shadowLayout, &sf, &soft);
        g.DrawString(text, n, &font, layout, &sf, &fill);
    }
}

}  // namespace bs3d
