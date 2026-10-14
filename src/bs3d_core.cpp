#include "bs3d/bs3d.h"

#include <algorithm>
#include <cmath>
#include <cstring>

namespace bs3d {

const char* rcToString(Rc rc) {
    switch (rc) {
        case Rc::Ok: return "OK";
        case Rc::IoError: return "I/O error";
        case Rc::EmptyFile: return "empty file";
        case Rc::ParseError: return "parse error";
        case Rc::Unsupported: return "unsupported";
        case Rc::TooLarge: return "file too large";
        case Rc::Timeout: return "timeout";
        case Rc::OutOfMemory: return "out of memory";
    }
    return "unknown";
}

bool deadlineExpired(uint64_t deadlineMs, uint64_t& elapsedMs) {
    if (deadlineMs == 0) { elapsedMs = 0; return false; }
    return GetTickCount64() >= deadlineMs;
}

void recomputeBounds(Mesh& mesh) {
    mesh.hasBounds = false;
    mesh.bmin = {1e30f, 1e30f, 1e30f};
    mesh.bmax = {-1e30f, -1e30f, -1e30f};
    for (size_t i = 0; i < mesh.vertexCount(); ++i) {
        Vec3 p = mesh.pos(i);
        if (!std::isfinite(p.x) || !std::isfinite(p.y) || !std::isfinite(p.z)) continue;
        mesh.bmin = minOf(mesh.bmin, p);
        mesh.bmax = maxOf(mesh.bmax, p);
    }
    if (mesh.vertexCount() == 0) {
        mesh.bmin = {0.f, 0.f, 0.f};
        mesh.bmax = {0.f, 0.f, 0.f};
        return;
    }
    mesh.hasBounds = true;
}

void computeVertexNormals(Mesh& mesh) {
    const size_t v = mesh.vertexCount();
    if (v == 0 || mesh.triangleCount() == 0) { mesh.hasNormals = false; return; }
    if (mesh.normals.size() != mesh.positions.size()) mesh.normals.assign(mesh.positions.size(), 0.f);
    for (size_t t = 0; t < mesh.triangleCount(); ++t) {
        const unsigned int a = mesh.indices[t * 3];
        const unsigned int b = mesh.indices[t * 3 + 1];
        const unsigned int c = mesh.indices[t * 3 + 2];
        if (a >= v || b >= v || c >= v) continue;
        Vec3 pa = mesh.pos(a), pb = mesh.pos(b), pc = mesh.pos(c);
        Vec3 n = normalize(cross(pb - pa, pc - pa));
        if (n.x == 0.f && n.y == 0.f && n.z == 0.f) n = Vec3{0.f, 1.f, 0.f};
        float area = vlength(cross(pb - pa, pc - pa));  // weight by area, avoids spikes
        for (unsigned int idx : {a, b, c}) {
            mesh.normals[idx * 3] += n.x * area;
            mesh.normals[idx * 3 + 1] += n.y * area;
            mesh.normals[idx * 3 + 2] += n.z * area;
        }
    }
    for (size_t i = 0; i < v; ++i) {
        Vec3 n = normalize(mesh.nor(i));
        if (n.x == 0.f && n.y == 0.f && n.z == 0.f) n = Vec3{0.f, 1.f, 0.f};
        mesh.normals[i * 3] = n.x;
        mesh.normals[i * 3 + 1] = n.y;
        mesh.normals[i * 3 + 2] = n.z;
    }
    mesh.hasNormals = true;
}

void mergeMesh(const Mesh& src, Mesh& dst) {
    if (src.vertexCount() == 0 || src.triangleCount() == 0) return;
    const unsigned int base = static_cast<unsigned int>(dst.vertexCount());
    dst.positions.insert(dst.positions.end(), src.positions.begin(), src.positions.end());
    if (!src.normals.empty()) {
        if (dst.normals.size() < dst.positions.size()) dst.normals.resize(dst.positions.size(), 0.f);
        dst.normals.insert(dst.normals.end(), src.normals.begin(), src.normals.end());
        dst.hasNormals = dst.hasNormals && src.hasNormals;
    }
    for (unsigned int i : src.indices) dst.indices.push_back(i + base);
    if (!dst.hasBounds) recomputeBounds(dst);
    recomputeBounds(dst);
}

}  // namespace bs3d
