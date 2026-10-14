#include "bs3d/winutil.h"

#include <bcrypt.h>
#include <shlobj.h>
#include <shellapi.h>
#include <tlhelp32.h>
#include <tchar.h>
#include <winreg.h>

#include <algorithm>
#include <cstdio>
#include <sstream>

#pragma comment(lib, "bcrypt.lib")
#pragma comment(lib, "shell32.lib")
#pragma comment(lib, "advapi32.lib")

namespace bs3d {

// ---------------------------------------------------------------------------
// Strings
// ---------------------------------------------------------------------------
std::wstring lowerW(const std::wstring& s) {
    std::wstring r = s;
    std::transform(r.begin(), r.end(), r.begin(),
                   [](wchar_t c) { return static_cast<wchar_t>(::towlower(static_cast<wchar_t>(c))); });
    return r;
}

std::wstring trimW(const std::wstring& s) {
    size_t a = 0, b = s.size();
    while (a < b && isspace(static_cast<unsigned char>(s[a]))) ++a;
    while (b > a && isspace(static_cast<unsigned char>(s[b - 1]))) --b;
    return s.substr(a, b - a);
}

std::vector<std::wstring> splitW(const std::wstring& s, wchar_t sep) {
    std::vector<std::wstring> out;
    std::wstring cur;
    for (wchar_t c : s) {
        if (c == sep) { out.push_back(cur); cur.clear(); } else cur.push_back(c);
    }
    out.push_back(cur);
    return out;
}

bool startsWith(const std::wstring& s, const std::wstring& prefix, bool ci) {
    if (s.size() < prefix.size()) return false;
    if (ci) return lowerW(s.substr(0, prefix.size())) == lowerW(prefix);
    return s.compare(0, prefix.size(), prefix) == 0;
}

std::wstring toHexW(const unsigned char* data, size_t n) {
    static const wchar_t d[] = L"0123456789abcdef";
    std::wstring out(n * 2, L'0');
    for (size_t i = 0; i < n; ++i) {
        out[i * 2] = d[data[i] >> 4];
        out[i * 2 + 1] = d[data[i] & 0xF];
    }
    return out;
}

// ---------------------------------------------------------------------------
// Registry
// ---------------------------------------------------------------------------
bool keyExists(HKEY root, const std::wstring& subKey) {
    HKEY h = nullptr;
    if (RegOpenKeyExW(root, subKey.c_str(), 0, KEY_READ, &h) != ERROR_SUCCESS) return false;
    RegCloseKey(h);
    return true;
}

bool ensureRegKey(HKEY root, const std::wstring& subKey) {
    HKEY h = nullptr;
    LSTATUS s = RegCreateKeyExW(root, subKey.c_str(), 0, nullptr, 0, KEY_READ | KEY_WRITE, nullptr, &h, nullptr);
    if (s != ERROR_SUCCESS) return false;
    RegCloseKey(h);
    return true;
}

bool setRegString(HKEY root, const std::wstring& subKey, const std::wstring& valueName, const std::wstring& data) {
    if (!ensureRegKey(root, subKey)) return false;
    HKEY h = nullptr;
    if (RegOpenKeyExW(root, subKey.c_str(), 0, KEY_SET_VALUE, &h) != ERROR_SUCCESS) return false;
    std::wstring v = data;
    v.push_back(L'\0');
    LSTATUS s = RegSetValueExW(h, valueName.c_str(), 0, REG_SZ,
                               reinterpret_cast<const BYTE*>(v.c_str()),
                               static_cast<DWORD>(v.size() * sizeof(wchar_t)));
    RegCloseKey(h);
    return s == ERROR_SUCCESS;
}

bool setRegDWORD(HKEY root, const std::wstring& subKey, const std::wstring& valueName, DWORD value) {
    if (!ensureRegKey(root, subKey)) return false;
    HKEY h = nullptr;
    if (RegOpenKeyExW(root, subKey.c_str(), 0, KEY_SET_VALUE, &h) != ERROR_SUCCESS) return false;
    LSTATUS s = RegSetValueExW(h, valueName.c_str(), 0, REG_DWORD,
                               reinterpret_cast<const BYTE*>(&value), sizeof(value));
    RegCloseKey(h);
    return s == ERROR_SUCCESS;
}

bool getRegString(HKEY root, const std::wstring& subKey, const std::wstring& valueName, std::wstring& out) {
    HKEY h = nullptr;
    if (RegOpenKeyExW(root, subKey.c_str(), 0, KEY_READ, &h) != ERROR_SUCCESS) return false;
    DWORD need = 0;
    LSTATUS s = RegQueryValueExW(h, valueName.c_str(), nullptr, nullptr, nullptr, &need);
    if (s != ERROR_SUCCESS || need == 0) { RegCloseKey(h); return false; }
    std::vector<BYTE> buf(need);
    if (RegQueryValueExW(h, valueName.c_str(), nullptr, nullptr, buf.data(), &need) != ERROR_SUCCESS) {
        RegCloseKey(h);
        return false;
    }
    RegCloseKey(h);
    out.assign(reinterpret_cast<const wchar_t*>(buf.data()), (need / sizeof(wchar_t)));
    while (!out.empty() && out.back() == L'\0') out.pop_back();
    return true;
}

bool getRegDWORD(HKEY root, const std::wstring& subKey, const std::wstring& valueName, DWORD& out) {
    HKEY h = nullptr;
    if (RegOpenKeyExW(root, subKey.c_str(), 0, KEY_READ, &h) != ERROR_SUCCESS) return false;
    DWORD need = sizeof(out);
    LSTATUS s = RegQueryValueExW(h, valueName.c_str(), nullptr, nullptr, reinterpret_cast<BYTE*>(&out), &need);
    RegCloseKey(h);
    return s == ERROR_SUCCESS;
}

bool deleteRegValue(HKEY root, const std::wstring& subKey, const std::wstring& valueName) {
    HKEY h = nullptr;
    if (RegOpenKeyExW(root, subKey.c_str(), 0, KEY_SET_VALUE, &h) != ERROR_SUCCESS) return false;
    LSTATUS s = RegDeleteValueW(h, valueName.c_str());
    RegCloseKey(h);
    return s == ERROR_SUCCESS || s == ERROR_FILE_NOT_FOUND;
}

bool deleteRegTree(HKEY root, const std::wstring& subKey) {
    LSTATUS s = RegDeleteTreeW(root, subKey.c_str());
    return s == ERROR_SUCCESS || s == ERROR_FILE_NOT_FOUND;
}

// ---------------------------------------------------------------------------
// Files
// ---------------------------------------------------------------------------
bool fileExists(const std::wstring& path) {
    if (path.empty()) return false;
    DWORD a = GetFileAttributesW(path.c_str());
    return a != INVALID_FILE_ATTRIBUTES && !(a & FILE_ATTRIBUTE_DIRECTORY);
}

bool ensureDirectory(const std::wstring& dir) {
    if (dir.empty()) return false;
    DWORD a = GetFileAttributesW(dir.c_str());
    if (a != INVALID_FILE_ATTRIBUTES) return (a & FILE_ATTRIBUTE_DIRECTORY) != 0;
    std::wstring p = dir;
    while (!p.empty() && p.back() == L'\\') p.pop_back();
    size_t slash = p.find_last_of(L'\\');
    if (slash != std::wstring::npos && slash > 1) {
        std::wstring parent = p.substr(0, slash);
        if (!ensureDirectory(parent)) return false;
    }
    return CreateDirectoryW(dir.c_str(), nullptr) == TRUE || GetFileAttributesW(dir.c_str()) != INVALID_FILE_ATTRIBUTES;
}

bool writeFileBytes(const std::wstring& path, const void* data, size_t size) {
    HANDLE h = CreateFileW(path.c_str(), GENERIC_WRITE, FILE_SHARE_READ, nullptr, CREATE_ALWAYS,
                           FILE_ATTRIBUTE_NORMAL, nullptr);
    if (h == INVALID_HANDLE_VALUE) return false;
    const Byte* p = static_cast<const Byte*>(data);
    size_t left = size;
    DWORD written = 0;
    bool ok = true;
    while (left > 0) {
        if (!WriteFile(h, p, static_cast<DWORD>(left > 0x7FFFFFFF ? 0x7FFFFFFF : left), &written, nullptr) || written == 0) {
            ok = false;
            break;
        }
        p += written;
        left -= written;
    }
    CloseHandle(h);
    return ok;
}

bool readFileBytes(const std::wstring& path, std::vector<Byte>& out, uint64_t maxBytes) {
    HANDLE h = CreateFileW(path.c_str(), GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_WRITE, nullptr,
                           OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (h == INVALID_HANDLE_VALUE) return false;
    LARGE_INTEGER sz;
    if (!GetFileSizeEx(h, &sz) || sz.QuadPart < 0) { CloseHandle(h); return false; }
    if (sz.QuadPart > maxBytes) { CloseHandle(h); return false; }
    out.resize(static_cast<size_t>(sz.QuadPart));
    uint64_t got = 0;
    bool ok = true;
    while (got < (uint64_t)out.size()) {
        DWORD want = static_cast<DWORD>(out.size() - got);
        if (want > 1024 * 1024) want = 1024 * 1024;
        DWORD read = 0;
        if (!ReadFile(h, out.data() + got, want, &read, nullptr) || read == 0) { ok = false; break; }
        got += read;
    }
    CloseHandle(h);
    if (!ok) out.clear();
    return ok;
}

bool removeFileSafe(const std::wstring& path) {
    if (path.empty()) return false;
    SetFileAttributesW(path.c_str(), FILE_ATTRIBUTE_NORMAL);
    return DeleteFileW(path.c_str()) == TRUE;
}

bool renameFile(const std::wstring& src, const std::wstring& dst) {
    if (!MoveFileExW(src.c_str(), dst.c_str(), MOVEFILE_REPLACE_EXISTING)) return false;
    return true;
}

FileStamp stampOf(const std::wstring& path) {
    FileStamp st;
    HANDLE h = CreateFileW(path.c_str(), FILE_READ_ATTRIBUTES, FILE_SHARE_READ | FILE_SHARE_WRITE, nullptr,
                           OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (h == INVALID_HANDLE_VALUE) return st;
    BY_HANDLE_FILE_INFORMATION fi;
    if (GetFileInformationByHandle(h, &fi)) {
        st.size = (static_cast<int64_t>(fi.nFileSizeHigh) << 32) | fi.nFileSizeLow;
        st.mtime.LowPart = fi.ftLastWriteTime.dwLowDateTime;
        st.mtime.HighPart = fi.ftLastWriteTime.dwHighDateTime;
        st.ok = true;
    }
    CloseHandle(h);
    return st;
}

int64_t directorySize(const std::wstring& dir) {
    WIN32_FIND_DATAW fd;
    std::wstring pat = dir + L"\\*";
    HANDLE h = FindFirstFileW(pat.c_str(), &fd);
    if (h == INVALID_HANDLE_VALUE) return 0;
    int64_t total = 0;
    do {
        if (wcscmp(fd.cFileName, L".") == 0 || wcscmp(fd.cFileName, L"..") == 0) continue;
        if (fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) {
            std::wstring sub = dir + L"\\";
            sub += fd.cFileName;
            total += directorySize(sub);
        } else {
            total += (static_cast<int64_t>(fd.nFileSizeHigh) << 32) | fd.nFileSizeLow;
        }
    } while (FindNextFileW(h, &fd));
    FindClose(h);
    return total;
}

std::vector<std::wstring> listFiles(const std::wstring& dir, const wchar_t* mask) {
    std::vector<std::wstring> out;
    WIN32_FIND_DATAW fd;
    std::wstring pat = dir + L"\\";
    pat += mask;
    HANDLE h = FindFirstFileW(pat.c_str(), &fd);
    if (h == INVALID_HANDLE_VALUE) return out;
    do {
        if ((fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) == 0) {
            std::wstring n = dir + L"\\";
            n += fd.cFileName;
            out.push_back(n);
        }
    } while (FindNextFileW(h, &fd));
    FindClose(h);
    return out;
}

// ---------------------------------------------------------------------------
// Paths / identity
// ---------------------------------------------------------------------------
std::wstring moduleDir(HMODULE mod) {
    wchar_t buf[MAX_PATH] = {};
    DWORD n = GetModuleFileNameW(mod, buf, MAX_PATH - 1);
    if (n == 0) return L"";
    std::wstring p(buf, n);
    size_t s = p.find_last_of(L'\\');
    return s == std::wstring::npos ? L"" : p.substr(0, s);
}

// Resolve the module that this library code was linked into. bs3d_lib is
// static and ends up in BOTH bs3d_assoc.exe and bs3d_thumb.dll; inside
// explorer.exe a plain GetModuleFileNameW(NULL) would return explorer's own
// path, so we ask GetModuleHandleExW which module contains this function.
static HMODULE selfModule() {
    HMODULE mod = nullptr;
    if (GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS |
                               GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
                           reinterpret_cast<LPCWSTR>(&selfModule), &mod)) {
        return mod;
    }
    return nullptr;
}

std::wstring programDir() { return moduleDir(selfModule()); }

std::wstring localAppDataDir() {
    wchar_t buf[MAX_PATH] = {};
    if (SUCCEEDED(SHGetFolderPathW(nullptr, CSIDL_LOCAL_APPDATA, nullptr, 0, buf)) && buf[0]) {
        return buf;
    }
    return L"";
}

std::wstring programDataDir() {
    wchar_t buf[MAX_PATH] = {};
    std::wstring d;
    if (SHGetFolderPathW(nullptr, CSIDL_COMMON_APPDATA, nullptr, 0, buf) == S_OK && buf[0]) {
        d = buf;
    } else {
        d = L"C:\\ProgramData";
    }
    d += L"\\Bambu3DAssoc";
    return d;
}

std::wstring cacheDir() {
    std::wstring d = programDataDir();
    d += L"\\thumbcache";
    return d;
}

std::wstring shellExtDllPath() {
    std::wstring d = programDir();
    if (d.empty()) return L"bs3d_thumb.dll";
    return d + L"\\bs3d_thumb.dll";
}

bool isElevated() {
    HANDLE t = nullptr;
    if (!OpenProcessToken(GetCurrentProcess(), TOKEN_QUERY, &t) || t == nullptr) return false;
    TOKEN_ELEVATION e = {};
    DWORD need = sizeof(e);
    bool ok = GetTokenInformation(t, TokenElevation, &e, sizeof(e), &need) == TRUE && e.TokenIsElevated != 0;
    CloseHandle(t);
    return ok;
}

bool elevateAndRun(const std::wstring& exePath, const std::wstring& args) {
    SHELLEXECUTEINFOW sei = {};
    sei.cbSize = sizeof(sei);
    sei.lpVerb = L"runas";
    sei.lpFile = exePath.c_str();
    sei.lpParameters = args.c_str();
    sei.nShow = SW_SHOWNORMAL;
    sei.fMask = SEE_MASK_FLAG_NO_UI;
    BOOL r = ShellExecuteExW(&sei);
    return r == TRUE;
}

// ---------------------------------------------------------------------------
// Hashing (CNG)
// ---------------------------------------------------------------------------
std::string sha256Hex(const void* data, size_t size) {
    unsigned char out[32] = {};
    BCRYPT_HASH_HANDLE h = nullptr;
    if (BCryptCreateHash(BCRYPT_SHA256_ALGORITHM, &h, nullptr, 0, nullptr, 0, 0) != 0) return {};
    if (BCryptHashData(h, const_cast<BYTE*>(static_cast<const BYTE*>(data)), static_cast<UINT32>(size), 0) != 0) {
        BCryptDestroyHash(h);
        return {};
    }
    if (BCryptFinishHash(h, out, 32, 0) != 0) { BCryptDestroyHash(h); return {}; }
    BCryptDestroyHash(h);
    std::string s;
    s.reserve(64);
    static const char d[] = "0123456789abcdef";
    for (unsigned char c : out) { s.push_back(d[c >> 4]); s.push_back(d[c & 0xF]); }
    return s;
}

std::string sha256Hex(const std::wstring& text) {
    std::string s;
    s.reserve(text.size() * 2);
    for (wchar_t w : text) {
        const unsigned short u = static_cast<unsigned short>(w);
        s.push_back(static_cast<char>(u & 0xFF));
        s.push_back(static_cast<char>((u >> 8) & 0xFF));
    }
    return sha256Hex(s.data(), s.size());
}

// ---------------------------------------------------------------------------
// Explorer integration
// ---------------------------------------------------------------------------
void notifyAssociationsChanged() {
    SHChangeNotify(SHCNE_ASSOCCHANGED, SHCNF_IDLIST, nullptr, nullptr);
}

bool clearExplorerThumbnailCache(bool restartExplorer) {
    wchar_t buf[MAX_PATH] = {};
    if (SHGetFolderPathW(nullptr, CSIDL_LOCAL_APPDATA, nullptr, 0, buf) != S_OK) return false;
    std::wstring dir = buf;
    dir += L"\\Microsoft\\Windows\\Explorer";
    std::vector<std::wstring> files = listFiles(dir, L"thumbcache_*.db");
    for (const std::wstring& f : files) removeFileSafe(f);
    std::vector<std::wstring> history = listFiles(dir, L"thumbcache_idx_*.db");
    for (const std::wstring& f : history) removeFileSafe(f);
    if (restartExplorer) {
        HANDLE snap = CreateToolhelp32Snapshot(TH32CS_SNAPPROCESS, 0);
        if (snap != INVALID_HANDLE_VALUE) {
            PROCESSENTRY32W pe = {sizeof(pe)};
            if (Process32FirstW(snap, &pe)) {
                do {
                    if (_wcsicmp(pe.szExeFile, L"explorer.exe") == 0) {
                        HANDLE p = OpenProcess(PROCESS_TERMINATE, FALSE, pe.th32ProcessID);
                        if (p) { TerminateProcess(p, 0); CloseHandle(p); }
                    }
                } while (Process32NextW(snap, &pe));
            }
            CloseHandle(snap);
        }
        Sleep(400);
        ShellExecuteW(nullptr, L"open", L"explorer.exe", nullptr, nullptr, SW_SHOWNORMAL);
    }
    return !files.empty();
}

}  // namespace bs3d
