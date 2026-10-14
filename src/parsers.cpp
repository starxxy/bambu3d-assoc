#include "bs3d/parsers.h"
#include "bs3d/zipread.h"

#include <algorithm>
#include <cctype>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <map>
#include <set>
#include <sstream>
#include <string>
#include <vector>

#include <windows.h>
#include <objidl.h>
#include <gdiplus.h>
#pragma comment(lib, "gdiplus.lib")

namespace bs3d {

namespace {

// ---------------------------------------------------------------------------
// Text parsing helpers
// ---------------------------------------------------------------------------
struct TextStream {
    std::string text;
    size_t pos = 0;

    static TextStream readFile(const std::wstring& path, size_t cap) {
        TextStream ts;
        HANDLE h = CreateFileW(path.c_str(), GENERIC_READ, FILE_SHARE_READ, nullptr,
                               OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
        if (h == INVALID_HANDLE_VALUE) return ts;
        LARGE_INTEGER sz;
        if (!GetFileSizeEx(h, &sz) || sz.QuadPart <= 0) { CloseHandle(h); return ts; }
        size_t n = static_cast<size_t>(sz.QuadPart);
        if (n > cap) n = cap;
        ts.text.resize(n);
        DWORD got = 0;
        if (!ReadFile(h, ts.text.data(), static_cast<DWORD>(n), &got, nullptr)) {
            CloseHandle(h); ts.text.clear(); return ts;
        }
        CloseHandle(h);
        ts.text.resize(got);
        return ts;
    }

    bool eof() const { return pos >= text.size(); }
    void skipWs() {
        while (pos < text.size()) {
            char c = text[pos];
            if (c == ' ' || c == '\t' || c == '\r' || c == '\n') ++pos; else break;
        }
    }
    void eol() {
        skipWs();
        if (pos < text.size() && text[pos] == '\r') ++pos;
        if (pos < text.size() && text[pos] == '\n') ++pos;
    }
    std::string word() {
        skipWs();
        size_t start = pos;
        while (pos < text.size()) {
            char c = text[pos];
            if (c == ' ' || c == '\t' || c == '\r' || c == '\n') break;
            ++pos;
        }
        return text.substr(start, pos - start);
    }
    float number() {
        skipWs();
        const char* p = text.data() + pos;
        char* end = nullptr;
        float v = static_cast<float>(std::strtof(p, &end));
        if (end == p) return 0.f;
        pos = static_cast<size_t>(end - text.data());
        return v;
    }
};

std::string toLowerAscii(std::string s) {
    std::transform(s.begin(), s.end(), s.begin(),
                   [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
    return s;
}

void meshClear(Mesh& m) {
    m.positions.clear();
    m.normals.clear();
    m.indices.clear();
    m.colors.clear();
    m.hasBounds = false;
    m.hasNormals = false;
    m.hasColors = false;
}

// Emit one triangle into the flat float Mesh. Each vertex becomes 3 floats.
inline void addTri(Mesh& m, Vec3 a, Vec3 b, Vec3 c, size_t triCap) {
    if (m.indices.size() / 3 >= triCap) return;
    const size_t base = m.positions.size() / 3;
    m.positions.push_back(a.x); m.positions.push_back(a.y); m.positions.push_back(a.z);
    m.positions.push_back(b.x); m.positions.push_back(b.y); m.positions.push_back(b.z);
    m.positions.push_back(c.x); m.positions.push_back(c.y); m.positions.push_back(c.z);
    m.indices.push_back(static_cast<uint32_t>(base));
    m.indices.push_back(static_cast<uint32_t>(base + 1));
    m.indices.push_back(static_cast<uint32_t>(base + 2));
}

void xmlAttrValue(const std::string& s, size_t pos, const char* attr, std::string& out) {
    size_t p = s.find(attr, pos);
    if (p == std::string::npos) return;
    if (p > 0) {
        char prev = s[p - 1];
        if (std::isalnum(static_cast<unsigned char>(prev)) || prev == '_') return;
    }
    size_t eq = s.find('=', p);
    if (eq == std::string::npos) return;
    size_t q = s.find('"', eq);
    if (q == std::string::npos) return;
    size_t q2 = s.find('"', q + 1);
    if (q2 == std::string::npos) return;
    out = s.substr(q + 1, q2 - q - 1);
}

bool base64Decode(const std::string& in, std::vector<Byte>& out) {
    int tbl[256];
    for (int i = 0; i < 256; ++i) tbl[i] = -1;
    for (int i = 0; i < 26; ++i) tbl['A' + i] = i;
    for (int i = 0; i < 26; ++i) tbl['a' + i] = 26 + i;
    for (int i = 0; i < 10; ++i) tbl['0' + i] = 52 + i;
    tbl['+'] = 62; tbl['/'] = 63;
    out.clear();
    int val = 0, valb = -8;
    for (unsigned char c : in) {
        if (c == '=' || c == '\n' || c == '\r' || c == ' ' || c == '\t') continue;
        int t = tbl[c];
        if (t < 0) continue;
        val = (val << 6) | t;
        valb += 6;
        if (valb >= 0) {
            out.push_back(static_cast<Byte>((val >> valb) & 0xFF));
            valb -= 8;
        }
    }
    return true;
}

}  // namespace

// ----------------------------------------------------------------- STL -------

Rc parseStl(const std::wstring& path, Mesh& out, const ParseOpts& o) {
    meshClear(out);
    TextStream ts = TextStream::readFile(path, o.maxBytes);
    if (ts.text.empty()) return Rc::IoError;
    const char* head = ts.text.c_str();
    bool ascii = false;
    if (ts.text.size() >= 5 && head[0] == 's' && head[1] == 'o' && head[2] == 'l' &&
        head[3] == 'i' && head[4] == 'd') {
        ascii = ts.text.find("endsolid") != std::string::npos;
    }
    if (ascii) {
        TextStream scan = ts;
        Vec3 v0, v1, v2;
        int triIdx = 0;
        bool inFacet = false;
        while (!scan.eof()) {
            if (o.stopNow()) return Rc::Timeout;
            std::string w = scan.word();
            if (w == "facet") { inFacet = true; triIdx = 0; continue; }
            if (w == "vertex" && inFacet) {
                float x = scan.number(), y = scan.number(), z = scan.number();
                if (triIdx == 0) v0 = {x, y, z};
                else if (triIdx == 1) v1 = {x, y, z};
                else if (triIdx == 2) v2 = {x, y, z};
                ++triIdx;
                continue;
            }
            if (w == "endfacet" && inFacet && triIdx >= 3) {
                addTri(out, v0, v1, v2, o.maxTriangles);
                inFacet = false;
                continue;
            }
            scan.eol();
        }
        if (out.indices.empty()) return Rc::ParseError;
        recomputeBounds(out);
        computeVertexNormals(out);
        return Rc::Ok;
    }
    // binary STL
    if (ts.text.size() < 84) return Rc::ParseError;
    uint32_t count = 0;
    std::memcpy(&count, ts.text.data() + 80, 4);
    if (84 + static_cast<size_t>(count) * 50 > ts.text.size()) return Rc::ParseError;
    for (uint32_t i = 0; i < count; ++i) {
        if (o.stopNow()) return Rc::Timeout;
        if (out.indices.size() / 3 >= o.maxTriangles) break;
        const unsigned char* p = reinterpret_cast<const unsigned char*>(
            ts.text.data() + 84 + static_cast<size_t>(i) * 50);
        float ax, ay, az, bx, by, bz, cx, cy, cz;
        std::memcpy(&ax, p + 12, 4); std::memcpy(&ay, p + 16, 4); std::memcpy(&az, p + 20, 4);
        std::memcpy(&bx, p + 24, 4); std::memcpy(&by, p + 28, 4); std::memcpy(&bz, p + 32, 4);
        std::memcpy(&cx, p + 36, 4); std::memcpy(&cy, p + 40, 4); std::memcpy(&cz, p + 44, 4);
        addTri(out, {ax, ay, az}, {bx, by, bz}, {cx, cy, cz}, o.maxTriangles);
    }
    if (out.indices.empty()) return Rc::ParseError;
    recomputeBounds(out);
    computeVertexNormals(out);
    return Rc::Ok;
}

// ------------------------------------------------------------------ OBJ ------

Rc parseObj(const std::wstring& path, Mesh& out, const ParseOpts& o) {
    meshClear(out);
    TextStream ts = TextStream::readFile(path, o.maxBytes);
    if (ts.text.empty()) return Rc::IoError;
    std::vector<Vec3> verts;
    verts.reserve(1024);
    while (!ts.eof()) {
        if (o.stopNow()) return Rc::Timeout;
        ts.skipWs();
        if (ts.pos >= ts.text.size()) break;
        char c = ts.text[ts.pos];
        if (c == 'v' || c == 'f') {
            if (c == 'v' && ts.pos + 1 < ts.text.size() && ts.text[ts.pos + 1] == 'n') {
                ++ts.pos;  // skip 'n'
                float x = ts.number(), y = ts.number(), z = ts.number();
                // Normal, ignore
                (void)x; (void)y; (void)z;
                continue;
            }
            if (c == 'v' && ts.pos + 1 < ts.text.size() && ts.text[ts.pos + 1] == 't') {
                ++ts.pos; ts.eol(); continue;
            }
            if (c == 'v') {
                float x = ts.number(), y = ts.number(), z = ts.number();
                verts.push_back({x, y, z});
                continue;
            }
            // c == 'f'
            std::vector<uint32_t> ids;
            bool bad = false;
            while (!ts.eof()) {
                std::string tok = ts.word();
                if (tok.empty()) break;
                size_t slash = tok.find('/');
                std::string idxStr = tok.substr(0, slash);
                if (idxStr.empty()) { bad = true; break; }
                int32_t idx = static_cast<int32_t>(std::atoi(idxStr.c_str()));
                if (idx > 0) ids.push_back(static_cast<uint32_t>(idx - 1));
                else if (idx < 0) ids.push_back(static_cast<uint32_t>(verts.size() + idx));
                else { bad = true; break; }
                if (ts.eof()) break;
            }
            if (!bad && ids.size() >= 3 && !verts.empty()) {
                for (size_t i = 1; i + 1 < ids.size(); ++i) {
                    if (ids[0] >= verts.size() || ids[i] >= verts.size() ||
                        ids[i + 1] >= verts.size()) continue;
                    addTri(out, verts[ids[0]], verts[ids[i]], verts[ids[i + 1]],
                           o.maxTriangles);
                }
            }
            continue;
        }
        ts.eol();
    }
    if (out.indices.empty()) return Rc::ParseError;
    recomputeBounds(out);
    computeVertexNormals(out);
    return Rc::Ok;
}

// ------------------------------------------------------------------ PLY ------

Rc parsePly(const std::wstring& path, Mesh& out, const ParseOpts& o) {
    meshClear(out);
    TextStream ts = TextStream::readFile(path, o.maxBytes);
    if (ts.text.empty()) return Rc::IoError;
    const std::string& s = ts.text;
    size_t p = 0;
    if (s.size() < 4 || s.compare(0, 3, "ply") != 0) return Rc::ParseError;
    // Locate end_header line
    size_t headerEnd = s.find("end_header");
    if (headerEnd == std::string::npos) return Rc::ParseError;
    size_t headerLineEnd = s.find('\n', headerEnd);
    if (headerLineEnd == std::string::npos) headerLineEnd = s.size();
    else ++headerLineEnd;
    std::string header = s.substr(0, headerLineEnd);
    bool ascii = header.find("format ascii") != std::string::npos;
    bool bigEndian = header.find("format binary_big_endian") != std::string::npos;
    (void)bigEndian;
    int32_t vertexCount = 0, faceCount = 0;
    size_t faceListPropPos = std::string::npos;
    // Parse element/property lines
    {
        std::string::const_iterator it = header.begin();
        std::vector<std::string> lines;
        {
            std::string line;
            for (char c : header) {
                if (c == '\n' || c == '\r') { lines.push_back(line); line.clear(); }
                else line.push_back(c);
            }
            lines.push_back(line);
        }
        for (const auto& line : lines) {
            std::string t = line;
            // tokenize
            std::vector<std::string> tok;
            {
                std::string cur;
                for (char c : t) {
                    if (c == ' ' || c == '\t') { if (!cur.empty()) tok.push_back(cur); cur.clear(); }
                    else cur.push_back(c);
                }
                if (!cur.empty()) tok.push_back(cur);
            }
            if (tok.empty()) continue;
            if (tok[0] == "element" && tok.size() >= 3) {
                if (tok[1] == "vertex") vertexCount = static_cast<int32_t>(std::atoi(tok[2].c_str()));
                else if (tok[1] == "face") faceCount = static_cast<int32_t>(std::atoi(tok[2].c_str()));
            } else if (tok[0] == "property" && tok.size() >= 4 &&
                       tok[1] == "list") {
                faceListPropPos = tok[2].empty() ? std::string::npos : 0;
            }
        }
        (void)it;
    }
    if (vertexCount <= 0) return Rc::ParseError;
    std::vector<Vec3> verts;
    verts.reserve(vertexCount);
    size_t pos = headerLineEnd;
    if (ascii) {
        TextStream ls{s};
        ls.pos = pos;
        for (int32_t i = 0; i < vertexCount && !ls.eof(); ++i) {
            if (o.stopNow()) return Rc::Timeout;
            float x = ls.number(), y = ls.number(), z = ls.number();
            verts.push_back({x, y, z});
        }
    } else {
        for (int32_t i = 0; i < vertexCount && pos + 12 <= s.size(); ++i) {
            if (o.stopNow()) return Rc::Timeout;
            float x, y, z;
            std::memcpy(&x, s.data() + pos, 4);
            std::memcpy(&y, s.data() + pos + 4, 4);
            std::memcpy(&z, s.data() + pos + 8, 4);
            verts.push_back({x, y, z});
            pos += 12;
        }
    }
    if (static_cast<int32_t>(verts.size()) != vertexCount) return Rc::ParseError;
    if (faceCount > 0) {
        // Face list type — default to uint8 count + uint32 indices
        uint8_t countSz = 1, idxSz = 4;
        // Find "property list <countType> <idxType>"
        size_t propPos = header.find("property list");
        if (propPos != std::string::npos) {
            size_t lineEnd = header.find('\n', propPos);
            if (lineEnd == std::string::npos) lineEnd = header.size();
            std::string prop = header.substr(propPos, lineEnd - propPos);
            if (prop.find("uint8") != std::string::npos && prop.rfind("uint8", prop.find(" ")) != std::string::npos) {
                // First "uint8" is countType
                size_t first = prop.find("uint8");
                size_t spaceAfter = prop.find(' ', first);
                if (spaceAfter != std::string::npos) {
                    std::string idxType = prop.substr(spaceAfter + 1);
                    // Trim trailing whitespace
                    while (!idxType.empty() && (idxType.back() == '\r' || idxType.back() == '\n' ||
                                                 idxType.back() == ' '))
                        idxType.pop_back();
                    if (idxType == "uint8") idxSz = 1;
                    else if (idxType == "uint16") idxSz = 2;
                    else if (idxType == "uint32") idxSz = 4;
                    else if (idxType == "int8") idxSz = 1;
                    else if (idxType == "int16") idxSz = 2;
                    else if (idxType == "int32") idxSz = 4;
                }
            } else if (prop.find("uint16") != std::string::npos && prop.rfind("uint16", prop.find(" ")) != std::string::npos) {
                countSz = 2;
            } else if (prop.find("uint32") != std::string::npos && prop.rfind("uint32", prop.find(" ")) != std::string::npos) {
                countSz = 4;
            }
        }
        for (int32_t i = 0; i < faceCount && !ascii && pos + countSz <= s.size(); ++i) {
            if (o.stopNow()) return Rc::Timeout;
            uint32_t n = 0;
            if (countSz == 1) n = static_cast<uint8_t>(s.data()[pos]);
            else if (countSz == 2) {
                uint16_t v; std::memcpy(&v, s.data() + pos, 2); n = v;
            } else if (countSz == 4) {
                uint32_t v; std::memcpy(&v, s.data() + pos, 4); n = v;
            }
            pos += countSz;
            std::vector<uint32_t> ids;
            ids.reserve(n);
            for (uint32_t j = 0; j < n && pos + idxSz <= s.size(); ++j) {
                uint32_t id = 0;
                if (idxSz == 1) id = static_cast<uint8_t>(s.data()[pos]);
                else if (idxSz == 2) {
                    uint16_t v; std::memcpy(&v, s.data() + pos, 2); id = v;
                } else if (idxSz == 4) {
                    uint32_t v; std::memcpy(&v, s.data() + pos, 4); id = v;
                }
                pos += idxSz;
                if (id < verts.size()) ids.push_back(id);
            }
            for (size_t k = 1; k + 1 < ids.size(); ++k) {
                addTri(out, verts[ids[0]], verts[ids[k]], verts[ids[k + 1]], o.maxTriangles);
                if (out.indices.size() / 3 >= o.maxTriangles) return Rc::Ok;
            }
        }
        if (ascii && faceCount > 0) {
            TextStream ls{s};
            ls.pos = pos;
            for (int32_t i = 0; i < faceCount && !ls.eof(); ++i) {
                if (o.stopNow()) return Rc::Timeout;
                int32_t n = static_cast<int32_t>(ls.number());
                std::vector<uint32_t> ids;
                for (int32_t j = 0; j < n && !ls.eof(); ++j) {
                    int32_t id = static_cast<int32_t>(ls.number());
                    if (id >= 0 && id < static_cast<int32_t>(verts.size())) ids.push_back(static_cast<uint32_t>(id));
                }
                for (size_t k = 1; k + 1 < ids.size(); ++k) {
                    addTri(out, verts[ids[0]], verts[ids[k]], verts[ids[k + 1]], o.maxTriangles);
                }
                if (out.indices.size() / 3 >= o.maxTriangles) return Rc::Ok;
            }
        }
    }
    if (out.indices.empty()) return Rc::ParseError;
    recomputeBounds(out);
    computeVertexNormals(out);
    return Rc::Ok;
}

// ------------------------------------------------------------------ OFF ------

Rc parseOff(const std::wstring& path, Mesh& out, const ParseOpts& o) {
    meshClear(out);
    TextStream ts = TextStream::readFile(path, o.maxBytes);
    if (ts.text.empty()) return Rc::IoError;
    std::string header = ts.word();
    if (header != "OFF" && header != "COFF") return Rc::ParseError;
    int32_t nVerts = static_cast<int32_t>(ts.number());
    int32_t nFaces = static_cast<int32_t>(ts.number());
    int32_t nEdges = 0;
    // Optional: only 3D printing OFFs always have 3 numbers
    ts.skipWs();
    if (ts.pos < ts.text.size() && std::isdigit(static_cast<unsigned char>(ts.text[ts.pos]))) {
        nEdges = static_cast<int32_t>(ts.number());
    }
    ts.eol();
    (void)nEdges;
    if (nVerts <= 0) return Rc::ParseError;
    std::vector<Vec3> verts;
    verts.reserve(nVerts);
    for (int32_t i = 0; i < nVerts && !ts.eof(); ++i) {
        if (o.stopNow()) return Rc::Timeout;
        float x = ts.number(), y = ts.number(), z = ts.number();
        verts.push_back({x, y, z});
    }
    for (int32_t i = 0; i < nFaces && !ts.eof(); ++i) {
        if (o.stopNow()) return Rc::Timeout;
        int32_t n = static_cast<int32_t>(ts.number());
        std::vector<uint32_t> ids;
        for (int32_t j = 0; j < n && !ts.eof(); ++j) {
            int32_t idx = static_cast<int32_t>(ts.number());
            if (idx < 0) idx += nVerts;
            if (idx >= 0 && idx < nVerts) ids.push_back(static_cast<uint32_t>(idx));
        }
        for (size_t k = 1; k + 1 < ids.size(); ++k) {
            addTri(out, verts[ids[0]], verts[ids[k]], verts[ids[k + 1]], o.maxTriangles);
            if (out.indices.size() / 3 >= o.maxTriangles) return Rc::Ok;
        }
    }
    if (out.indices.empty()) return Rc::ParseError;
    recomputeBounds(out);
    computeVertexNormals(out);
    return Rc::Ok;
}

// ------------------------------------------------------------------ 3DS ------

namespace {

void parse3dsMesh(size_t dataStart, size_t dataEnd,
                  const std::vector<Byte>& buf, Mesh& out, size_t triCap) {
    std::vector<Vec3> verts;
    size_t p = dataStart;
    while (p + 6 <= dataEnd) {
        uint16_t sub = static_cast<uint16_t>(buf[p]) | (static_cast<uint16_t>(buf[p + 1]) << 8);
        uint32_t ss = static_cast<uint32_t>(buf[p + 2])
                    | (static_cast<uint32_t>(buf[p + 3]) << 8)
                    | (static_cast<uint32_t>(buf[p + 4]) << 16)
                    | (static_cast<uint32_t>(buf[p + 5]) << 24);
        if (ss < 6 || p + ss > dataEnd) break;
        size_t ds = p + 6, de = p + ss;
        if (sub == 0x4004 && (de - ds) >= 12) {
            size_t n = (de - ds) / 12;
            for (size_t i = 0; i < n; ++i) {
                float x, y, z;
                std::memcpy(&x, buf.data() + ds + i * 12, 4);
                std::memcpy(&y, buf.data() + ds + i * 12 + 4, 4);
                std::memcpy(&z, buf.data() + ds + i * 12 + 8, 4);
                verts.push_back({x, y, z});
            }
        } else if (sub == 0x4005 && (de - ds) >= 8) {
            size_t n = (de - ds) / 8;
            for (size_t i = 0; i < n && verts.size() > 2; ++i) {
                uint16_t v1 = buf[ds + i * 8] | (static_cast<uint16_t>(buf[ds + i * 8 + 1]) << 8);
                uint16_t v2 = buf[ds + i * 8 + 2] | (static_cast<uint16_t>(buf[ds + i * 8 + 3]) << 8);
                uint16_t v3 = buf[ds + i * 8 + 4] | (static_cast<uint16_t>(buf[ds + i * 8 + 5]) << 8);
                v1--; v2--; v3--;
                if (v1 < verts.size() && v2 < verts.size() && v3 < verts.size()) {
                    addTri(out, verts[v1], verts[v2], verts[v3], triCap);
                }
                if (out.indices.size() / 3 >= triCap) return;
            }
        }
        p += ss;
    }
}

}  // namespace

Rc parse3ds(const std::wstring& path, Mesh& out, const ParseOpts& o) {
    meshClear(out);
    HANDLE h = CreateFileW(path.c_str(), GENERIC_READ, FILE_SHARE_READ, nullptr,
                           OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (h == INVALID_HANDLE_VALUE) return Rc::IoError;
    LARGE_INTEGER sz;
    if (!GetFileSizeEx(h, &sz) || sz.QuadPart <= 0) { CloseHandle(h); return Rc::EmptyFile; }
    DWORD need = static_cast<DWORD>(std::min<size_t>(static_cast<size_t>(sz.QuadPart), o.maxBytes));
    std::vector<Byte> buf(need);
    DWORD got = 0;
    ReadFile(h, buf.data(), need, &got, nullptr);
    CloseHandle(h);
    buf.resize(got);
    size_t pos = 0;
    while (pos + 6 <= buf.size()) {
        uint16_t id = static_cast<uint16_t>(buf[pos]) | (static_cast<uint16_t>(buf[pos + 1]) << 8);
        uint32_t szk = static_cast<uint32_t>(buf[pos + 2])
                     | (static_cast<uint32_t>(buf[pos + 3]) << 8)
                     | (static_cast<uint32_t>(buf[pos + 4]) << 16)
                     | (static_cast<uint32_t>(buf[pos + 5]) << 24);
        if (szk < 6 || pos + szk > buf.size()) break;
        size_t dataStart = pos + 6, dataEnd = pos + szk;
        if (id == 0x4000) {
            parse3dsMesh(dataStart, dataEnd, buf, out, o.maxTriangles);
        } else if (id == 0x3D3D) {
            // Root — descend
            size_t p = dataStart;
            while (p + 6 <= dataEnd) {
                uint16_t sub = static_cast<uint16_t>(buf[p]) | (static_cast<uint16_t>(buf[p + 1]) << 8);
                uint32_t ss = static_cast<uint32_t>(buf[p + 2])
                            | (static_cast<uint32_t>(buf[p + 3]) << 8)
                            | (static_cast<uint32_t>(buf[p + 4]) << 16)
                            | (static_cast<uint32_t>(buf[p + 5]) << 24);
                if (ss < 6 || p + ss > dataEnd) break;
                if (sub == 0x4000) parse3dsMesh(p + 6, p + ss, buf, out, o.maxTriangles);
                p += ss;
            }
        }
        pos += szk;
    }
    if (out.indices.empty()) return Rc::ParseError;
    recomputeBounds(out);
    computeVertexNormals(out);
    return Rc::Ok;
}

// ------------------------------------------------------------------ AMF ------

Rc parseAmfMesh(const std::wstring& path, Mesh& out, const ParseOpts& o) {
    meshClear(out);
    TextStream ts = TextStream::readFile(path, o.maxBytes);
    if (ts.text.empty()) return Rc::IoError;
    const std::string& s = ts.text;
    if (s.find("<AMF") == std::string::npos && s.find("<amf") == std::string::npos) {
        return Rc::ParseError;
    }
    size_t meshStart = 0;
    while ((meshStart = s.find("<mesh", meshStart)) != std::string::npos) {
        if (o.stopNow()) return Rc::Timeout;
        size_t blockEnd = s.find("</vertices>", meshStart);
        if (blockEnd == std::string::npos) break;
        size_t vertsBase = out.positions.size() / 3;
        size_t p = meshStart;
        while ((p = s.find("<vertex", p)) != std::string::npos && p < blockEnd) {
            size_t lineEnd = s.find(">", p);
            if (lineEnd == std::string::npos) break;
            std::string tag = s.substr(p, lineEnd - p);
            std::string xs, ys, zs;
            xmlAttrValue(tag, 0, "x", xs);
            xmlAttrValue(tag, 0, "y", ys);
            xmlAttrValue(tag, 0, "z", zs);
            out.positions.push_back(xs.empty() ? 0.f : static_cast<float>(std::atof(xs.c_str())));
            out.positions.push_back(ys.empty() ? 0.f : static_cast<float>(std::atof(ys.c_str())));
            out.positions.push_back(zs.empty() ? 0.f : static_cast<float>(std::atof(zs.c_str())));
            p = lineEnd;
        }
        p = blockEnd;
        size_t meshEnd = s.find("</mesh>", p);
        if (meshEnd == std::string::npos) meshEnd = s.size();
        const size_t nVerts = out.positions.size() / 3;

        // AMF index convention detection. The AMF spec is 1-based, but Cura,
        // PrusaSlicer and Bambu Studio all emit 0-based indices. Instead of
        // hard-coding one convention, probe the minimum index in this mesh
        // block: min == 0 => 0-based (subtract 0); min >= 1 => 1-based
        // (subtract 1). Files that use neither convention (min < 0) will
        // emit nothing because the range check below rejects them.
        int32_t minIdx = INT32_MAX;
        {
            size_t q = blockEnd;
            while ((q = s.find("<triangle", q)) != std::string::npos && q < meshEnd) {
                // Skip the <triangles> parent container: "<triangle" is a
                // prefix of "<triangles>", so a naive find() picks it up and
                // would treat it as an attribute-less triangle.
                if (q + 9 < s.size() && s[q + 9] == 's') { q += 10; continue; }
                if (o.stopNow()) return Rc::Timeout;
                size_t le = s.find(">", q);
                if (le == std::string::npos) break;
                std::string tg = s.substr(q, le - q);
                std::string a1, a2, a3;
                xmlAttrValue(tg, 0, "v1", a1);
                xmlAttrValue(tg, 0, "v2", a2);
                xmlAttrValue(tg, 0, "v3", a3);
                int32_t i1 = static_cast<int32_t>(std::atoi(a1.c_str()));
                int32_t i2 = static_cast<int32_t>(std::atoi(a2.c_str()));
                int32_t i3 = static_cast<int32_t>(std::atoi(a3.c_str()));
                if (i1 < minIdx) minIdx = i1;
                if (i2 < minIdx) minIdx = i2;
                if (i3 < minIdx) minIdx = i3;
                q = le;
            }
        }
        const int32_t base = (minIdx >= 1) ? 1 : 0;

        p = blockEnd;
        while ((p = s.find("<triangle", p)) != std::string::npos && p < meshEnd) {
            if (p + 9 < s.size() && s[p + 9] == 's') { p += 10; continue; }
            if (o.stopNow()) return Rc::Timeout;
            size_t lineEnd = s.find(">", p);
            if (lineEnd == std::string::npos) break;
            std::string tag = s.substr(p, lineEnd - p);
            std::string a1, a2, a3;
            xmlAttrValue(tag, 0, "v1", a1);
            xmlAttrValue(tag, 0, "v2", a2);
            xmlAttrValue(tag, 0, "v3", a3);
            int32_t i1 = static_cast<int32_t>(std::atoi(a1.c_str()));
            int32_t i2 = static_cast<int32_t>(std::atoi(a2.c_str()));
            int32_t i3 = static_cast<int32_t>(std::atoi(a3.c_str()));
            const int32_t n1 = i1 - base;
            const int32_t n2 = i2 - base;
            const int32_t n3 = i3 - base;
            if (n1 >= 0 && n2 >= 0 && n3 >= 0 &&
                static_cast<size_t>(n1) < nVerts &&
                static_cast<size_t>(n2) < nVerts &&
                static_cast<size_t>(n3) < nVerts) {
                out.indices.push_back(static_cast<uint32_t>(n1));
                out.indices.push_back(static_cast<uint32_t>(n2));
                out.indices.push_back(static_cast<uint32_t>(n3));
            }
            if (out.indices.size() / 3 >= o.maxTriangles) break;
            p = lineEnd;
        }
        (void)vertsBase;
        meshStart += 5;
    }
    if (out.indices.empty()) return Rc::ParseError;
    recomputeBounds(out);
    computeVertexNormals(out);
    return Rc::Ok;
}

// ------------------------------------------------------------------ FBX ------

namespace {

// Zlib-wrapped DEFLATE inflate via ntdll. `out` must be pre-sized to the
// expected uncompressed length. Returns true on success.
struct FbxCompressedHeader {
    PUCHAR CompressedData;
    ULONG CompressedDataSize;
    PUCHAR UncompressedBuffer;
    ULONG UncompressedBufferSize;
};
typedef LONG (NTAPI* RtlDecompressFn)(SHORT, PUCHAR, ULONG, FbxCompressedHeader*, ULONG, PULONG);
static bool fbxInflate(const Byte* src, size_t srcLen, std::vector<Byte>& out) {
    if (srcLen < 2 || src[0] != 0x78) return false; // not a zlib stream
    static RtlDecompressFn fn = reinterpret_cast<RtlDecompressFn>(
        GetProcAddress(GetModuleHandleW(L"ntdll.dll"), "RtlDecompressBufferEx2"));
    if (!fn) return false;
    FbxCompressedHeader ch;
    ch.CompressedData = const_cast<PUCHAR>(src + 2); // skip 2-byte zlib header
    ch.CompressedDataSize = static_cast<ULONG>(srcLen - 2);
    ch.UncompressedBuffer = nullptr;
    ch.UncompressedBufferSize = 0;
    ULONG finalSize = static_cast<ULONG>(out.size());
    LONG st = fn(8 /*DEFLATE*/, out.data(), finalSize, &ch, ch.CompressedDataSize, &finalSize);
    return st == 0 && finalSize > 0;
}

// Read a null-terminated style binary record from FBX. Returns true if a valid
// (non-empty) record header was parsed. The header layout (little-endian):
//   [4] endOffset  [4] numProps  [4] propListLen  [1] nameLen  [nameLen] name
static uint32_t rd32(const Byte* p) {
    return static_cast<uint32_t>(p[0]) |
           (static_cast<uint32_t>(p[1]) << 8) |
           (static_cast<uint32_t>(p[2]) << 16) |
           (static_cast<uint32_t>(p[3]) << 24);
}

struct FbxRecordHead {
    uint32_t endOffset = 0;
    uint32_t numProps = 0;
    uint32_t propListLen = 0;
    uint32_t nameLen = 0;
    std::string name;
    size_t headerSize = 0; // bytes consumed by this header
};
static bool fbxReadRecordHead(const Byte* data, size_t size, size_t pos, FbxRecordHead& out) {
    if (pos + 13 > size) return out.name.empty(), false;
    uint32_t endOffset = rd32(data + pos);
    uint32_t numProps = rd32(data + pos + 4);
    uint32_t propListLen = rd32(data + pos + 8);
    uint8_t nameLen = data[pos + 12];
    if (endOffset == 0 && numProps == 0 && propListLen == 0 && nameLen == 0) {
        // end-of-file null record
        out.name.clear(); out.endOffset = 0;
        return false;
    }
    if (pos + 13 + nameLen > size) return out.name.clear(), false;
    out.endOffset = endOffset;
    out.numProps = numProps;
    out.propListLen = propListLen;
    out.nameLen = nameLen;
    out.name.assign(reinterpret_cast<const char*>(data + pos + 13), nameLen);
    out.headerSize = 13 + nameLen;
    return true;
}

// A single FBX property slot (one of the numProps in a record). Properties can
// be a primitive scalar, an array, or a nested sub-record. We only care about
// typed arrays (f=Float[], d=Double[], i=Int32[], etc.) carrying vertex/index data.
struct FbxProperty {
    char type = 0;
    uint32_t arrayLen = 0;
    uint32_t encoding = 0;
    uint32_t compLen = 0;
    size_t dataOffset = 0; // offset into the file data where the raw/compressed payload begins
    size_t slotBytes = 0;  // bytes consumed by this property in the stream
};
static bool fbxReadProperty(const Byte* data, size_t size, size_t pos, FbxProperty& out) {
    if (pos >= size) return false;
    char type = static_cast<char>(data[pos]);
    out.type = type;
    out.slotBytes = 1;
    if (type == 'f' || type == 'd' || type == 'l' || type == 'i' || type == 'b') {
        if (pos + 13 > size) return false;
        out.arrayLen = rd32(data + pos + 1);
        out.encoding = rd32(data + pos + 5);
        out.compLen = rd32(data + pos + 9);
        out.dataOffset = pos + 13;
        out.slotBytes = 13 + out.compLen;
    } else {
        // scalar: Y(2) C(1) I(4) F(4) D(8) L(8) R/S(var+len) -- skip safely
        if (type == 'Y') out.slotBytes = 3;
        else if (type == 'C') out.slotBytes = 2;
        else if (type == 'I' || type == 'F') out.slotBytes = 5;
        else if (type == 'L' || type == 'D') out.slotBytes = 9;
        else if (type == 'R' || type == 'S') {
            if (pos + 5 > size) return false;
            uint32_t sl = rd32(data + pos + 1);
            out.slotBytes = 5 + sl;
        } else {
            return false;
        }
    }
    return pos + out.slotBytes <= size;
}

// Extract typed array bytes for one property. Handles zlib-compression if needed.
static bool fbxReadArray(const Byte* data, size_t size, const FbxProperty& p, std::vector<Byte>& out) {
    out.clear();
    if (p.encoding == 0) {
        if (p.dataOffset + p.compLen > size) return false;
        out.assign(data + p.dataOffset, data + p.dataOffset + p.compLen);
        return true;
    }
    if (p.encoding == 1) {
        if (p.dataOffset + p.compLen > size) return false;
        size_t elemCount = p.arrayLen;
        size_t elemSize = (p.type == 'd' || p.type == 'l') ? 8 : 4;
        out.resize(elemCount * elemSize);
        if (!fbxInflate(data + p.dataOffset, p.compLen, out)) return false;
        return true;
    }
    return false; // unsupported encoding
}

// Walk all top-level records, find "Geometry" nodes, within them pull the
// "Vertices" (double[] or float[]) and "PolygonVertexIndex" (int32[]) arrays.
// FBX polygons use a negative sentinel to mark the last vertex of each polygon:
//   actualIndex = (v < 0) ? -(v + 1) : v
// Triangulate each polygon as a fan from its first non-sentinel vertex.
static void parseFbxBinaryMesh(const Byte* data, size_t size, Mesh& out, size_t triCap) {
    size_t pos = 0;
    // FBX binary header: "Kaydara FBX Binary  \0" + 0x1A + 0x00 + version(u32)
    if (size < 27) return;
    pos = 27; // skip fixed header

    std::vector<float> positions;
    std::vector<int32_t> rawIndices;

    while (pos < size) {
        FbxRecordHead rec;
        if (!fbxReadRecordHead(data, size, pos, rec)) break;
        size_t recEnd = rec.endOffset ? rec.endOffset : size;
        size_t propsStart = pos + rec.headerSize;
        size_t afterProps = propsStart + rec.propListLen;
        bool isGeometry = (rec.name == "Geometry" || rec.name == "Model" || rec.name == "Mesh");

        if (isGeometry) {
            // scan this record's direct properties for Vertices / PolygonVertexIndex
            size_t pp = propsStart;
            for (uint32_t i = 0; i < rec.numProps && pp < afterProps; ++i) {
                FbxProperty p;
                if (!fbxReadProperty(data, size, pp, p)) break;
                if (p.type == 'd' || p.type == 'f') {
                    if (p.arrayLen >= 3 && p.arrayLen % 3 == 0) {
                        std::vector<Byte> arr;
                        if (fbxReadArray(data, size, p, arr)) {
                            size_t elemSize = (p.type == 'd') ? 8 : 4;
                            size_t n = arr.size() / elemSize;
                            if (n % 3 == 0 && positions.empty()) {
                                positions.resize(n);
                                for (size_t k = 0; k < n; ++k) {
                                    if (p.type == 'd') {
                                        double v; std::memcpy(&v, arr.data() + k * 8, 8);
                                        positions[k] = static_cast<float>(v);
                                    } else {
                                        std::memcpy(&positions[k], arr.data() + k * 4, 4);
                                    }
                                }
                            }
                        }
                    }
                } else if (p.type == 'i') {
                    if (p.arrayLen >= 3 && rawIndices.empty()) {
                        std::vector<Byte> arr;
                        if (fbxReadArray(data, size, p, arr)) {
                            size_t n = arr.size() / 4;
                            rawIndices.resize(n);
                            for (size_t k = 0; k < n; ++k) {
                                int32_t v; std::memcpy(&v, arr.data() + k * 4, 4);
                                rawIndices[k] = v;
                            }
                        }
                    }
                }
                pp += p.slotBytes;
            }
            if (!positions.empty() && !rawIndices.empty()) {
                break; // got what we need from this geometry
            }
        }
        // also recurse into sub-records to find nested Geometry/Mesh
        size_t sub = afterProps;
        while (sub < recEnd) {
            FbxRecordHead child;
            if (!fbxReadRecordHead(data, size, sub, child)) break;
            size_t childEnd = child.endOffset ? child.endOffset : recEnd;
            if (child.name == "Geometry" || child.name == "Mesh" || child.name == "Model") {
                size_t cpropsStart = sub + child.headerSize;
                size_t cpropsEnd = cpropsStart + child.propListLen;
                size_t cpp = cpropsStart;
                for (uint32_t i = 0; i < child.numProps && cpp < cpropsEnd; ++i) {
                    FbxProperty p;
                    if (!fbxReadProperty(data, size, cpp, p)) break;
                    if (p.type == 'd' || p.type == 'f') {
                        if (p.arrayLen >= 3 && p.arrayLen % 3 == 0 && positions.empty()) {
                            std::vector<Byte> arr;
                            if (fbxReadArray(data, size, p, arr)) {
                                size_t elemSize = (p.type == 'd') ? 8 : 4;
                                size_t n = arr.size() / elemSize;
                                if (n % 3 == 0) {
                                    positions.resize(n);
                                    for (size_t k = 0; k < n; ++k) {
                                        if (p.type == 'd') {
                                            double v; std::memcpy(&v, arr.data() + k * 8, 8);
                                            positions[k] = static_cast<float>(v);
                                        } else {
                                            std::memcpy(&positions[k], arr.data() + k * 4, 4);
                                        }
                                    }
                                }
                            }
                        }
                    } else if (p.type == 'i') {
                        if (p.arrayLen >= 3 && rawIndices.empty()) {
                            std::vector<Byte> arr;
                            if (fbxReadArray(data, size, p, arr)) {
                                size_t n = arr.size() / 4;
                                rawIndices.resize(n);
                                for (size_t k = 0; k < n; ++k) {
                                    int32_t v; std::memcpy(&v, arr.data() + k * 4, 4);
                                    rawIndices[k] = v;
                                }
                            }
                        }
                    }
                    cpp += p.slotBytes;
                }
            }
            sub = childEnd;
        }
        pos = recEnd;
    }

    if (positions.size() < 9 || rawIndices.size() < 3) return; // not enough data
    size_t vertCount = positions.size() / 3;
    // triangulate polygon strips using negative sentinel
    size_t p = 0;
    while (p < rawIndices.size()) {
        // gather one polygon run
        std::vector<size_t> ring;
        while (p < rawIndices.size()) {
            int32_t v = rawIndices[p++];
            size_t idx = (v < 0) ? static_cast<size_t>(-(v + 1)) : static_cast<size_t>(v);
            ring.push_back(idx);
            if (v < 0) break;
        }
        if (ring.size() < 3) continue;
        for (size_t k = 1; k + 1 < ring.size() && out.indices.size() / 3 < triCap; ++k) {
            size_t a = ring[0], b = ring[k], c = ring[k + 1];
            if (a < vertCount && b < vertCount && c < vertCount) {
                size_t base = out.positions.size() / 3;
                out.positions.push_back(positions[a * 3]); out.positions.push_back(positions[a * 3 + 1]); out.positions.push_back(positions[a * 3 + 2]);
                out.positions.push_back(positions[b * 3]); out.positions.push_back(positions[b * 3 + 1]); out.positions.push_back(positions[b * 3 + 2]);
                out.positions.push_back(positions[c * 3]); out.positions.push_back(positions[c * 3 + 1]); out.positions.push_back(positions[c * 3 + 2]);
                out.indices.push_back(static_cast<uint32_t>(base));
                out.indices.push_back(static_cast<uint32_t>(base + 1));
                out.indices.push_back(static_cast<uint32_t>(base + 2));
            }
        }
    }
}

}  // namespace

// ASCII FBX parser: the text format uses XML-like nested sections with
//   Vertices: *N { a: x0,y0,z0,x1,y1,z1,... }
//   PolygonVertexIndex: *N { a: i0,i1,i2,... }
// Same negative-sentinel polygon convention as the binary format.
Rc parseFbxAscii(const std::wstring& path, Mesh& out, const ParseOpts& o) {
    meshClear(out);
    TextStream ts = TextStream::readFile(path, o.maxBytes);
    if (ts.text.empty()) return Rc::IoError;
    const std::string& s = ts.text;

    size_t pos = 0;
    auto findAfter = [&](const char* key, size_t from) -> size_t {
        size_t p = from;
        while ((p = s.find(key, p)) != std::string::npos) {
            // ensure it's a token boundary (preceded by whitespace/line)
            if (p == 0 || s[p - 1] == '\n' || s[p - 1] == ' ' || s[p - 1] == '\r') return p;
            ++p;
        }
        return std::string::npos;
    };

    // --- extract vertices array ---
    std::vector<float> positions;
    size_t vp = findAfter("Vertices:", pos);
    if (vp != std::string::npos) {
        size_t brace = s.find('{', vp);
        size_t close = s.find('}', brace == std::string::npos ? vp : brace);
        if (brace != std::string::npos && close != std::string::npos && close > brace) {
            std::string inside(s, brace + 1, close - brace - 1);
            // inside may have "a:" prefix then comma/space separated floats
            size_t a = inside.find('a');
            if (a != std::string::npos) {
                size_t colon = inside.find(':', a);
                std::string nums = (colon != std::string::npos) ? inside.substr(colon + 1) : inside;
                std::replace(nums.begin(), nums.end(), ',', ' ');
                std::replace(nums.begin(), nums.end(), '\n', ' ');
                std::replace(nums.begin(), nums.end(), '\r', ' ');
                std::istringstream iss(nums);
                double v;
                while (iss >> v) positions.push_back(static_cast<float>(v));
            }
        }
    }

    // --- extract polygon indices ---
    std::vector<int32_t> rawIndices;
    size_t ip = findAfter("PolygonVertexIndex:", pos);
    if (ip != std::string::npos) {
        size_t brace = s.find('{', ip);
        size_t close = s.find('}', brace == std::string::npos ? ip : brace);
        if (brace != std::string::npos && close != std::string::npos && close > brace) {
            std::string inside(s, brace + 1, close - brace - 1);
            size_t a = inside.find('a');
            if (a != std::string::npos) {
                size_t colon = inside.find(':', a);
                std::string nums = (colon != std::string::npos) ? inside.substr(colon + 1) : inside;
                std::replace(nums.begin(), nums.end(), ',', ' ');
                std::replace(nums.begin(), nums.end(), '\n', ' ');
                std::replace(nums.begin(), nums.end(), '\r', ' ');
                std::istringstream iss(nums);
                int32_t v;
                while (iss >> v) rawIndices.push_back(v);
            }
        }
    }

    if (positions.size() < 9 || rawIndices.size() < 3) return Rc::ParseError;
    size_t vertCount = positions.size() / 3;

    size_t p = 0;
    while (p < rawIndices.size()) {
        if (o.stopNow()) return Rc::Timeout;
        std::vector<size_t> ring;
        while (p < rawIndices.size()) {
            int32_t v = rawIndices[p++];
            size_t idx = (v < 0) ? static_cast<size_t>(-(v + 1)) : static_cast<size_t>(v);
            ring.push_back(idx);
            if (v < 0) break;
        }
        if (ring.size() < 3) continue;
        for (size_t k = 1; k + 1 < ring.size() && out.indices.size() / 3 < o.maxTriangles; ++k) {
            size_t a = ring[0], b = ring[k], c = ring[k + 1];
            if (a >= vertCount || b >= vertCount || c >= vertCount) continue;
            size_t base = out.positions.size() / 3;
            out.positions.push_back(positions[a * 3]); out.positions.push_back(positions[a * 3 + 1]); out.positions.push_back(positions[a * 3 + 2]);
            out.positions.push_back(positions[b * 3]); out.positions.push_back(positions[b * 3 + 1]); out.positions.push_back(positions[b * 3 + 2]);
            out.positions.push_back(positions[c * 3]); out.positions.push_back(positions[c * 3 + 1]); out.positions.push_back(positions[c * 3 + 2]);
            out.indices.push_back(static_cast<uint32_t>(base));
            out.indices.push_back(static_cast<uint32_t>(base + 1));
            out.indices.push_back(static_cast<uint32_t>(base + 2));
        }
    }
    if (out.indices.empty()) return Rc::ParseError;
    recomputeBounds(out);
    computeVertexNormals(out);
    return Rc::Ok;
}

Rc parseFbxMesh(const std::wstring& path, Mesh& out, const ParseOpts& o) {
    meshClear(out);
    HANDLE h = CreateFileW(path.c_str(), GENERIC_READ, FILE_SHARE_READ, nullptr,
                           OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (h == INVALID_HANDLE_VALUE) return Rc::IoError;
    LARGE_INTEGER sz;
    if (!GetFileSizeEx(h, &sz) || sz.QuadPart <= 0) { CloseHandle(h); return Rc::EmptyFile; }
    if (o.maxBytes > 0 && (uint64_t)sz.QuadPart > (uint64_t)o.maxBytes) {
        CloseHandle(h);
        return Rc::TooLarge;
    }
    std::vector<Byte> data(static_cast<size_t>(sz.QuadPart));
    DWORD got = 0;
    if (!ReadFile(h, data.data(), static_cast<DWORD>(data.size()), &got, nullptr)) { CloseHandle(h); return Rc::IoError; }
    CloseHandle(h);
    data.resize(got);
    if (data.size() < 2) return Rc::ParseError;

    // Detect binary FBX by its magic: "Kaydara FBX Binary  \0"
    const unsigned char binMagic[] = {'K','a','y','d','a','r','a',' ','F','B','X',' ','B','i','n','a','r','y',' ',' ','\0'};
    if (data.size() >= 21 && std::memcmp(data.data(), binMagic, 21) == 0) {
        Mesh m;
        parseFbxBinaryMesh(data.data(), data.size(), m, o.maxTriangles);
        if (m.indices.empty()) return Rc::ParseError;
        out = std::move(m);
        recomputeBounds(out);
        computeVertexNormals(out);
        return Rc::Ok;
    }
    // else treat as ASCII FBX
    return parseFbxAscii(path, out, o);
}

// ------------------------------------------------------------------ 3MF ------

namespace {

// A 3MF transform: 12 whitespace separated numbers -- the 3x3 rotation/scale
// in row-major order first, then the translation vector (spec 2.1.1), e.g.
// r11 r12 r13 r21 r22 r23 r31 r32 r33 tx ty tz. parseMat3x4 rearranges that
// into our internal row-major 3x4 (x' = m0*x + m1*y + m2*z + m3).
struct Mat3x4 {
    float m[12] = {1, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1, 0};
};

bool isIdentityXf(const Mat3x4& t) {
    static const float k[12] = {1, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1, 0};
    for (int i = 0; i < 12; ++i) {
        if (std::fabs(t.m[i] - k[i]) > 1e-6f) return false;
    }
    return true;
}

void matMul(const Mat3x4& a, const Mat3x4& b, Mat3x4& out) {
    float A[16], B[16], C[16];
    for (int r = 0; r < 3; ++r)
        for (int c = 0; c < 4; ++c) { A[r * 4 + c] = a.m[r * 4 + c]; B[r * 4 + c] = b.m[r * 4 + c]; }
    for (int c = 0; c < 4; ++c) { A[12 + c] = (c == 3) ? 1.f : 0.f; B[12 + c] = (c == 3) ? 1.f : 0.f; }
    for (int r = 0; r < 4; ++r) {
        for (int c = 0; c < 4; ++c) {
            float s = 0;
            for (int k = 0; k < 4; ++k) s += A[r * 4 + k] * B[k * 4 + c];
            C[r * 4 + c] = s;
        }
    }
    for (int r = 0; r < 3; ++r)
        for (int c = 0; c < 4; ++c) out.m[r * 4 + c] = C[r * 4 + c];
}

void matApply(const Mat3x4& t, float x, float y, float z, float o[3]) {
    o[0] = t.m[0] * x + t.m[1] * y + t.m[2] * z + t.m[3];
    o[1] = t.m[4] * x + t.m[5] * y + t.m[6] * z + t.m[7];
    o[2] = t.m[8] * x + t.m[9] * y + t.m[10] * z + t.m[11];
}

bool parseMat3x4(const std::string& s, Mat3x4& out) {
    std::istringstream is(s);
    float v[12];
    int n = 0;
    while (n < 12 && (is >> v[n])) ++n;
    if (n != 12) return false;
    // Source order: r11 r12 r13 r21 r22 r23 r31 r32 r33 tx ty tz.
    // Rearrange into internal row-major 3x4 with translation in column 4.
    out.m[0]  = v[0]; out.m[1]  = v[1]; out.m[2]  = v[2];  out.m[3]  = v[9];
    out.m[4]  = v[3]; out.m[5]  = v[4]; out.m[6]  = v[5];  out.m[7]  = v[10];
    out.m[8]  = v[6]; out.m[9]  = v[7]; out.m[10] = v[8];  out.m[11] = v[11];
    return true;
}

// Strict attribute lookup: the match must not be the tail of a longer name, so
// asking for "id" will not pick up "objectid", while "path" still matches "p:path".
bool xmlAttrNamed(const std::string& tag, const char* attr, std::string& out) {
    if (!attr) return false;
    const size_t n = std::strlen(attr);
    size_t p = 0;
    while ((p = tag.find(attr, p)) != std::string::npos) {
        const char left = (p == 0) ? ' ' : tag[p - 1];
        const bool leftOk = !(std::isalnum(static_cast<unsigned char>(left)) || left == '_');
        const size_t after = p + n;
        const bool rightOk = after < tag.size() &&
            (tag[after] == '=' || tag[after] == ' ' || tag[after] == '/' || tag[after] == '>');
        if (leftOk && rightOk) {
            const size_t eq = tag.find('=', after);
            if (eq != std::string::npos) {
                const size_t q = tag.find('"', eq);
                if (q != std::string::npos) {
                    const size_t q2 = tag.find('"', q + 1);
                    if (q2 != std::string::npos) {
                        out = tag.substr(q + 1, q2 - q - 1);
                        return true;
                    }
                }
            }
        }
        p += n;
    }
    return false;
}

// One <object> of a 3MF model part: either triangle soup or references to other
// objects (possibly living in another .model part of the package).
struct Object3mf {
    struct Ref {
        std::string src;      // package path, OPC relationship id, or empty (same part)
        uint32_t objectId = 0;
        bool hasObjectId = false;
        Mat3x4 xf;
    };
    std::vector<float> pos;
    std::vector<uint32_t> idx;
    std::vector<Ref> refs;
};

struct BuildItem3mf {
    uint32_t objectId = 0;
    Mat3x4 xf;
};

// Pull vertices/triangles of one <mesh> block into flat arrays.
void scanMeshBlock3mf(const std::string& xml, size_t from, size_t to, Object3mf& obj, size_t cap) {
    size_t p = from;
    while ((p = xml.find("<vertex", p)) != std::string::npos && p < to) {
        const size_t lineEnd = xml.find(">", p);
        if (lineEnd == std::string::npos) break;
        const std::string tag = xml.substr(p, lineEnd - p);
        std::string xs, ys, zs;
        xmlAttrNamed(tag, "x", xs);
        xmlAttrNamed(tag, "y", ys);
        xmlAttrNamed(tag, "z", zs);
        obj.pos.push_back(xs.empty() ? 0.f : static_cast<float>(std::atof(xs.c_str())));
        obj.pos.push_back(ys.empty() ? 0.f : static_cast<float>(std::atof(ys.c_str())));
        obj.pos.push_back(zs.empty() ? 0.f : static_cast<float>(std::atof(zs.c_str())));
        p = lineEnd;
        if (obj.pos.size() / 3 >= cap) break;
    }
    p = from;
    while ((p = xml.find("<triangle", p)) != std::string::npos && p < to) {
        const size_t lineEnd = xml.find(">", p);
        if (lineEnd == std::string::npos) break;
        const std::string tag = xml.substr(p, lineEnd - p);
        std::string a1, a2, a3;
        xmlAttrNamed(tag, "v1", a1);
        xmlAttrNamed(tag, "v2", a2);
        xmlAttrNamed(tag, "v3", a3);
        const int32_t i1 = static_cast<int32_t>(std::atoi(a1.c_str()));
        const int32_t i2 = static_cast<int32_t>(std::atoi(a2.c_str()));
        const int32_t i3 = static_cast<int32_t>(std::atoi(a3.c_str()));
        // 3MF triangle vertex references are 0-indexed into the same <vertices> block.
        const uint32_t nVerts = static_cast<uint32_t>(obj.pos.size() / 3);
        if (i1 >= 0 && i2 >= 0 && i3 >= 0 &&
            static_cast<uint32_t>(i1) < nVerts && static_cast<uint32_t>(i2) < nVerts &&
            static_cast<uint32_t>(i3) < nVerts) {
            obj.idx.push_back(static_cast<uint32_t>(i1));
            obj.idx.push_back(static_cast<uint32_t>(i2));
            obj.idx.push_back(static_cast<uint32_t>(i3));
        }
        p = lineEnd;
        if (obj.idx.size() / 3 >= cap) break;
    }
}

// End of the <mesh> block that starts at `start`, bounded by `limit`.
size_t meshBlockEnd3mf(const std::string& xml, size_t start, size_t limit) {
    const size_t mEnd = xml.find("</mesh>", start);
    if (mEnd != std::string::npos && mEnd < limit) return mEnd + 7;
    const size_t vEnd = xml.find("</vertices>", start);
    if (vEnd != std::string::npos && vEnd < limit) return vEnd + 11;
    return limit;
}

// Scan one <mesh> block and append it with correct index base. Returns triangle count.
size_t appendMeshBlock3mf(const std::string& xml, size_t from, size_t to,
                          std::vector<float>& pos, std::vector<uint32_t>& idx, size_t cap) {
    Object3mf blk;
    scanMeshBlock3mf(xml, from, to, blk, cap);
    const uint32_t base = static_cast<uint32_t>(pos.size() / 3);
    for (float v : blk.pos) pos.push_back(v);
    for (uint32_t ix : blk.idx) idx.push_back(base + ix);
    return blk.idx.size() / 3;
}

// Parse every <object> and <build><item> of one model part.
void collect3mfObjects(const std::string& xml, std::map<uint32_t, Object3mf>& objs,
                       std::vector<BuildItem3mf>& items, size_t cap) {
    size_t p = 0;
    while ((p = xml.find("<object", p)) != std::string::npos) {
        const size_t tagEnd = xml.find('>', p);
        if (tagEnd == std::string::npos) break;
        const std::string tag = xml.substr(p, tagEnd - p);
        std::string idStr;
        if (!xmlAttrNamed(tag, "id", idStr)) { p = tagEnd; continue; }
        const uint32_t id = static_cast<uint32_t>(std::strtoul(idStr.c_str(), nullptr, 10));
        const size_t close = xml.find("</object>", tagEnd);
        const size_t bodyEnd = (close == std::string::npos) ? xml.size() : close;
        Object3mf obj;
        size_t q = tagEnd;
        while ((q = xml.find("<mesh", q)) != std::string::npos && q < bodyEnd) {
            const size_t stop = std::min(bodyEnd, meshBlockEnd3mf(xml, q, bodyEnd));
            appendMeshBlock3mf(xml, q, stop, obj.pos, obj.idx, cap);
            q = stop;
            if (obj.pos.size() / 3 >= cap) break;
        }
        q = tagEnd;
        while ((q = xml.find("<component", q)) != std::string::npos && q < bodyEnd) {
            const size_t t2 = xml.find('>', q);
            if (t2 == std::string::npos) break;
            const std::string ctag = xml.substr(q, t2 - q);
            Object3mf::Ref ref;
            std::string s;
            if (xmlAttrNamed(ctag, "path", s) || xmlAttrNamed(ctag, "src", s)) ref.src = s;
            s.clear();
            if (xmlAttrNamed(ctag, "objectid", s) || xmlAttrNamed(ctag, "object", s)) {
                ref.objectId = static_cast<uint32_t>(std::strtoul(s.c_str(), nullptr, 10));
                ref.hasObjectId = true;
            }
            s.clear();
            if (!xmlAttrNamed(ctag, "transform", s) || !parseMat3x4(s, ref.xf)) ref.xf = Mat3x4();
            obj.refs.push_back(std::move(ref));
            q = t2;
        }
        objs[id] = std::move(obj);
        p = bodyEnd + 1;
        if (xml.size() - p < 8) break;
    }
    const size_t b = xml.find("<build");
    if (b != std::string::npos) {
        const size_t bEnd = xml.find("</build>", b);
        const size_t bStop = (bEnd == std::string::npos) ? xml.size() : bEnd;
        size_t q = b;
        while ((q = xml.find("<item", q)) != std::string::npos && q < bStop) {
            const size_t t2 = xml.find('>', q);
            if (t2 == std::string::npos) break;
            const std::string tag = xml.substr(q, t2 - q);
            BuildItem3mf it;
            std::string s;
            if (!xmlAttrNamed(tag, "objectid", s)) xmlAttrNamed(tag, "id", s);
            it.objectId = static_cast<uint32_t>(std::strtoul(s.c_str(), nullptr, 10));
            s.clear();
            if (!xmlAttrNamed(tag, "transform", s) || !parseMat3x4(s, it.xf)) it.xf = Mat3x4();
            items.push_back(it);
            q = t2;
        }
    }
}

bool endsWithLower(const std::string& low, const char* tail) {
    const size_t n = std::strlen(tail);
    return low.size() >= n && low.compare(low.size() - n, n, tail) == 0;
}

// Turns "package/3D/Objects/x.model" / "\3D\Objects\x.model" into "3d/objects/x.model".
std::string normPkgPath(std::string s) {
    for (auto& c : s) {
        if (c == '\\') c = '/';
        c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
    }
    if (s.compare(0, 9, "package//") == 0) s.erase(0, 9);
    else if (s.compare(0, 8, "package/") == 0) s.erase(0, 8);
    while (!s.empty() && s.front() == '/') s.erase(s.begin());
    return s;
}

// Resolves an OPC relative reference ("../Objects/x.model") against a part directory.
std::string joinPkgPath(const std::string& dir, const std::string& rel) {
    std::vector<std::string> segs;
    size_t p = 0;
    while (p < dir.size()) {
        const size_t q = dir.find('/', p);
        const std::string seg = dir.substr(p, q == std::string::npos ? std::string::npos : q - p);
        if (!seg.empty() && seg != ".") segs.push_back(seg);
        if (q == std::string::npos) break;
        p = q + 1;
    }
    p = 0;
    while (p < rel.size()) {
        const size_t q = rel.find('/', p);
        const std::string seg = rel.substr(p, q == std::string::npos ? std::string::npos : q - p);
        if (seg == "..") { if (!segs.empty()) segs.pop_back(); }
        else if (!seg.empty() && seg != ".") segs.push_back(seg);
        if (q == std::string::npos) break;
        p = q + 1;
    }
    std::string out;
    for (size_t i = 0; i < segs.size(); ++i) { if (i) out += '/'; out += segs[i]; }
    return out;
}

// Resolves components across OPC parts so Bambu Studio / newer slicers that keep
// geometry in 3D/Objects/object_N.model still render on their own.
struct Loader3mf {
    zip::Reader zr;
    std::vector<Byte> buf;
    const ParseOpts* o = nullptr;
    std::map<std::string, std::map<uint32_t, Object3mf>> parts;   // part path -> objects
    std::map<std::string, std::vector<BuildItem3mf>> builds;      // part path -> build items
    std::map<std::string, std::string> relTargets;                // relationship id -> part path
    std::set<std::string> visiting;
    size_t tris = 0;

    const std::map<uint32_t, Object3mf>* seedPart(const std::string& pathLower, const std::string& xml) {
        auto res = parts.emplace(pathLower, std::map<uint32_t, Object3mf>{});
        if (res.second) {
            std::vector<BuildItem3mf> items;
            collect3mfObjects(xml, res.first->second, items, o->maxTriangles);
            builds.emplace(pathLower, std::move(items));
        }
        return &res.first->second;
    }

    const std::map<uint32_t, Object3mf>* partObjects(const std::string& pathLower) {
        auto it = parts.find(pathLower);
        if (it != parts.end()) return &it->second;
        const zip::Entry* e = findEntry(pathLower);
        if (!e) return nullptr;
        std::vector<Byte> raw;
        if (!zip::readEntry(zr, buf.data(), buf.size(), *e, raw, 96ULL << 20)) return nullptr;
        const std::string xml(reinterpret_cast<const char*>(raw.data()), raw.size());
        raw.clear();
        raw.shrink_to_fit();
        return seedPart(pathLower, xml);
    }

    const zip::Entry* findEntry(const std::string& pathLower) const {
        for (const auto& e : zr.entries) {
            if (normPkgPath(e.name) == pathLower) return &e;
        }
        const std::string tail = "/" + pathLower;
        for (const auto& e : zr.entries) {
            const std::string low = normPkgPath(e.name);
            if (low.size() > tail.size() && low.compare(low.size() - tail.size(), tail.size(), tail) == 0)
                return &e;
        }
        for (const auto& e : zr.entries) {
            const std::string low = normPkgPath(e.name);
            const size_t slash = pathLower.find_last_of('/');
            const std::string base = (slash == std::string::npos) ? pathLower : pathLower.substr(slash + 1);
            if (!base.empty() && low.size() >= base.size() &&
                low.compare(low.size() - base.size(), base.size(), base) == 0)
                return &e;
        }
        return nullptr;
    }

    void loadRels() {
        for (const auto& e : zr.entries) {
            const std::string low = normPkgPath(e.name);
            if (!endsWithLower(low, ".model")) continue;
            const size_t slash = low.find_last_of('/');
            const std::string dir = (slash == std::string::npos) ? std::string() : low.substr(0, slash);
            const std::string rel = dir + "/_rels/" + low.substr(slash + 1) + ".rels";
            const zip::Entry* re = nullptr;
            for (const auto& c : zr.entries) {
                if (normPkgPath(c.name) == rel) { re = &c; break; }
            }
            if (!re) continue;
            std::vector<Byte> raw;
            if (!zip::readEntry(zr, buf.data(), buf.size(), *re, raw, 4ULL << 20)) continue;
            const std::string xml(reinterpret_cast<const char*>(raw.data()), raw.size());
            size_t q = 0;
            while ((q = xml.find("<Relationship", q)) != std::string::npos) {
                const size_t t2 = xml.find('>', q);
                if (t2 == std::string::npos) break;
                const std::string tag = xml.substr(q, t2 - q);
                std::string id, target;
                xmlAttrNamed(tag, "Id", id);
                xmlAttrNamed(tag, "Target", target);
                if (!id.empty() && !target.empty()) {
                    char head = target[0];
                    if (head == '\\') head = '/';
                    relTargets[id] = (head == '/')
                        ? normPkgPath(target)
                        : normPkgPath(joinPkgPath(dir, normPkgPath(target)));
                }
                q = t2;
            }
            raw.clear();
        }
    }

    std::string resolveSrc(const std::string& srcRaw, const std::string& curPart) const {
        if (srcRaw.empty()) return curPart;
        const std::string low = normPkgPath(srcRaw);
        if (low.find('/') == std::string::npos) {
            const auto it = relTargets.find(low);
            if (it != relTargets.end()) return it->second;
        }
        std::string raw = srcRaw;
        for (auto& c : raw) { if (c == '\\') c = '/'; }
        const bool absolute = raw[0] == '/' || raw.compare(0, 8, "package/") == 0;
        if (absolute) return low;
        const size_t slash = curPart.find_last_of('/');
        const std::string dir = (slash == std::string::npos) ? std::string() : curPart.substr(0, slash);
        return normPkgPath(joinPkgPath(dir, low));
    }

    void instantiate(const std::string& part, uint32_t id, const Mat3x4& xf, int depth, Mesh& out) {
        if (depth > 12 || tris >= o->maxTriangles || o->stopNow()) return;
        const std::string key = part + "#" + std::to_string(id);
        if (!visiting.insert(key).second) return;
        const std::map<uint32_t, Object3mf>* objs = partObjects(part);
        if (objs) {
            auto it = objs->find(id);
            if (it != objs->end()) {
                const Object3mf& ob = it->second;
                if (!ob.pos.empty() && !ob.idx.empty()) {
                    const uint32_t base = static_cast<uint32_t>(out.positions.size() / 3);
                    for (size_t i = 0; i + 2 < ob.pos.size(); i += 3) {
                        float p[3];
                        matApply(xf, ob.pos[i], ob.pos[i + 1], ob.pos[i + 2], p);
                        out.positions.push_back(p[0]);
                        out.positions.push_back(p[1]);
                        out.positions.push_back(p[2]);
                    }
                    for (uint32_t ix : ob.idx) out.indices.push_back(base + ix);
                    tris += ob.idx.size() / 3;
                }
                for (const auto& r : ob.refs) {
                    if (!r.hasObjectId) continue;
                    const std::string child = resolveSrc(r.src, part);
                    Mat3x4 cx;
                    matMul(xf, r.xf, cx);
                    instantiate(child, r.objectId, cx, depth + 1, out);
                    if (tris >= o->maxTriangles || o->stopNow()) break;
                }
            }
        }
        visiting.erase(key);
    }
};

const zip::Entry* pickMainModelEntry(const zip::Reader& zr) {
    const zip::Entry* any = nullptr;
    const zip::Entry* notObjects = nullptr;
    const zip::Entry* named = nullptr;
    for (const auto& e : zr.entries) {
        const std::string low = normPkgPath(e.name);
        if (!endsWithLower(low, ".model")) continue;
        if (low == "3d/3dmodel.model") return &e;
        if (endsWithLower(low, "3dmodel.model") && !named) named = &e;
        if (!any) any = &e;
        if (!notObjects && low.find("objects/") == std::string::npos) notObjects = &e;
    }
    if (named) return named;
    if (notObjects) return notObjects;
    return any;
}

// Raw <mesh> scan of one model part, ignoring objects/build (legacy behaviour).
size_t scanInlineMeshes(const std::string& xml, Mesh& out, const ParseOpts& o) {
    size_t total = 0;
    size_t start = 0;
    while ((start = xml.find("<mesh", start)) != std::string::npos) {
        if (o.stopNow()) break;
        const size_t stop = meshBlockEnd3mf(xml, start, xml.size());
        if (stop <= start) break;
        total += appendMeshBlock3mf(xml, start, stop, out.positions, out.indices, o.maxTriangles);
        start = stop;
        if (out.positions.size() / 3 >= o.maxTriangles) break;
    }
    return total;
}

// How well a picture stored inside the package represents the model itself.
// Bambu/Creality/Anycubic slicers put the real cover in Auxiliaries/.thumbnails
// (middle > small > 3mf); Metadata/plate_N.png are per-plate crop previews and
// only make sense as a last resort. 0 means "not a usable cover".
int score3mfThumbName(const std::string& low) {
    auto has = [&low](const char* t) { return low.find(t) != std::string::npos; };
    if (has("thumbnail_middle") || has("thumbnail_large") || has("thumbnail_big")) return 60;
    if (has("thumbnail_small")) return 50;
    if (has("thumbnail_3mf")) return 40;
    const size_t slash = low.find_last_of('/');
    const std::string base = (slash == std::string::npos) ? low : low.substr(slash + 1);
    if (base == "thumbnail.png" || base == "thumbnail.jpg" ||
        base == "thumbnail.jpeg" || base == "thumbnail.bmp") return 30;
    if (has(".thumbnails/")) return 28;
    if (has("thumbnail")) return 25;
    if (has("preview")) return 20;
    if (low.compare(0, 9, "metadata/") == 0 || has("/metadata/")) return 1;
    if (has("no_light") || has("nopick") || has("pick_") ||
        has("plate_") || has("top_") || has("icon")) return 1;
    return 8;
}

// Picks the best cover picture of a 3MF package: highest rank, then biggest bytes.
const zip::Entry* pick3mfThumbEntry(const zip::Reader& zr) {
    const zip::Entry* best = nullptr;
    int bestRank = 0;
    uint64_t bestSize = 0;
    for (const auto& e : zr.entries) {
        const std::string low = normPkgPath(e.name);
        if (!endsWithLower(low, ".png") && !endsWithLower(low, ".jpg") &&
            !endsWithLower(low, ".jpeg") && !endsWithLower(low, ".bmp")) continue;
        if (e.rawSize == 0) continue;
        const int rank = score3mfThumbName(low);
        if (rank <= 0) continue;
        if (rank > bestRank || (rank == bestRank && e.rawSize > bestSize)) {
            best = &e;
            bestRank = rank;
            bestSize = e.rawSize;
        }
    }
    return best;
}

}  // namespace

Rc parse3mfMesh(const std::wstring& path, Mesh& out, const ParseOpts& o) {
    meshClear(out);
    HANDLE h = CreateFileW(path.c_str(), GENERIC_READ, FILE_SHARE_READ, nullptr,
                           OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (h == INVALID_HANDLE_VALUE) return Rc::IoError;
    LARGE_INTEGER sz;
    if (!GetFileSizeEx(h, &sz) || sz.QuadPart <= 0) { CloseHandle(h); return Rc::EmptyFile; }
    if (sz.QuadPart > static_cast<LONGLONG>(o.maxBytes)) { CloseHandle(h); return Rc::TooLarge; }
    std::vector<Byte> data(static_cast<size_t>(sz.QuadPart));
    DWORD got = 0;
    ReadFile(h, data.data(), static_cast<DWORD>(data.size()), &got, nullptr);
    CloseHandle(h);
    data.resize(got);
    zip::Reader zr = zip::open(data.data(), data.size());
    if (!zr.ok) return Rc::ParseError;
    const zip::Entry* main = pickMainModelEntry(zr);
    if (!main) return Rc::ParseError;

    Loader3mf l;
    l.zr = std::move(zr);
    l.buf = std::move(data);
    l.o = &o;
    l.loadRels();

    const std::string mainLow = normPkgPath(main->name);
    std::vector<Byte> raw;
    if (!zip::readEntry(l.zr, l.buf.data(), l.buf.size(), *main, raw, 96ULL << 20)) {
        return Rc::ParseError;
    }
    const std::string mainXml(reinterpret_cast<const char*>(raw.data()), raw.size());
    raw.clear();
    raw.shrink_to_fit();
    const std::map<uint32_t, Object3mf>* objs = l.seedPart(mainLow, mainXml);

    // Preferred route: honour the <build> section, following components that
    // slicers store in separate 3D/Objects/object_N.model parts.
    const auto build = l.builds.find(mainLow);
    if (build != l.builds.end() && !build->second.empty()) {
        for (const auto& bi : build->second) {
            if (o.stopNow()) return Rc::Timeout;
            l.instantiate(mainLow, bi.objectId, bi.xf, 0, out);
            if (l.tris >= o.maxTriangles) break;
        }
    } else if (!objs->empty()) {
        auto root = objs->find(1);
        if (root == objs->end()) root = objs->begin();
        l.instantiate(mainLow, root->first, Mat3x4(), 0, out);
    }

    if (out.indices.empty()) {
        meshClear(out);
        scanInlineMeshes(mainXml, out, o);
    }

    // Last resort: some packages keep the build section in another part.
    if (out.indices.empty()) {
        for (size_t i = 0; i < l.zr.entries.size(); ++i) {
            if (o.stopNow()) return Rc::Timeout;
            const std::string low = normPkgPath(l.zr.entries[i].name);
            if (!endsWithLower(low, ".model") || low == mainLow) continue;
            const std::map<uint32_t, Object3mf>* part = l.partObjects(low);
            if (!part || part->empty()) continue;
            const auto items = l.builds.find(low);
            if (items != l.builds.end() && !items->second.empty()) {
                for (const auto& bi : items->second) l.instantiate(low, bi.objectId, bi.xf, 0, out);
            } else {
                auto cand = part->find(1);
                if (cand == part->end()) cand = part->begin();
                l.instantiate(low, cand->first, Mat3x4(), 0, out);
            }
            if (!out.indices.empty()) break;
        }
    }

    if (out.indices.empty()) return Rc::ParseError;
    recomputeBounds(out);
    computeVertexNormals(out);
    return Rc::Ok;
}

// ------------------------------------------------------- dispatch ------------

Rc parseMeshFor(const std::wstring& path, Mesh& out, Kind& kind, const ParseOpts& o) {
    const FormatDef* fmt = formatOfPath(path);
    if (!fmt) { kind = Kind::Unknown; return Rc::Unsupported; }
    kind = fmt->kind;
    if (fmt->kind != Kind::Mesh) return Rc::Unsupported;
    const std::wstring wext = extOf(path);
    std::string ext(wext.begin(), wext.end());
    if (!ext.empty() && ext[0] == '.') ext.erase(0, 1);
    if (ext == "stl") return parseStl(path, out, o);
    if (ext == "obj") return parseObj(path, out, o);
    if (ext == "ply") return parsePly(path, out, o);
    if (ext == "off") return parseOff(path, out, o);
    if (ext == "3ds") return parse3ds(path, out, o);
    if (ext == "amf") return parseAmfMesh(path, out, o);
    if (ext == "3mf") return parse3mfMesh(path, out, o);
    if (ext == "fbx") return parseFbxMesh(path, out, o);
    return Rc::Unsupported;
}

// --------------------------------------------------- embedded images ---------

Rc extractEmbeddedImage(const std::wstring& path, std::vector<Byte>& out, uint64_t maxBytes) {
    out.clear();
    const FormatDef* fmt = formatOfPath(path);
    if (!fmt) return Rc::Unsupported;
    const std::wstring wext = extOf(path);
    std::string ext(wext.begin(), wext.end());
    if (!ext.empty() && ext[0] == '.') ext.erase(0, 1);
    HANDLE h = CreateFileW(path.c_str(), GENERIC_READ, FILE_SHARE_READ, nullptr,
                           OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (h == INVALID_HANDLE_VALUE) return Rc::IoError;
    LARGE_INTEGER sz;
    if (!GetFileSizeEx(h, &sz) || sz.QuadPart <= 0) { CloseHandle(h); return Rc::EmptyFile; }
    if (sz.QuadPart > 128ULL << 20) { CloseHandle(h); return Rc::TooLarge; }
    std::vector<Byte> buf(static_cast<size_t>(sz.QuadPart));
    DWORD got = 0;
    ReadFile(h, buf.data(), static_cast<DWORD>(buf.size()), &got, nullptr);
    CloseHandle(h);
    buf.resize(got);

    if (ext == "3mf") {
        zip::Reader zr = zip::open(buf.data(), buf.size());
        if (!zr.ok) return Rc::ParseError;
        // Not "the first PNG in the archive": slicers ship many pictures (per-plate
        // slices, pick masks). Take the one that actually shows the whole model.
        const zip::Entry* e = pick3mfThumbEntry(zr);
        if (!e) return Rc::Unsupported;
        if (!zip::readEntry(zr, buf.data(), buf.size(), *e, out, maxBytes)) return Rc::ParseError;
        return Rc::Ok;
    }
    if (ext == "amf") {
        std::string s(reinterpret_cast<const char*>(buf.data()), buf.size());
        size_t p = 0;
        while ((p = s.find("base64,", p)) != std::string::npos) {
            size_t start = p + 7;
            size_t end = s.find('"', start);
            if (end == std::string::npos) break;
            std::string b64 = s.substr(start, end - start);
            std::vector<Byte> dec;
            base64Decode(b64, dec);
            if (dec.size() > 100) { out.swap(dec); return Rc::Ok; }
            p = end + 1;
        }
        return Rc::Unsupported;
    }
    return Rc::Unsupported;
}

// ------------------------------------------------------- toolpath ------------

Rc parseToolpath(const std::wstring& path, std::vector<Seg2>& out, const ParseOpts& o) {
    out.clear();
    TextStream ts = TextStream::readFile(path, o.maxBytes);
    if (ts.text.empty()) return Rc::IoError;
    const std::string& s = ts.text;
    float cx = 0, cy = 0, cz = 0;
    int currentG = -1;
    for (size_t i = 0; i < s.size(); ++i) {
        if (o.stopNow()) return Rc::Timeout;
        if (s[i] == ';') {
            while (i < s.size() && s[i] != '\n') ++i;
            continue;
        }
        if (s[i] == ' ' || s[i] == '\t' || s[i] == '\r' || s[i] == '\n') continue;
        // Start of a token
        if (!std::isalpha(static_cast<unsigned char>(s[i]))) continue;
        std::vector<std::pair<char, float>> tok;
        size_t j = i;
        while (j < s.size()) {
            char c = s[j];
            if (std::isalpha(static_cast<unsigned char>(c))) {
                char cmd = static_cast<char>(std::toupper(static_cast<unsigned char>(c)));
                float v = 0;
                ++j;
                if (j < s.size() && (s[j] == '+' || s[j] == '-' || s[j] == '.')) {
                    char* endp = nullptr;
                    v = static_cast<float>(std::strtof(s.c_str() + j, &endp));
                    if (endp > s.c_str() + j) {
                        j = static_cast<size_t>(endp - s.c_str());
                    }
                }
                tok.push_back({cmd, v});
            } else if (c == ' ' || c == '\t' || c == '\r' || c == '\n') {
                ++j;
            } else {
                ++j;
            }
        }
        i = j;
        bool hasG = false, hasX = false, hasY = false, hasZ = false;
        float gx = 0, nx = cx, ny = cy, nz = cz;
        for (auto& kv : tok) {
            if (kv.first == 'G') { gx = kv.second; hasG = true; }
            else if (kv.first == 'X') nx = kv.second;
            else if (kv.first == 'Y') ny = kv.second;
            else if (kv.first == 'Z') { nz = kv.second; hasZ = true; }
            (void)hasX; (void)hasY;
        }
        if (hasG) currentG = static_cast<int>(gx);
        // Detect XY moves by checking original token sequence
        bool movedXY = false;
        for (auto& kv : tok) if (kv.first == 'X' || kv.first == 'Y') movedXY = true;
        (void)hasZ;
        if (!movedXY) continue;
        if (currentG != 0 && currentG != 1) continue;
        float oldX = cx, oldY = cy;
        cx = nx; cy = ny; cz = nz;
        if (oldX == cx && oldY == cy) continue;
        Seg2 seg;
        seg.x0 = oldX; seg.y0 = oldY; seg.x1 = cx; seg.y1 = cy; seg.z = cz;
        out.push_back(seg);
        if (out.size() > o.maxTriangles) return Rc::Ok;
    }
    if (out.empty()) return Rc::ParseError;
    return Rc::Ok;
}

// ------------------------------------------------------- GDI+ decode ---------

bool decodeImageBytes(const Byte* data, size_t dataLen, int px, Bitmap& out) {
    if (!data || dataLen < 64 || px <= 0 || px > 4096) return false;
    static ULONG_PTR gToken = 0;
    static bool gInited = false;
    if (!gInited) {
        Gdiplus::GdiplusStartupInput input;
        Gdiplus::Status st = Gdiplus::GdiplusStartup(&gToken, &input, nullptr);
        gInited = (st == Gdiplus::Ok);
        if (!gInited) return false;
    }
    // Create an HGLOBAL holding the bytes and stream it into GDI+
    HGLOBAL hGlobal = GlobalAlloc(GMEM_MOVEABLE, static_cast<SIZE_T>(dataLen));
    if (!hGlobal) return false;
    BYTE* dst = static_cast<BYTE*>(GlobalLock(hGlobal));
    if (!dst) { GlobalFree(hGlobal); return false; }
    std::memcpy(dst, data, dataLen);
    GlobalUnlock(hGlobal);
    IStream* stm = nullptr;
    HRESULT hr = CreateStreamOnHGlobal(hGlobal, TRUE, &stm);
    if (hr != S_OK) { GlobalFree(hGlobal); return false; }
    // Decode
    Gdiplus::Bitmap* bmp = new Gdiplus::Bitmap(stm);
    if (bmp->GetLastStatus() != Gdiplus::Ok) {
        delete bmp;
        stm->Release();
        return false;
    }
    int srcW = bmp->GetWidth();
    int srcH = bmp->GetHeight();
    if (srcW <= 0 || srcH <= 0) {
        delete bmp;
        stm->Release();
        return false;
    }
    Gdiplus::Rect rect(0, 0, srcW, srcH);
    Gdiplus::BitmapData bd;
    Gdiplus::Status gs = bmp->LockBits(&rect, Gdiplus::ImageLockModeRead,
                                       PixelFormat32bppARGB, &bd);
    if (gs != Gdiplus::Ok) {
        delete bmp;
        stm->Release();
        return false;
    }
    // Fit into px x px keeping aspect
    int scaleW = px, scaleH = px;
    float ratio = static_cast<float>(srcW) / static_cast<float>(srcH);
    if (ratio > 1.f) scaleH = static_cast<int>(px / ratio);
    else scaleW = static_cast<int>(px * ratio);
    if (scaleW < 1) scaleW = 1;
    if (scaleH < 1) scaleH = 1;
    int offX = (px - scaleW) / 2;
    int offY = (px - scaleH) / 2;

    out.w = px;
    out.h = px;
    out.px.assign(static_cast<size_t>(px) * px * 4, 0);

    const BYTE* src = static_cast<const BYTE*>(bd.Scan0);
    int stride = bd.Stride;
    BYTE* dstPx = out.px.data();
    for (int y = 0; y < px; ++y) {
        for (int x = 0; x < px; ++x) {
            size_t idx = (static_cast<size_t>(y) * px + x) * 4;
            if (x < offX || x >= offX + scaleW || y < offY || y >= offY + scaleH) continue;
            int sx = (x - offX) * srcW / scaleW;
            int sy = (y - offY) * srcH / scaleH;
            if (sx < 0 || sx >= srcW || sy < 0 || sy >= srcH) continue;
            const BYTE* sp = src + static_cast<size_t>(sy) * stride + static_cast<size_t>(sx) * 4;
            // GDI+ 32bppARGB in memory layout is BGRA
            dstPx[idx] = sp[0];
            dstPx[idx + 1] = sp[1];
            dstPx[idx + 2] = sp[2];
            dstPx[idx + 3] = sp[3];
        }
    }
    bmp->UnlockBits(&bd);
    delete bmp;
    stm->Release();
    return true;
}

}  // namespace bs3d
