#pragma once

#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif

#include <windows.h>

#include <algorithm>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <string>
#include <vector>

namespace bs3d {

using Byte = unsigned char;

// ---------------------------------------------------------------------------
// Minimal 3D math
// ---------------------------------------------------------------------------
struct Vec3 {
    float x = 0.f, y = 0.f, z = 0.f;
};

inline Vec3 operator+(Vec3 a, Vec3 b) { return {a.x + b.x, a.y + b.y, a.z + b.z}; }
inline Vec3 operator-(Vec3 a, Vec3 b) { return {a.x - b.x, a.y - b.y, a.z - b.z}; }
inline Vec3 operator*(Vec3 a, float s) { return {a.x * s, a.y * s, a.z * s}; }
inline Vec3 operator/(Vec3 a, float s) { return {a.x / s, a.y / s, a.z / s}; }
inline Vec3 operator*(float s, Vec3 a) { return {a.x * s, a.y * s, a.z * s}; }

inline float dot(Vec3 a, Vec3 b) { return a.x * b.x + a.y * b.y + a.z * b.z; }
inline Vec3 cross(Vec3 a, Vec3 b) {
    return {a.y * b.z - a.z * b.y, a.z * b.x - a.x * b.z, a.x * b.y - a.y * b.x};
}
inline float vlength(Vec3 a) { float d = dot(a, a); return d > 0.f ? sqrtf(d) : 0.f; }
inline Vec3 normalize(Vec3 a) { float l = vlength(a); return l > 1e-12f ? a / l : Vec3{0.f, 1.f, 0.f}; }
inline Vec3 minOf(Vec3 a, Vec3 b) { return {std::min(a.x, b.x), std::min(a.y, b.y), std::min(a.z, b.z)}; }
inline Vec3 maxOf(Vec3 a, Vec3 b) { return {std::max(a.x, b.x), std::max(a.y, b.y), std::max(a.z, b.z)}; }

// 3x3 row-major rotation/scale matrix.
struct Mat3 {
    float m[9] = {1, 0, 0, 0, 1, 0, 0, 0, 1};
    float at(int r, int c) const { return m[r * 3 + c]; }
};

inline Vec3 transformVec3(const Mat3& t, Vec3 v) {
    return {t.at(0, 0) * v.x + t.at(0, 1) * v.y + t.at(0, 2) * v.z,
            t.at(1, 0) * v.x + t.at(1, 1) * v.y + t.at(1, 2) * v.z,
            t.at(2, 0) * v.x + t.at(2, 1) * v.y + t.at(2, 2) * v.z};
}

inline Mat3 rotX(float a) {
    float c = cosf(a), s = sinf(a);
    return Mat3{{1, 0, 0, 0, c, -s, 0, s, c}};
}
inline Mat3 rotY(float a) {
    float c = cosf(a), s = sinf(a);
    return Mat3{{c, 0, s, 0, 1, 0, -s, 0, c}};
}
inline Mat3 rotZ(float a) {
    float c = cosf(a), s = sinf(a);
    return Mat3{{c, -s, 0, s, c, 0, 0, 0, 1}};
}
inline Mat3 multiply(const Mat3& a, const Mat3& b) {
    Mat3 r;
    for (int i = 0; i < 3; ++i)
        for (int j = 0; j < 3; ++j)
            r.m[i * 3 + j] = a.at(i, 0) * b.at(0, j) + a.at(i, 1) * b.at(1, j) + a.at(i, 2) * b.at(2, j);
    return r;
}

// ---------------------------------------------------------------------------
// Mesh containers
// ---------------------------------------------------------------------------
struct Mesh {
    std::vector<float> positions;    // 3 floats per vertex
    std::vector<float> normals;      // 3 floats per vertex (parallel to positions)
    std::vector<unsigned int> indices;  // 3 per triangle
    Vec3 bmin{0.f, 0.f, 0.f}, bmax{0.f, 0.f, 0.f};
    bool hasBounds = false;
    bool hasNormals = false;
    bool hasColors = false;
    std::vector<Byte> colors;        // 3 bytes per vertex, parallel to positions

    size_t vertexCount() const { return positions.size() / 3; }
    size_t triangleCount() const { return indices.size() / 3; }

    Vec3 pos(size_t i) const { return {positions[i * 3], positions[i * 3 + 1], positions[i * 3 + 2]}; }
    Vec3 nor(size_t i) const { return {normals[i * 3], normals[i * 3 + 1], normals[i * 3 + 2]}; }
};

void recomputeBounds(Mesh& mesh);
void computeVertexNormals(Mesh& mesh);
void mergeMesh(const Mesh& src, Mesh& dst);

// ---------------------------------------------------------------------------
// RGBA bitmap (top-down rows, straight alpha, BGRA order to match Windows DIB)
// ---------------------------------------------------------------------------
struct Bitmap {
    int w = 0, h = 0;
    std::vector<Byte> px;  // w*h*4, B,G,R,A

    Bitmap() = default;
    Bitmap(int w, int h) : w(w), h(h), px(static_cast<size_t>(w) * h * 4, 0) {}

    void clear(Byte b = 0, Byte g = 0, Byte r = 0, Byte a = 0) {
        for (size_t i = 0; i < px.size(); i += 4) { px[i] = b; px[i + 1] = g; px[i + 2] = r; px[i + 3] = a; }
    }
    Byte* row(int y) { return px.data() + static_cast<size_t>(y) * w * 4; }
    const Byte* row(int y) const { return px.data() + static_cast<size_t>(y) * w * 4; }
};

// ---------------------------------------------------------------------------
// Result codes shared by parsers / renderers
// ---------------------------------------------------------------------------
enum class Rc : int {
    Ok = 0,
    IoError = 1,
    EmptyFile = 2,
    ParseError = 3,
    Unsupported = 4,
    TooLarge = 5,
    Timeout = 6,
    OutOfMemory = 7,
};

const char* rcToString(Rc rc);

// Deadline helper: milliseconds since process start. Returns true when expired.
// Uses GetTickCount64 so the comparison stays valid across the 49.7-day
// DWORD wrap-around that GetTickCount would hit on long-lived processes
// (Explorer / dwm.exe routinely run for days).
bool deadlineExpired(uint64_t deadlineMs, uint64_t& elapsedMs);
inline bool withinBudget(uint64_t deadlineMs, uint64_t stepDivisor = 1) {
    (void)stepDivisor;
    if (deadlineMs == 0) return true;
    return GetTickCount64() < deadlineMs;
}

}  // namespace bs3d
