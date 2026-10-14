#pragma once

#include "bs3d.h"

namespace bs3d {

// ---- registry --------------------------------------------------------------
bool ensureRegKey(HKEY root, const std::wstring& subKey);
bool setRegString(HKEY root, const std::wstring& subKey, const std::wstring& valueName, const std::wstring& data);
bool setRegDWORD(HKEY root, const std::wstring& subKey, const std::wstring& valueName, DWORD value);
bool getRegString(HKEY root, const std::wstring& subKey, const std::wstring& valueName, std::wstring& out);
bool getRegDWORD(HKEY root, const std::wstring& subKey, const std::wstring& valueName, DWORD& out);
bool deleteRegValue(HKEY root, const std::wstring& subKey, const std::wstring& valueName);
bool deleteRegTree(HKEY root, const std::wstring& subKey);
bool keyExists(HKEY root, const std::wstring& subKey);

// ---- files / paths ---------------------------------------------------------
bool fileExists(const std::wstring& path);
bool ensureDirectory(const std::wstring& dir);
bool readFileBytes(const std::wstring& path, std::vector<Byte>& out, uint64_t maxBytes);
bool writeFileBytes(const std::wstring& path, const void* data, size_t size);
bool removeFileSafe(const std::wstring& path);
bool renameFile(const std::wstring& src, const std::wstring& dst);

struct FileStamp {
    int64_t size = 0;
    ULARGE_INTEGER mtime = {};
    bool ok = false;
};
FileStamp stampOf(const std::wstring& path);

std::wstring programDataDir();     // %ProgramData%\Bambu3DAssoc
std::wstring cacheDir();           // <programDataDir>\thumbcache
std::wstring programDir();         // directory containing THIS module (exe or DLL)
std::wstring localAppDataDir();    // %LOCALAPPDATA%, empty on failure
std::wstring shellExtDllPath();    // absolute path of the thumbnail provider DLL

bool isElevated();
bool elevateAndRun(const std::wstring& exePath, const std::wstring& args);

std::string sha256Hex(const void* data, size_t size);
std::string sha256Hex(const std::wstring& text);

// ---- explorer helpers ------------------------------------------------------
void notifyAssociationsChanged();
bool clearExplorerThumbnailCache(bool restartExplorer);

// ---- string helpers --------------------------------------------------------
std::wstring lowerW(const std::wstring& s);
std::wstring trimW(const std::wstring& s);
std::vector<std::wstring> splitW(const std::wstring& s, wchar_t sep);
bool startsWith(const std::wstring& s, const std::wstring& prefix, bool caseInsensitive = true);

// ---- misc ------------------------------------------------------------------
int64_t directorySize(const std::wstring& dir);
std::vector<std::wstring> listFiles(const std::wstring& dir, const wchar_t* mask);

}  // namespace bs3d
