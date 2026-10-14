#pragma once

#include "bs3d.h"
#include "formats.h"

namespace bs3d {

// One XY segment of a gcode toolpath, kept in raw file coordinates.
struct Seg2 {
    float x0 = 0.f, y0 = 0.f, x1 = 0.f, y1 = 0.f;
    float z = 0.f;
};

struct ParseOpts {
    size_t maxTriangles = 4000000;   // hard cap, avoid runaway memory
    size_t maxBytes = 256ULL << 20;  // read cap for streaming parsers
    uint64_t deadlineMs = 0;         // 0 = no limit; 64-bit to survive 49.7-day tick wrap
    bool stopNow() const { return deadlineMs != 0 && GetTickCount64() >= deadlineMs; }
};

// ---- mesh parsers ----------------------------------------------------------
Rc parseStl(const std::wstring& path, Mesh& out, const ParseOpts& o);
Rc parseObj(const std::wstring& path, Mesh& out, const ParseOpts& o);
Rc parsePly(const std::wstring& path, Mesh& out, const ParseOpts& o);
Rc parseOff(const std::wstring& path, Mesh& out, const ParseOpts& o);
Rc parse3ds(const std::wstring& path, Mesh& out, const ParseOpts& o);
Rc parseAmfMesh(const std::wstring& path, Mesh& out, const ParseOpts& o);
Rc parseFbxMesh(const std::wstring& path, Mesh& out, const ParseOpts& o);

// 3MF: merges every <mesh> block of the OPC package (transforms are ignored).
Rc parse3mfMesh(const std::wstring& path, Mesh& out, const ParseOpts& o);

// Dispatch on extension. Fills `kind` so callers can pick a renderer.
Rc parseMeshFor(const std::wstring& path, Mesh& out, Kind& kind, const ParseOpts& o);

// ---- embedded preview images ----------------------------------------------
// Extracts the first preview image (PNG/JPEG/BMP/GIF) stored inside a container
// format such as 3MF (OPC) or AMF (base64). Returns Rc::Unsupported when the
// format has no embedded image.
Rc extractEmbeddedImage(const std::wstring& path, std::vector<Byte>& out, uint64_t maxBytes = 16ULL << 20);

// ---- toolpath --------------------------------------------------------------
Rc parseToolpath(const std::wstring& path, std::vector<Seg2>& out, const ParseOpts& o);

// ---- decoders --------------------------------------------------------------
// Decode arbitrary image bytes (PNG/JPEG/BMP/GIF/ICO via GDI+) into an RGBA bitmap
// fitted into a `size` x `size` canvas with aspect-ratio preservation.
bool decodeImageBytes(const Byte* data, size_t dataLen, int px, Bitmap& out);

}  // namespace bs3d
