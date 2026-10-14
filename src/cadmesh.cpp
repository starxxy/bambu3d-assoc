#include "bs3d/cadmesh.h"
#include "bs3d/occplugin.h"
#include "bs3d/winutil.h"

#include <algorithm>
#include <cstdio>
#include <cstring>
#include <map>
#include <vector>

#ifndef M_PI
#define M_PI 3.14159265358979323846f
#endif

namespace bs3d {
namespace {

// Read the STEP manifest: every entity is "ID#TYPE(args);" on its own line.
// We collect the type and the argument string for every ID so callers can
// walk the graph. Empty args are represented by the empty string.
struct StepEntity {
    std::wstring type;
    std::vector<std::wstring> args;  // references like #42 / 'text' / numerics as-is
};

using StepTable = std::map<long long, StepEntity>;

bool isStepFile(const std::wstring& path) {
    HANDLE h = CreateFileW(path.c_str(), GENERIC_READ, FILE_SHARE_READ, nullptr,
                           OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (h == INVALID_HANDLE_VALUE) return false;
    char buf[64] = {0};
    DWORD got = 0;
    BOOL ok = ReadFile(h, buf, sizeof(buf) - 1, &got, nullptr);
    CloseHandle(h);
    if (!ok || got < 13) return false;
    // Skip optional "ISO-10303-21;" header, case-insensitive.
    std::string header(buf, got);
    std::string want = "ISO-10303-21;";
    if (header.size() < want.size()) return false;
    for (size_t i = 0; i < want.size(); ++i) {
        if (std::tolower(static_cast<unsigned char>(header[i])) != want[i]) return false;
    }
    return true;
}

bool parseStepTable(const std::wstring& path, StepTable& out) {
    std::vector<Byte> raw;
    if (!readFileBytes(path, raw, 256ULL << 20)) return false;
    if (raw.empty()) return false;

    // Convert to UTF-8 text stream (STEP files are ASCII; tolerate UTF-8).
    std::string text(reinterpret_cast<const char*>(raw.data()), raw.size());
    // Trim trailing NULs / whitespace.
    while (!text.empty() && (text.back() == '\n' || text.back() == '\r' ||
                             text.back() == ' '  || text.back() == '\0')) text.pop_back();

    out.clear();
    size_t pos = 0;
    while (pos < text.size()) {
        // Look for "NNN#TYPE(" ... ");" segment.
        size_t idStart = pos;
        while (pos < text.size() && text[pos] >= '0' && text[pos] <= '9') ++pos;
        if (pos == idStart || pos >= text.size() || text[pos] != '#') break;
        long long id = 0;
        for (size_t i = idStart; i < pos; ++i) id = id * 10 + (text[i] - '0');
        ++pos;  // skip '#'

        size_t typeStart = pos;
        while (pos < text.size() && (std::isalnum(static_cast<unsigned char>(text[pos])) || text[pos] == '_')) ++pos;
        std::string typeStr(text.begin() + typeStart, text.begin() + pos);
        if (typeStr.empty()) break;

        // Skip whitespace, expect '('
        while (pos < text.size() && (text[pos] == ' ' || text[pos] == '\t' ||
                                     text[pos] == '\n' || text[pos] == '\r')) ++pos;
        if (pos >= text.size() || text[pos] != '(') break;
        ++pos;

        // Read args until ');'
        size_t argStart = pos;
        int depth = 1;
        bool inString = false;
        char strQuote = 0;
        while (pos < text.size() && depth > 0) {
            char c = text[pos];
            if (inString) {
                if (c == strQuote) {
                    if (pos + 1 < text.size() && text[pos + 1] == strQuote) {
                        ++pos;  // escaped quote
                    } else {
                        inString = false;
                    }
                }
            } else if (c == '\'' || c == '"') {
                inString = true;
                strQuote = c;
            } else if (c == '(') {
                ++depth;
            } else if (c == ')') {
                --depth;
            }
            ++pos;
        }
        size_t argEnd = pos;
        if (argEnd > 0 && text[argEnd - 1] == ')') --argEnd;
        if (pos < text.size() && text[pos] == ';') ++pos;

        // Split args by top-level commas.
        std::vector<std::string> rawArgs;
        {
            std::string cur;
            int d = 0;
            bool s = false;
            char sq = 0;
            for (size_t i = argStart; i < argEnd; ++i) {
                char c = text[i];
                if (s) {
                    cur.push_back(c);
                    if (c == sq) {
                        if (i + 1 < argEnd && text[i + 1] == sq) {
                            cur.push_back(text[++i]);
                        } else {
                            s = false;
                        }
                    }
                } else if (c == '\'' || c == '"') {
                    s = true; sq = c; cur.push_back(c);
                } else if (c == '(') {
                    ++d; cur.push_back(c);
                } else if (c == ')') {
                    --d; cur.push_back(c);
                } else if (c == ',' && d == 0) {
                    rawArgs.push_back(cur);
                    cur.clear();
                } else {
                    cur.push_back(c);
                }
            }
            if (!cur.empty()) rawArgs.push_back(cur);
        }

        // Convert each arg to wstring (they're ASCII).
        StepEntity e;
        e.type.assign(typeStr.begin(), typeStr.end());
        e.args.reserve(rawArgs.size());
        for (const auto& a : rawArgs) {
            std::wstring w;
            w.reserve(a.size());
            for (char c : a) w.push_back(static_cast<wchar_t>(static_cast<unsigned char>(c)));
            e.args.push_back(std::move(w));
        }
        out[id] = std::move(e);
    }
    return !out.empty();
}

// Helper: pull "#NNN" reference tokens from an arg string.
std::vector<long long> refsOf(const StepEntity& e, size_t argIndex) {
    std::vector<long long> out;
    if (argIndex >= e.args.size()) return out;
    const std::wstring& s = e.args[argIndex];
    size_t i = 0;
    while (i < s.size()) {
        if (s[i] == '#') {
            ++i;
            long long v = 0;
            bool any = false;
            while (i < s.size() && s[i] >= L'0' && s[i] <= L'9') {
                v = v * 10 + (s[i] - L'0');
                ++i;
                any = true;
            }
            if (any) out.push_back(v);
        } else {
            ++i;
        }
    }
    return out;
}

bool findType(const StepTable& t, const wchar_t* type, std::vector<long long>& out) {
    out.clear();
    for (const auto& kv : t) {
        if (kv.second.type == type) out.push_back(kv.first);
    }
    return !out.empty();
}

}  // namespace

// ===========================================================================
// Built-in STEP fallback: minimal walker. We don't have OCCT so we can only
// count faces and report Unsupported to let the caller show a fallback badge.
// ===========================================================================
Rc meshStepBuiltin(const std::wstring& path, Mesh& out, const ParseOpts& o, const CadOpts& opts) {
    (void)o; (void)opts;
    if (!isStepFile(path)) return Rc::Unsupported;

    StepTable table;
    if (!parseStepTable(path, table)) return Rc::ParseError;

    // Walk ADVANCED_FACE entries; we can't tessellate without a real geometry
    // kernel, so just bail out. This is intentionally minimal so the code still
    // compiles without OpenCASCADE and the pipeline falls back to the badge.
    std::vector<long long> faces;
    if (findType(table, L"ADVANCED_FACE", faces)) {
        // Could emit a placeholder cube, but we'd rather be honest.
        return Rc::Unsupported;
    }
    return Rc::ParseError;
}


// ===========================================================================
// Dispatch
// ===========================================================================
Rc meshCad(const std::wstring& path, Mesh& out, const ParseOpts& o, const CadOpts& opts,
           bool* usedOcc) {
    if (usedOcc) *usedOcc = false;
    if (o.stopNow()) return Rc::Timeout;

    // 1) Built-in STEP manifest walker (works without any extra runtime).
    Rc rc = meshStepBuiltin(path, out, o, opts);
    if (rc == Rc::Ok) return Rc::Ok;

    // 2) Runtime OCCT plugin (bs3d_occ.dll), discovered via LoadLibrary.
    //    Installed on demand through the app's environment-check downloader;
    //    covers STEP/IGES/BREP with real kernel tessellation.
    if (rc == Rc::Unsupported || rc == Rc::ParseError) {
        Rc pr = meshCadViaPlugin(path, out, o);
        if (pr == Rc::Ok) {
            if (usedOcc) *usedOcc = true;
            return Rc::Ok;
        }
        if (pr == Rc::TooLarge || pr == Rc::Timeout) return pr;
    }
    return rc;
}

}  // namespace bs3d
