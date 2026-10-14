#include "bs3d/occplugin.h"
#include "bs3d/winutil.h"

#include <windows.h>
#include <mutex>

namespace bs3d {
namespace {

HMODULE g_mod = nullptr;
Bs3dOccMeshFn g_fn = nullptr;
std::once_flag g_loadOnce;

void loadPlugin() {
    for (const auto& p : occPluginCandidates()) {
        if (!fileExists(p)) continue;
        // Altered search path: OCCT runtime DLLs sitting next to the plugin
        // resolve from the plugin directory, not from the host EXE directory
        // (the host may be explorer.exe).
        HMODULE m = LoadLibraryExW(p.c_str(), nullptr, LOAD_WITH_ALTERED_SEARCH_PATH);
        if (!m) continue;
        FARPROC proc = GetProcAddress(m, BS3D_OCC_EXPORT_NAME);
        if (!proc) {
            FreeLibrary(m);
            continue;
        }
        g_mod = m;
        g_fn = reinterpret_cast<Bs3dOccMeshFn>(proc);
        return;
    }
}

// SEH wrapper: MSVC forbids __try in functions with unwound C++ objects, so
// the raw call lives here with POD locals only.
int callPlugin(Bs3dOccMeshFn fn, const wchar_t* path, Bs3dMeshOut* out) {
    __try {
        return fn(path, out);
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return 0;
    }
}

}  // namespace

std::vector<std::wstring> occPluginCandidates() {
    std::vector<std::wstring> v;
    std::wstring d = programDir();
    if (!d.empty()) {
        v.push_back(d + L"\\bs3d_occ.dll");
        v.push_back(d + L"\\occt\\bs3d_occ.dll");
    }
    std::wstring lad = localAppDataDir();
    if (!lad.empty()) v.push_back(lad + L"\\Bambu3DAssoc\\occt\\bs3d_occ.dll");
    std::wstring pd = programDataDir();
    if (!pd.empty()) {
        v.push_back(pd + L"\\bs3d_occ.dll");
        v.push_back(pd + L"\\occt\\bs3d_occ.dll");
    }
    return v;
}

std::wstring occRuntimeDir() {
    // Prefer the install directory when it is writable so every user shares
    // one pack; fall back to per-user %LOCALAPPDATA%.
    std::wstring d = programDir();
    if (!d.empty()) {
        std::wstring occt = d + L"\\occt";
        if (ensureDirectory(occt)) {
            std::wstring probe = occt + L"\\.wtest";
            if (writeFileBytes(probe, "x", 1)) {
                removeFileSafe(probe);
                return occt;
            }
        }
    }
    std::wstring lad = localAppDataDir();
    if (!lad.empty()) {
        std::wstring occt = lad + L"\\Bambu3DAssoc\\occt";
        ensureDirectory(occt);
        return occt;
    }
    return programDataDir() + L"\\occt";
}

bool occPackInstalled() {
    for (const auto& p : occPluginCandidates()) {
        if (fileExists(p)) return true;
    }
    return false;
}

bool occPluginAvailable() {
    std::call_once(g_loadOnce, loadPlugin);
    return g_fn != nullptr;
}

Rc meshCadViaPlugin(const std::wstring& path, Mesh& out, const ParseOpts& o) {
    if (!occPluginAvailable()) return Rc::Unsupported;
    if (o.stopNow()) return Rc::Timeout;

    Bs3dMeshOut po = {};
    po.abiVersion = BS3D_OCC_ABI_VERSION;
    int ok = callPlugin(g_fn, path.c_str(), &po);

    // Validate before touching std containers; the plugin owns the buffers.
    const bool shapeOk =
        ok == 1 && po.positions != nullptr && po.indices != nullptr && po.release != nullptr &&
        po.positionCount >= 3 && po.positionCount % 3 == 0 && po.indexCount >= 3 &&
        po.indexCount % 3 == 0 && po.positionCount <= o.maxTriangles * 9 + 3 * 3 &&
        po.indexCount / 3 <= o.maxTriangles;
    if (!shapeOk) {
        if (ok == 1 && po.release) po.release(&po);
        return ok == 1 ? Rc::ParseError : Rc::Unsupported;
    }
    const size_t vertCount = po.positionCount / 3;
    for (size_t i = 0; i < po.indexCount; ++i) {
        if (po.indices[i] >= vertCount) {
            po.release(&po);
            return Rc::ParseError;
        }
    }

    Mesh m;
    bool copied = false;
    try {
        m.positions.assign(po.positions, po.positions + po.positionCount);
        m.indices.assign(po.indices, po.indices + po.indexCount);
        copied = true;
    } catch (...) {
        copied = false;
    }
    po.release(&po);
    if (!copied) return Rc::OutOfMemory;

    if (m.triangleCount() == 0) return Rc::EmptyFile;
    recomputeBounds(m);
    computeVertexNormals(m);
    m.hasNormals = true;
    m.hasColors = false;
    out = std::move(m);
    return Rc::Ok;
}

}  // namespace bs3d
