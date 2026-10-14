#include "bs3d/registry.h"
#include "bs3d/formats.h"
#include "bs3d/winutil.h"

#include <algorithm>
#include <set>
#include <shlobj.h>
#include <shlwapi.h>
#pragma comment(lib, "shell32.lib")

namespace bs3d {

// Short aliases for the two class roots we touch.
static const HKEY HKLM = HKEY_LOCAL_MACHINE;
static const HKEY HKCU = HKEY_CURRENT_USER;

// IThumbnailProvider shell-extension handler key. Explorer resolves a file
// type's thumbnail handler by looking this GUID up under the type's ShellEx
// key, so it MUST be the IThumbnailProvider IID. Any other value makes
// Explorer silently skip our DLL: files keep their plain fallback icon.
const wchar_t* kThumbHandlerGuid = L"{E357FCCD-A995-4576-B01F-234630154E96}";

namespace AppConst {
const wchar_t* kRootKey        = L"SOFTWARE\\Bambu3DAssoc";
const wchar_t* kShellExtSubKey = L"Bambu3DAssoc\\ShellExt";
const wchar_t* kShellExtGuid   = L"{E8B5A3F0-2F5E-4C2D-9A34-6C1F8E2B0D77}";
const wchar_t* kProgIdPrefix   = L"Bambu3D.";
const wchar_t* kCompanyName    = L"Bambu3DAssoc";
const wchar_t* kProductName    = L"Bambu 3D File Association & Thumbnail";
const wchar_t* kVersion        = L"1.3.0";
}  // namespace AppConst

namespace {

// Registry paths passed to the winutil helpers are relative to the root we
// picked (HKCU or HKLM). File-type and ProgID keys live under the per-root
// class store, so every such path must carry this prefix.
std::wstring classPath(const std::wstring& sub) {
    return std::wstring(L"SOFTWARE\\Classes\\") + sub;
}

std::wstring progIdOf(const std::wstring& ext) {
    return std::wstring(AppConst::kProgIdPrefix) + ext;
}

std::wstring handlerOf(const std::wstring& ext) {
    return progIdOf(ext);
}

// Every place Explorer may look for a file type's thumbnail handler. Windows
// walks the association chain — first the resolved ProgID (which a third-party
// UserChoice can grab), then the bare extension, then SystemFileAssociations —
// and stops at the first handler it finds. Registering all three keeps our
// provider in play even when another app owns the default handler.
std::vector<std::wstring> thumbHandlerKeys(const std::wstring& ext) {
    return {
        classPath(progIdOf(ext)) + L"\\ShellEx\\" + kThumbHandlerGuid,
        classPath(L"." + ext) + L"\\ShellEx\\" + kThumbHandlerGuid,
        classPath(L"SystemFileAssociations\\." + ext) + L"\\ShellEx\\" +
            kThumbHandlerGuid,
    };
}

// Handler key written by v1.2.0 with a non-standard GUID. Explorer ignored it,
// so it only needs sweeping away whenever we restore or disable the handler.
const wchar_t* kLegacyThumbHandlerGuid = L"{E1775614-BA1F-4B66-85BB-33800A1D334F}";

void removeLegacyThumbKey(HKEY root, const std::wstring& ext) {
    // v1.2.0 wrote it into every shell-lookup location, so sweep all three.
    const std::wstring guid = kLegacyThumbHandlerGuid;
    deleteRegTree(root, classPath(progIdOf(ext)) + L"\\ShellEx\\" + guid);
    deleteRegTree(root, classPath(L"." + ext) + L"\\ShellEx\\" + guid);
    deleteRegTree(root,
                  classPath(L"SystemFileAssociations\\." + ext) + L"\\ShellEx\\" +
                      guid);
}

// Interface entries written for legacy shells. Windows ships its own
// registration for these IIDs under HKLM, so only a per-user (HKCU) copy is
// ever ours to sweep away.
const wchar_t* kThumbIfaceGuids[] = {
    L"{e357fccd-a995-4576-b01f-234630154e96}",
    L"{b824b49d-22ac-4161-ac8a-9916e8fa3f7f}",
};

bool writeShellOpenCommand(HKEY root, const std::wstring& ext, const std::wstring& bambuExe) {
    const std::wstring extPath = classPath(L"." + ext);
    if (!ensureRegKey(root, extPath)) return false;
    if (!setRegString(root, extPath, L"", progIdOf(ext))) return false;

    const std::wstring progKey = classPath(progIdOf(ext));
    if (!ensureRegKey(root, progKey)) return false;
    if (!setRegString(root, progKey, L"FriendlyTypeName",
                      L"Bambu Studio - 3D Print Files")) return false;

    const std::wstring cmdPath = progKey + L"\\shell\\open\\command";
    if (!ensureRegKey(root, cmdPath)) return false;
    if (!setRegString(root, cmdPath, L"", L"\"" + bambuExe + L"\" \"%1\"")) return false;

    return true;
}

bool writeSavedHandler(HKEY root, const std::wstring& ext) {
    // Save the current default handler before we take over, so we can restore.
    const std::wstring extPath = classPath(L"." + ext);
    std::wstring saved;
    getRegString(root, extPath, L"", saved);
    const std::wstring saveKey = extPath + L"\\ShellEx\\SavedHandler";
    // Applying twice must not overwrite the pristine backup with our own ProgID.
    // The backup may legitimately be an *empty* string (extension had no prior
    // handler), so presence of the backup — not its content — is the guard.
    std::wstring existing;
    if (getRegString(root, saveKey, L"", existing) &&
        saved.rfind(AppConst::kProgIdPrefix, 0) == 0) {
        return true;
    }
    setRegString(root, saveKey, L"", saved);
    return true;
}

bool clearSavedHandler(HKEY root, const std::wstring& ext) {
    const std::wstring extPath = classPath(L"." + ext);
    return deleteRegTree(root, extPath + L"\\ShellEx\\SavedHandler");
}

// Undo our takeover of one extension, but only when it is actually ours.
// The default value must either be our own ProgID or have a SavedHandler
// backup we wrote; otherwise the extension belongs to a third party (e.g.
// Visual Studio owning .obj) and we must not touch it.
void restoreOneExtension(HKEY root, const std::wstring& ext) {
    const std::wstring extPath = classPath(L"." + ext);
    std::wstring cur;
    getRegString(root, extPath, L"", cur);
    std::wstring saved;
    const bool ours = (cur.rfind(AppConst::kProgIdPrefix, 0) == 0);
    const bool hadSaved =
        getRegString(root, extPath + L"\\ShellEx\\SavedHandler", L"", saved);
    if (ours || hadSaved) {
        if (hadSaved && !saved.empty()) {
            setRegString(root, extPath, L"", saved);
        } else {
            // Either no backup, or the backup was legitimately empty: the
            // extension had no handler before us.
            deleteRegValue(root, extPath, L"");
        }
        deleteRegTree(root, extPath + L"\\ShellEx\\SavedHandler");
    }
    deleteRegTree(root, classPath(progIdOf(ext)));
    for (const auto& key : thumbHandlerKeys(ext)) deleteRegTree(root, key);
    removeLegacyThumbKey(root, ext);
}

// The merged HKCR view resolves a default handler per path with HKCU winning
// over HKLM, so a per-user registration (e.g. Bambu Studio writing
// " Bambu.Studio.1" under HKCU\Software\Classes) shadows a machine-level
// takeover and makes it a no-op for the current user. Whenever we take over
// or restore at HKLM, mirror the same change into the current user's class
// root, keeping HKCU's own SavedHandler backup so restore paths (including
// the uninstaller's, which loops both roots) undo each hive faithfully.
void mirrorTakeoverToUser(const std::wstring& ext) {
    writeSavedHandler(HKCU, ext);
    const std::wstring extPath = classPath(L"." + ext);
    if (ensureRegKey(HKCU, extPath)) {
        setRegString(HKCU, extPath, L"", progIdOf(ext));
    }
}

// True when a path's file name is a Bambu Studio executable: matches
// "BambuStudio.exe" and the newer launcher name "bambu-studio.exe". Used to
// filter unrelated registry entries (e.g. any app's DisplayIcon) so we never
// associate files with the wrong program.
bool isBambuExeName(const std::wstring& path) {
    size_t sep = path.find_last_of(L"\\/");
    std::wstring name = (sep == std::wstring::npos) ? path : path.substr(sep + 1);
    name = lowerW(name);
    std::wstring norm;
    for (wchar_t c : name) {
        if (c != L'-' && c != L'_' && c != L' ') norm.push_back(c);
    }
    return norm.find(L"bambustudio") != std::wstring::npos;
}

// DisplayIcon values sometimes carry an icon index suffix ("app.exe,0").
std::wstring stripIconIndex(const std::wstring& path) {
    size_t comma = path.find_last_of(L',');
    if (comma == std::wstring::npos || comma + 1 >= path.size()) return path;
    for (size_t i = comma + 1; i < path.size(); ++i) {
        if (path[i] < L'0' || path[i] > L'9') return path;
    }
    return path.substr(0, comma);
}

}  // namespace

// ===========================================================================
// Bambu Studio discovery
// ===========================================================================
std::wstring findBambuStudio(const std::wstring& forcedPath) {
    if (!forcedPath.empty() && fileExists(forcedPath) && isBambuExeName(forcedPath))
        return forcedPath;

    std::vector<std::wstring> candidates = findBambuCandidates();
    for (const auto& p : candidates) {
        if (fileExists(p)) return p;
    }
    return L"";
}

std::vector<std::wstring> findBambuCandidates() {
    std::vector<std::wstring> out;
    auto pushUnique = [&out](const std::wstring& s) {
        if (s.empty()) return;
        std::wstring lower = lowerW(s);
        for (const auto& existing : out) {
            if (lowerW(existing) == lower) return;
        }
        out.push_back(s);
    };

    // 1) Standard locations, both the classic exe name and the 2.x launcher.
    wchar_t localAppData[MAX_PATH] = {0};
    if (SUCCEEDED(SHGetFolderPathW(nullptr, CSIDL_APPDATA, nullptr, 0, localAppData))) {
        pushUnique(std::wstring(localAppData) + L"\\BambuStudio\\studio64\\BambuStudio.exe");
        pushUnique(std::wstring(localAppData) + L"\\Bambu Studio\\BambuStudio.exe");
    }
    wchar_t localLow[MAX_PATH] = {0};
    if (SUCCEEDED(SHGetSpecialFolderPathW(nullptr, localLow, CSIDL_LOCAL_APPDATA, FALSE))) {
        pushUnique(std::wstring(localLow) + L"\\BambuStudio\\studio64\\BambuStudio.exe");
        pushUnique(std::wstring(localLow) + L"\\Bambu Studio\\BambuStudio.exe");
        pushUnique(std::wstring(localLow) + L"\\BambuStudio\\BambuStudio.exe");
    }
    for (int csidl : {CSIDL_PROGRAM_FILES, CSIDL_PROGRAM_FILESX86}) {
        wchar_t pf[MAX_PATH] = {0};
        if (FAILED(SHGetFolderPathW(nullptr, csidl, nullptr, 0, pf))) continue;
        pushUnique(std::wstring(pf) + L"\\Bambu Studio\\BambuStudio.exe");
        pushUnique(std::wstring(pf) + L"\\Bambu Studio\\bambu-studio.exe");
        pushUnique(std::wstring(pf) + L"\\BambuStudio\\studio64\\BambuStudio.exe");
    }

    // 2) HKLM / HKCU Uninstall entries
    HKEY roots[2] = {HKLM, HKCU};
    // Scan both registry views: 64-bit apps (e.g. Bambu Studio) register
    // their uninstall entries outside WOW6432Node, 32-bit apps inside it.
    const DWORD views[2] = {KEY_WOW64_64KEY, KEY_WOW64_32KEY};
    for (HKEY root : roots) {
      for (DWORD view : views) {
        HKEY base = nullptr;
        LSTATUS st = RegOpenKeyExW(root, L"SOFTWARE\\Microsoft\\Windows\\CurrentVersion\\Uninstall",
                                   0, KEY_READ | view, &base);
        if (st != ERROR_SUCCESS) continue;
        for (DWORD i = 0; ; ++i) {
            wchar_t sub[256];
            DWORD n = sizeof(sub) / sizeof(wchar_t);
            LSTATUS r = RegEnumKeyExW(base, i, sub, &n, nullptr, nullptr, nullptr, nullptr);
            if (r != ERROR_SUCCESS) break;
            HKEY hk = nullptr;
            if (RegOpenKeyExW(base, sub, 0, KEY_READ, &hk) != ERROR_SUCCESS) continue;
            std::wstring installLoc, displayIcon;
            wchar_t buf[MAX_PATH];
            DWORD cb;
            cb = sizeof(buf); if (RegQueryValueExW(hk, L"InstallLocation", nullptr, nullptr,
                                                    (LPBYTE)buf, &cb) == ERROR_SUCCESS)
                installLoc = buf;
            cb = sizeof(buf); if (RegQueryValueExW(hk, L"DisplayIcon", nullptr, nullptr,
                                                    (LPBYTE)buf, &cb) == ERROR_SUCCESS)
                displayIcon = buf;
            RegCloseKey(hk);
            // Only trust uninstall entries that actually belong to Bambu
            // Studio; any other app's InstallLocation/DisplayIcon must not
            // become an association target (e.g. unrelated apps registering
            // their exe as DisplayIcon).
            const bool bambuEntry =
                lowerW(displayIcon).find(L"bambu") != std::wstring::npos ||
                lowerW(installLoc).find(L"bambu") != std::wstring::npos ||
                lowerW(sub).find(L"bambu") != std::wstring::npos;
            if (bambuEntry && !installLoc.empty()) {
                pushUnique(installLoc + L"\\BambuStudio.exe");
                pushUnique(installLoc + L"\\studio64\\BambuStudio.exe");
                pushUnique(installLoc + L"\\bambu-studio.exe");
            }
            if (!displayIcon.empty()) {
                std::wstring icon = stripIconIndex(displayIcon);
                if (isBambuExeName(icon)) pushUnique(icon);
            }
        }
        RegCloseKey(base);
      }
    }

    // 3) PATH search
    wchar_t pathEnv[8192] = {0};
    DWORD pn = GetEnvironmentVariableW(L"PATH", pathEnv, sizeof(pathEnv) / sizeof(wchar_t));
    if (pn > 0 && pn < sizeof(pathEnv) / sizeof(wchar_t)) {
        std::wstring envPath(pathEnv);
        for (const auto& dir : splitW(envPath, L';')) {
            if (dir.empty()) continue;
            pushUnique(dir + L"\\BambuStudio.exe");
        }
    }

    return out;
}

// ===========================================================================
// Associations
// ===========================================================================
bool applyAssociations(HKEY root, const std::vector<std::wstring>& exts,
                       const std::wstring& bambuExe) {
    if (bambuExe.empty() || !fileExists(bambuExe)) return false;
    bool ok = true;
    for (const auto& ext : exts) {
        writeSavedHandler(root, ext);
        if (!writeShellOpenCommand(root, ext, bambuExe)) ok = false;
        if (root == HKLM) mirrorTakeoverToUser(ext);
    }
    return ok;
}

bool removeAssociations(HKEY root, const std::vector<std::wstring>& exts) {
    bool ok = true;
    for (const auto& ext : exts) {
        restoreOneExtension(root, ext);
        if (root == HKLM) restoreOneExtension(HKCU, ext);
    }
    return ok;
}

bool setExtensionAssociation(HKEY root, const std::wstring& ext, bool enable,
                             const std::wstring& bambuExe) {
    if (enable) {
        writeSavedHandler(root, ext);
        const bool ok = writeShellOpenCommand(root, ext, bambuExe);
        if (root == HKLM) mirrorTakeoverToUser(ext);
        return ok;
    } else {
        restoreOneExtension(root, ext);
        if (root == HKLM) restoreOneExtension(HKCU, ext);
        return true;
    }
}

// ===========================================================================
// Shell extension
// ===========================================================================
bool setExtensionThumbnail(HKEY root, const std::wstring& ext, bool enable) {
    // Mirror into every shell-lookup location so the handler survives a
    // third-party takeover of the default handler (see thumbHandlerKeys).
    bool ok = true;
    for (const auto& key : thumbHandlerKeys(ext)) {
        if (enable) {
            if (!ensureRegKey(root, key) ||
                !setRegString(root, key, L"", AppConst::kShellExtGuid)) {
                ok = false;
            }
        } else if (!deleteRegTree(root, key)) {
            ok = false;
        }
    }
    // Sweep the wrong-GUID cruft from v1.2.0 in both directions: Explorer
    // never looked it up, but it must not survive an apply either.
    removeLegacyThumbKey(root, ext);
    if (root == HKLM) removeLegacyThumbKey(HKCU, ext);
    return ok;
}

bool registerShellExtension(HKEY root, const std::wstring& dllPath,
                            const std::vector<std::wstring>& exts) {
    if (dllPath.empty()) return false;

    // COM class registration
    const std::wstring clsidKeyReal =
        std::wstring(L"SOFTWARE\\Classes\\CLSID\\") + AppConst::kShellExtGuid;
    if (!ensureRegKey(root, clsidKeyReal)) return false;
    setRegString(root, clsidKeyReal, L"", L"Bambu3D Thumbnail Provider");
    setRegString(root, clsidKeyReal + L"\\InprocServer32", L"", dllPath);
    setRegString(root, clsidKeyReal + L"\\InprocServer32", L"ThreadingModel",
                 L"Both");

    // Interface entries (some shells want these)
    for (const wchar_t* ifc : kThumbIfaceGuids) {
        const std::wstring key =
            std::wstring(L"SOFTWARE\\Classes\\Interface\\") + ifc;
        ensureRegKey(root, key);
        setRegString(root, key, L"ProxyStubClsid32",
                     L"{00020424-0000-0000-C000-000000000046}");
    }

    // Apply to selected extensions
    bool ok = true;
    for (const auto& ext : exts) {
        if (!setExtensionThumbnail(root, ext, true)) ok = false;
    }
    return ok;
}

bool unregisterShellExtension(HKEY root, const std::vector<std::wstring>& exts) {
    const std::wstring clsidKeyReal =
        std::wstring(L"SOFTWARE\\Classes\\CLSID\\") + AppConst::kShellExtGuid;
    deleteRegTree(root, clsidKeyReal);
    // A per-user Interface entry is ours alone; the HKLM one belongs to Windows
    // (standard ProxyStubClsid32 registration for the shell IIDs), so it stays.
    if (root == HKCU) {
        for (const wchar_t* ifc : kThumbIfaceGuids) {
            deleteRegTree(root, std::wstring(L"SOFTWARE\\Classes\\Interface\\") + ifc);
        }
    }
    bool ok = true;
    for (const auto& ext : exts) {
        if (!setExtensionThumbnail(root, ext, false)) ok = false;
    }
    return ok;
}

// ===========================================================================
// State queries
// ===========================================================================
std::vector<AssocEntry> queryAll() {
    std::vector<AssocEntry> out;
    const auto& all = allFormats();
    for (const auto* f : all) {
        AssocEntry e = queryOne(f->ext);
        out.push_back(e);
    }
    return out;
}

AssocEntry queryOne(const std::wstring& ext) {
    AssocEntry e;
    e.ext = lowerW(ext);
    if (e.ext.empty()) return e;
    const std::wstring extPath = classPath(L"." + e.ext);
    std::wstring defaultHandler;
    if (getRegString(HKLM, extPath, L"", defaultHandler)) {
        if (defaultHandler.empty()) {
            getRegString(HKCU, extPath, L"", defaultHandler);
        }
    }
    e.handlerName = defaultHandler;
    e.associated = !defaultHandler.empty() && defaultHandler == progIdOf(e.ext);
    // Enabled when any shell-lookup location maps the handler key to our CLSID
    // (see thumbHandlerKeys) in either root.
    e.thumbEnabled = false;
    for (const auto& key : thumbHandlerKeys(e.ext)) {
        std::wstring guid;
        bool have = getRegString(HKLM, key, L"", guid);
        if (!have) have = getRegString(HKCU, key, L"", guid);
        if (have && lowerW(guid) == lowerW(AppConst::kShellExtGuid)) {
            e.thumbEnabled = true;
            break;
        }
    }
    return e;
}

std::wstring readSavedPath(HKEY root) {
    std::wstring s;
    getRegString(root, AppConst::kRootKey, L"BambuExe", s);
    return s;
}

bool saveBambuPath(HKEY root, const std::wstring& exe) {
    if (!ensureRegKey(root, AppConst::kRootKey)) return false;
    return setRegString(root, AppConst::kRootKey, L"BambuExe", exe);
}

bool readBoolList(HKEY root, const wchar_t* valueName, std::vector<std::wstring>& out) {
    out.clear();
    std::wstring onList, offList;
    getRegString(root, AppConst::kRootKey, valueName, onList);
    if (onList.empty()) return false;
    for (const auto& s : splitW(onList, L';')) {
        if (!s.empty()) out.push_back(s);
    }
    return !out.empty();
}

bool writeBoolList(HKEY root, const wchar_t* valueName,
                   const std::vector<std::wstring>& on,
                   const std::vector<std::wstring>& off) {
    std::wstring s;
    for (size_t i = 0; i < on.size(); ++i) {
        if (i) s.push_back(L';');
        s += on[i];
    }
    if (!ensureRegKey(root, AppConst::kRootKey)) return false;
    setRegString(root, AppConst::kRootKey, valueName, s);
    (void)off;
    return true;
}

DWORD getThumbMode3mf() {
    DWORD mode = 1;
    getRegDWORD(HKCU, AppConst::kRootKey, L"ThumbMode3mf", mode);
    // Only 1 (embedded-preferred) and 2 (always self-render) are valid.
    if (mode != 1 && mode != 2) mode = 1;
    return mode;
}

bool setThumbMode3mf(DWORD mode) {
    if (mode != 1 && mode != 2) mode = 1;
    return setRegDWORD(HKCU, AppConst::kRootKey, L"ThumbMode3mf", mode);
}

bool getSetDefaultOpen() {
    DWORD v = 0;
    getRegDWORD(HKCU, AppConst::kRootKey, L"SetDefaultOpen", v);
    return v != 0;
}

bool setSetDefaultOpen(bool on) {
    return setRegDWORD(HKCU, AppConst::kRootKey, L"SetDefaultOpen", on ? 1 : 0);
}

HKEY pickRootKey(bool forceMachine) {
    if (forceMachine && isElevated()) return HKLM;
    if (!forceMachine) return HKCU;
    // Force machine but not elevated: caller must have elevated
    return HKCU;
}

}  // namespace bs3d
