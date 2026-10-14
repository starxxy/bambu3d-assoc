#pragma once

#include "bs3d.h"
#include "parsers.h"

namespace bs3d {

struct CadOpts {
    float deflection = 0.01f;     // relative angular/linear tolerance for curves
    int samplesPerArc = 48;       // polyline density for analytic curves
    int gridPerFace = 24;         // UV grid resolution when clipping a face patch
};

// ---- built-in fallback: minimal ISO-10303-21 (STEP AP203/214) reader -------
// Walks SHAPE_REPRESENTATION -> ADVANCED_FACE -> ORIENTED_SURFACE -> surface
// entity, tessellates analytic surfaces and clips the samples to the face loop
// polygons. Supports PLANE / CYLINDER / CONE / SPHERE / TORUS, linear
// extrusion, surface of revolution, B-spline surfaces and offset surfaces;
// edges support LINE / CIRCLE / ELLIPSE / B-spline / offset curves. Anything
// unrecognised is skipped rather than failing the whole file.
Rc meshStepBuiltin(const std::wstring& path, Mesh& out, const ParseOpts& o, const CadOpts& opts);

// ---- optional OpenCASCADE backend (runtime plugin) --------------------------
// The real OCCT tessellator ships as a separate bs3d_occ.dll discovered at
// runtime by the loader in occplugin.h (installed through the in-app
// environment-check downloader, no recompile or reinstall needed).
// meshCad() below transparently uses it when present.

// Dispatch used by the thumbnail pipeline for Cad files: tries the built-in
// STEP reader first, then the runtime OCCT plugin. IGES/BREP are only
// supported through the plugin; when neither works the result is
// Rc::Unsupported so the caller can show a fallback badge.
Rc meshCad(const std::wstring& path, Mesh& out, const ParseOpts& o, const CadOpts& opts,
           bool* usedOcc = nullptr);

}  // namespace bs3d
