#pragma once

#include "bs3d.h"
#include "formats.h"
#include "winutil.h"

namespace bs3d {

// Application identity (kept in one place so the installer and the code agree).
namespace AppConst {
extern const wchar_t* kRootKey;         // "SOFTWARE\\Bambu3DAssoc"
extern const wchar_t* kShellExtSubKey;  // "Bambu3DAssoc\\ShellExt"
extern const wchar_t* kShellExtGuid;    // CLSID of the thumbnail provider
extern const wchar_t* kProgIdPrefix;    // "Bambu3D."
extern const wchar_t* kCompanyName;
extern const wchar_t* kProductName;
extern const wchar_t* kVersion;
}

// IThumbnailProvider handler GUID registered under <ext>\shellex (shared by
// registry.cpp and envcheck.cpp's foreign-handler scanner).
extern const wchar_t* kThumbHandlerGuid;

struct AssocEntry {
    std::wstring ext;       // lower case, no dot
    bool associated = false;
    bool thumbEnabled = false;
    std::wstring handlerName;  // ProgID or "" when not ours
};

// ---- Bambu Studio discovery ------------------------------------------------
std::wstring findBambuStudio(const std::wstring& forcedPath = L"");
std::vector<std::wstring> findBambuCandidates();

// ---- association -----------------------------------------------------------
// Writes HKLM/HKCU\SOFTWARE\Classes\<ext> (+shell\open\command, ProgID,
// OpenWithProgids) for every extension in `exts`. `bambuExe` must exist.
bool applyAssociations(HKEY root, const std::vector<std::wstring>& exts, const std::wstring& bambuExe);
// Restores whatever handler was recorded before we took over (or removes ours).
bool removeAssociations(HKEY root, const std::vector<std::wstring>& exts);
// Per-extension toggle, also used by the manager UI.
bool setExtensionAssociation(HKEY root, const std::wstring& ext, bool enable, const std::wstring& bambuExe);

// ---- thumbnail shell extension ---------------------------------------------
bool setExtensionThumbnail(HKEY root, const std::wstring& ext, bool enable);
bool registerShellExtension(HKEY root, const std::wstring& dllPath, const std::vector<std::wstring>& exts);
bool unregisterShellExtension(HKEY root, const std::vector<std::wstring>& exts);

// ---- state queries ---------------------------------------------------------
std::vector<AssocEntry> queryAll();
AssocEntry queryOne(const std::wstring& ext);
std::wstring readSavedPath(HKEY root);
bool saveBambuPath(HKEY root, const std::wstring& exe);
bool readBoolList(HKEY root, const wchar_t* valueName, std::vector<std::wstring>& out);
bool writeBoolList(HKEY root, const wchar_t* valueName, const std::vector<std::wstring>& on,
                   const std::vector<std::wstring>& off);

// 3MF/AMF thumbnail source mode (REG_DWORD, HKCU\Software\Bambu3DAssoc):
//   1 = prefer embedded preview image, fall back to self-render (default)
//   2 = always self-render, ignore any embedded image
// Read from HKCU so the explorer-shell extension (per-user, non-elevated) can
// see it. Default is 1.
DWORD getThumbMode3mf();
bool setThumbMode3mf(DWORD mode);

// Optional takeover of the file types' default "open with" program. Off by
// default: the product's job is the Explorer thumbnail, and forcing a handler
// both surprises users and can shadow the thumbnail lookup chain.
bool getSetDefaultOpen();
bool setSetDefaultOpen(bool on);

// Which root key should the caller write to? HKLM when elevated, else HKCU.
HKEY pickRootKey(bool forceMachine);

}  // namespace bs3d
