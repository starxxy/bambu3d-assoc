// bs3d_occ.dll -- OpenCASCADE tessellator plugin for the bs3d thumbnail
// pipeline. Built only with BUILD_OCC_PLUGIN=ON on a machine that has a real
// OpenCASCADE toolchain (see tools/build_occt_pack.ps1). The manager exe and
// the shell extension DLL NEVER link this module: it is discovered at runtime
// via LoadLibraryEx and called through the pure-C ABI in
// include/bs3d/occplugin.h. That keeps the plugin out of explorer.exe's crash
// domain: any OCCT fault is contained and converts to a plain failure code.

#include <windows.h>

#include <cmath>
#include <cstring>
#include <cwctype>
#include <string>
#include <vector>

#include <algorithm>

#include <TCollection_AsciiString.hxx>
#include <IFSelect_ReturnStatus.hxx>
#include <STEPControl_Reader.hxx>
#include <IGESControl_Reader.hxx>
#include <BRepMesh_IncrementalMesh.hxx>
#include <BRep_Tool.hxx>
#include <Poly_Triangulation.hxx>
#include <Poly_Triangle.hxx>
#include <Standard_Failure.hxx>
#include <TopAbs_ShapeEnum.hxx>
#include <TopExp_Explorer.hxx>
#include <TopLoc_Location.hxx>
#include <TopoDS.hxx>
#include <TopoDS_Face.hxx>
#include <TopoDS_Shape.hxx>
#include <gp_Pnt.hxx>
#include <gp_Trsf.hxx>

#include "bs3d/occplugin.h"

namespace {

// Safety caps: thumbnails never need multi-million-triangle meshes, and this
// runs inside explorer.exe via the shell extension.
constexpr size_t kMaxVertices = 4000000;
constexpr size_t kMaxTriangles = 6000000;

// Tessellation parameters: 0.5 % relative deflection, ~5 degrees angular.
constexpr double kLinearDeflection = 0.005;
constexpr double kAngularDeflection = 0.0873;

std::string utf8FromWide(const std::wstring& w) {
    if (w.empty()) return std::string();
    const int n = WideCharToMultiByte(CP_UTF8, 0, w.c_str(), static_cast<int>(w.size()),
                                      nullptr, 0, nullptr, nullptr);
    if (n <= 0) return std::string();
    std::string s(static_cast<size_t>(n), '\0');
    WideCharToMultiByte(CP_UTF8, 0, w.c_str(), static_cast<int>(w.size()),
                        &s[0], n, nullptr, nullptr);
    return s;
}

std::wstring extensionOf(const std::wstring& path) {
    const size_t slash = path.find_last_of(L"\\/");
    const size_t dot = path.find_last_of(L'.');
    if (dot == std::wstring::npos || (slash != std::wstring::npos && dot < slash)) {
        return std::wstring();
    }
    std::wstring ext = path.substr(dot + 1);
    for (wchar_t& c : ext) c = static_cast<wchar_t>(towlower(c));
    return ext;
}

bool readStep(const std::wstring& path, TopoDS_Shape& shape) {
    STEPControl_Reader reader;
    const TCollection_AsciiString fn(utf8FromWide(path).c_str());
    if (reader.ReadFile(fn.ToCString()) != IFSelect_RetDone) return false;
    if (reader.NbRootsForTransfer() == 0) return false;
    reader.TransferRoots();
    shape = reader.OneShape();
    return !shape.IsNull();
}

bool readIges(const std::wstring& path, TopoDS_Shape& shape) {
    IGESControl_Reader reader;
    const TCollection_AsciiString fn(utf8FromWide(path).c_str());
    if (reader.ReadFile(fn.ToCString()) != IFSelect_RetDone) return false;
    if (reader.NbRootsForTransfer() == 0) return false;
    reader.TransferRoots();
    shape = reader.OneShape();
    return !shape.IsNull();
}

// Extension decides first; fall back to trying the other reader so mislabeled
// files still produce a thumbnail.
bool readShape(const std::wstring& path, TopoDS_Shape& shape) {
    const std::wstring ext = extensionOf(path);
    if (ext == L"iges" || ext == L"igs") {
        if (readIges(path, shape)) return true;
        return readStep(path, shape);
    }
    if (readStep(path, shape)) return true;
    return readIges(path, shape);
}

bool collectFaces(const TopoDS_Shape& shape, std::vector<float>& positions,
                  std::vector<unsigned int>& indices) {
    for (TopExp_Explorer exp(shape, TopAbs_FACE); exp.More(); exp.Next()) {
        const TopoDS_Face face = TopoDS::Face(exp.Current());
        TopLoc_Location loc;
        Handle(Poly_Triangulation) tri = BRep_Tool::Triangulation(face, loc);
        if (tri.IsNull() || tri->NbTriangles() <= 0) continue;

        const gp_Trsf& trsf = loc.Transformation();
        const bool reversed = (face.Orientation() == TopAbs_REVERSED);
        const unsigned int nodeBase = static_cast<unsigned int>(positions.size() / 3);

        const Standard_Integer nbNodes = tri->NbNodes();
        for (Standard_Integer i = 1; i <= nbNodes; ++i) {
            gp_Pnt p = tri->Node(i);
            p.Transform(trsf);
            positions.push_back(static_cast<float>(p.X()));
            positions.push_back(static_cast<float>(p.Y()));
            positions.push_back(static_cast<float>(p.Z()));
        }

        const Standard_Integer nbTris = tri->NbTriangles();
        for (Standard_Integer i = 1; i <= nbTris; ++i) {
            const Poly_Triangle& t = tri->Triangle(i);
            Standard_Integer a = t.Value(1);
            Standard_Integer b = t.Value(2);
            Standard_Integer c = t.Value(3);
            if (reversed) std::swap(b, c);
            indices.push_back(nodeBase + static_cast<unsigned int>(a - 1));
            indices.push_back(nodeBase + static_cast<unsigned int>(b - 1));
            indices.push_back(nodeBase + static_cast<unsigned int>(c - 1));
        }

        if (positions.size() / 3 > kMaxVertices || indices.size() / 3 > kMaxTriangles) {
            return false;
        }
    }
    return true;
}

void freeOut(Bs3dMeshOut* out) {
    if (!out) return;
    delete[] out->positions;
    delete[] out->indices;
    out->positions = nullptr;
    out->indices = nullptr;
    out->positionCount = 0;
    out->indexCount = 0;
    out->release = nullptr;
}

}  // namespace

extern "C" __declspec(dllexport) int bs3dOccMesh(const wchar_t* path,
                                                 Bs3dMeshOut* out) {
    if (!path || !out || out->abiVersion != BS3D_OCC_ABI_VERSION) return 0;
    out->positions = nullptr;
    out->positionCount = 0;
    out->indices = nullptr;
    out->indexCount = 0;
    out->release = nullptr;

    try {
        const std::wstring wpath(path);
        TopoDS_Shape shape;
        if (!readShape(wpath, shape)) return 0;

        BRepMesh_IncrementalMesh mesher(shape, kLinearDeflection, Standard_True,
                                        kAngularDeflection, Standard_True);
        if (!mesher.IsDone()) return 0;

        std::vector<float> positions;
        std::vector<unsigned int> indices;
        if (!collectFaces(shape, positions, indices)) return 0;
        if (positions.empty() || indices.empty()) return 0;

        // Plugin-side allocation; the loader copies before calling release().
        float* pbuf = new (std::nothrow) float[positions.size()];
        unsigned int* ibuf = new (std::nothrow) unsigned int[indices.size()];
        if (!pbuf || !ibuf) {
            delete[] pbuf;
            delete[] ibuf;
            return 0;
        }
        std::memcpy(pbuf, positions.data(), positions.size() * sizeof(float));
        std::memcpy(ibuf, indices.data(), indices.size() * sizeof(unsigned int));

        out->positions = pbuf;
        out->positionCount = positions.size();
        out->indices = ibuf;
        out->indexCount = indices.size();
        out->release = &freeOut;
        return 1;
    } catch (const Standard_Failure&) {
        return 0;
    } catch (...) {
        return 0;
    }
}
