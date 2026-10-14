#pragma once

#include "bs3d.h"

#include <functional>

namespace bs3d {

// ---------------------------------------------------------------------------
// Environment self-check: what is present, what is missing, what blocks
// Explorer thumbnails. Pure detection first; downloading is opt-in.
// ---------------------------------------------------------------------------
enum class EnvLevel : int {
    Ok = 0,      // green
    Warn = 1,    // yellow: works with degraded features
    Error = 2,   // red: something is broken
    Missing = 3, // dependency absent, downloadable
};

enum class EnvAction : int {
    None = 0,
    RepairOcct = 1,          // v1.2.2：内核随安装包自带，这里是“校验并重新注册”，不再下载
    OpenFolderOptions = 2,
    ReRegisterShellExt = 3,
};

struct EnvItem {
    std::wstring id;       // stable identifier: "bambustudio", "occt", ...
    std::wstring name;     // display name (Chinese UI)
    EnvLevel level = EnvLevel::Ok;
    std::wstring status;   // short status text
    std::wstring detail;   // longer explanation / path
    EnvAction action = EnvAction::None;
};

// Full scan (app dependency + OCCT pack + shell components/policies).
std::vector<EnvItem> detectEnvItems();

// ---------------------------------------------------------------------------
// OCCT runtime pack manifest
// ---------------------------------------------------------------------------
struct OccPackSpec {
    std::wstring url;        // https URL of the zip pack
    std::string sha256Hex;   // lowercase hex; empty = skip verification
    std::wstring version;    // informational
    std::wstring fileName;   // suggested zip name, default "occt-pack.zip"
    bool valid() const { return !url.empty(); }
};

// Resolve the pack manifest: <programDir>\occpack.url, then
// <programDataDir>\occpack.url (line 1 = URL, line 2 = optional sha256,
// line 3 = optional version), then the compiled-in default.
OccPackSpec occPackSpec();

// Persist a user-provided override to <programDataDir>\occpack.url.
bool saveOccPackOverride(const OccPackSpec& spec);

struct DownloadProgress {
    int percent = -1;      // -1 = indeterminate
    uint64_t downloaded = 0;
    uint64_t total = 0;
};

// Blocking download + SHA256 verify + zip extraction into occRuntimeDir().
// Call from a worker thread; `progress` may be null. Returns an empty string
// on success, otherwise a user-presentable (Chinese) error message.
std::wstring fetchAndInstallOccPack(
    const OccPackSpec& spec,
    const std::function<void(const DownloadProgress&)>& progress);

// Cooperative cancel for a running fetch (safe to call from the UI thread).
void requestOccDownloadCancel();
bool occDownloadCancelRequested();

}  // namespace bs3d
