#pragma once

// Runtime plugin ABI between the bs3d core (loader) and bs3d_occ.dll
// (OpenCASCADE tessellator). The ABI is pure C / POD so both sides may link
// their own copy of the static CRT: all buffers are allocated AND freed
// inside the plugin; the loader only copies them before calling release().

#include <stddef.h>

#ifdef __cplusplus
#include <string>
#include <vector>
#include "bs3d.h"
#include "parsers.h"
#endif

#define BS3D_OCC_ABI_VERSION 1u
#define BS3D_OCC_EXPORT_NAME "bs3dOccMesh"

typedef struct Bs3dMeshOut {
    unsigned int abiVersion;   // caller fills with BS3D_OCC_ABI_VERSION
    float* positions;          // plugin-owned, 3 floats per vertex
    size_t positionCount;      // number of floats in positions
    unsigned int* indices;     // plugin-owned, 3 per triangle
    size_t indexCount;         // number of indices
    void (*release)(struct Bs3dMeshOut* out);  // plugin frees its buffers
} Bs3dMeshOut;

// Exported by bs3d_occ.dll. Returns 1 on success (fields filled + release set),
// 0 on failure. Must never let exceptions cross the boundary.
typedef int (*Bs3dOccMeshFn)(const wchar_t* path, Bs3dMeshOut* out);

#ifdef __cplusplus
namespace bs3d {

// Absolute paths probed for bs3d_occ.dll, in priority order.
std::vector<std::wstring> occPluginCandidates();

// Writable directory where the downloaded OCCT runtime pack is extracted
// (plugin DLL + OCCT runtime DLLs live side by side so the altered search
// path resolves them).
std::wstring occRuntimeDir();

// True when bs3d_occ.dll exists on disk (does not LoadLibrary).
bool occPackInstalled();

// Load the plugin once (LoadLibraryEx with altered search path). Thread-safe;
// repeated calls are cheap. Returns true when the entry point resolved.
bool occPluginAvailable();

// Tessellate a CAD file through the plugin. Copies the plugin mesh into `out`
// (bounds + vertex normals computed locally). Plugin faults are contained.
Rc meshCadViaPlugin(const std::wstring& path, Mesh& out, const ParseOpts& o);

}  // namespace bs3d
#endif
