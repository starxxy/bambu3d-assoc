#pragma once

#include "bs3d.h"
#include "parsers.h"

namespace bs3d {

// Look for the axis that best acts as "up" (the one along which the model rests
// on a flat bottom plane) and rotate the mesh so +Y is up. Returns the axis that
// was chosen (-1 when the mesh is empty).
int autoOrientUp(Mesh& mesh);

// Normalise the mesh into a centred unit cube (largest extent == 1).
void normaliseMesh(Mesh& mesh);

struct RenderStyle {
    float ambient = 0.40f;
    float diffuse = 0.55f;   // 主光（key light）
    float fill = 0.28f;      // 补光（fill light），柔化背光面
    float rim = 0.26f;
    float backFaceAlpha = 0.22f;
    Byte baseR = 168, baseG = 178, baseB = 196;   // neutral blueprint-ish grey
    Byte shadowR = 62, shadowG = 72, shadowB = 92;
    float azimuth = 0.62f;    // radians
    float elevation = 0.34f;  // radians
    // 相机自动取景：投影包围盒占画面的比例（0.86 = 四周留 7% 边距）。
    float frameFill = 0.86f;
    // 中性渐变背景（上浅下深），与浅色主题一致；关闭则保持透明背景。
    bool gradientBg = true;
    Byte bgTopR = 246, bgTopG = 247, bgTopB = 250;
    Byte bgBotR = 219, bgBotG = 224, bgBotB = 232;
};

// Render a mesh into a square bitmap. 2x supersampling (anti-aliased edges);
// the camera auto-frames the model and the lights travel with the camera.
// Never throws. Returns false when the mesh is empty or the deadline is exceeded.
bool renderMesh(const Mesh& mesh, int px, Bitmap& out, const RenderStyle& style,
                uint64_t deadlineMs);

// Draw a top-down toolpath preview: one polyline per z layer, coloured by layer.
bool renderToolpath(const std::vector<Seg2>& segs, int px, Bitmap& out,
                    const RenderStyle& style);

// Utility: draw a small fallback badge ("no preview") so users can tell a file
// was seen but could not be rendered.
void drawFallbackBadge(Bitmap& out, const wchar_t* text);

}  // namespace bs3d
