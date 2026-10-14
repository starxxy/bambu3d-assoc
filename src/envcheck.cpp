#include "bs3d/envcheck.h"
#include "bs3d/occplugin.h"
#include "bs3d/registry.h"
#include "bs3d/winutil.h"
#include "bs3d/zipread.h"

#include <windows.h>
#include <winhttp.h>

#pragma comment(lib, "winhttp.lib")

#include <atomic>
#include <cctype>
#include <cstdio>
#include <cstdlib>
#include <cwchar>

namespace bs3d {
namespace {

std::atomic<bool> g_cancel{false};

// RAII wrapper for HINTERNET handles returned by WinHttp* open calls.
struct SafeHINTERNET {
    HINTERNET h = nullptr;
    SafeHINTERNET() = default;
    explicit SafeHINTERNET(HINTERNET handle) : h(handle) {}
    SafeHINTERNET(const SafeHINTERNET&) = delete;
    SafeHINTERNET& operator=(const SafeHINTERNET&) = delete;
    ~SafeHINTERNET() { if (h) WinHttpCloseHandle(h); }
    explicit operator bool() const { return h != nullptr; }
    HINTERNET get() const { return h; }
};

// ---- encoding helpers ------------------------------------------------------
std::wstring utf8ToWide(const std::string& s) {
    if (s.empty()) return L"";
    int n = MultiByteToWideChar(CP_UTF8, 0, s.data(), (int)s.size(), nullptr, 0);
    std::wstring w(static_cast<size_t>(n > 0 ? n : 0), L'\0');
    if (n > 0) MultiByteToWideChar(CP_UTF8, 0, s.data(), (int)s.size(), &w[0], n);
    return w;
}

std::string wideToUtf8(const std::wstring& s) {
    if (s.empty()) return std::string();
    int n = WideCharToMultiByte(CP_UTF8, 0, s.data(), (int)s.size(), nullptr, 0, nullptr, nullptr);
    std::string a(static_cast<size_t>(n > 0 ? n : 0), '\0');
    if (n > 0) {
        WideCharToMultiByte(CP_UTF8, 0, s.data(), (int)s.size(), &a[0], n, nullptr, nullptr);
    }
    return a;
}

std::string lowerA(std::string s) {
    for (auto& c : s) c = static_cast<char>(tolower(static_cast<unsigned char>(c)));
    return s;
}

EnvItem item(const wchar_t* id, const wchar_t* name, EnvLevel level,
             const std::wstring& status, const std::wstring& detail,
             EnvAction action = EnvAction::None) {
    EnvItem e;
    e.id = id;
    e.name = name;
    e.level = level;
    e.status = status;
    e.detail = detail;
    e.action = action;
    return e;
}

bool DWORD1(HKEY root, const wchar_t* sub, const wchar_t* value) {
    DWORD v = 0;
    return getRegDWORD(root, sub, value, v) && v == 1;
}

const wchar_t* kAdvancedSub = L"Software\\Microsoft\\Windows\\CurrentVersion\\Explorer\\Advanced";
const wchar_t* kPoliciesSub = L"Software\\Microsoft\\Windows\\CurrentVersion\\Policies\\Explorer";

// ---- shell / policy checks -------------------------------------------------
void addForeignShellexChecks(std::vector<EnvItem>& items);  // defined below
void addShellChecks(std::vector<EnvItem>& items) {
    wchar_t sys[MAX_PATH] = {};
    if (GetSystemWindowsDirectoryW(sys, MAX_PATH - 1) == 0) wcscpy(sys, L"C:\\Windows");

    // 1) Windows thumbnail cache subsystem
    std::wstring tc = std::wstring(sys) + L"\\System32\\thumbcache.dll";
    if (fileExists(tc)) {
        items.push_back(item(L"thumbcache", L"系统缩略图组件", EnvLevel::Ok, L"正常", tc));
    } else {
        items.push_back(item(L"thumbcache", L"系统缩略图组件", EnvLevel::Error, L"缺失",
                             L"未找到 thumbcache.dll，资源管理器无法显示任何缩略图。可在 设置>更新 中修复系统文件，或运行 sfc /scannow。"));
    }

    // 2) "Always show icons, never thumbnails" (folder option / IconsOnly)
    if (DWORD1(HKEY_CURRENT_USER, kAdvancedSub, L"IconsOnly")) {
        items.push_back(item(L"iconsonly", L"文件夹选项", EnvLevel::Warn, L"已关闭缩略图",
                             L"当前设置为“始终显示图标，从不显示缩略图”，请取消该选项。",
                             EnvAction::OpenFolderOptions));
    } else {
        items.push_back(item(L"iconsonly", L"文件夹选项", EnvLevel::Ok, L"显示缩略图",
                             L"未启用“始终显示图标”限制。"));
    }

    // 3) Group-policy style restrictions (HKCU then HKLM)
    bool polThumb = DWORD1(HKEY_CURRENT_USER, kPoliciesSub, L"DisableThumbnails") ||
                    DWORD1(HKEY_LOCAL_MACHINE, kPoliciesSub, L"DisableThumbnails");
    if (polThumb) {
        items.push_back(item(L"pol-disable", L"缩略图策略", EnvLevel::Error, L"被策略禁用",
                             L"注册表策略 DisableThumbnails=1 禁用了全部缩略图（常见于企业安全策略），需要管理员修改组策略后重启资源管理器。"));
    }
    bool polLegacy = DWORD1(HKEY_CURRENT_USER, kPoliciesSub, L"LegacyDisableThumbnailCache") ||
                     DWORD1(HKEY_LOCAL_MACHINE, kPoliciesSub, L"LegacyDisableThumbnailCache");
    if (polLegacy) {
        items.push_back(item(L"pol-legacy", L"缩略图缓存策略", EnvLevel::Warn, L"缓存被禁用",
                             L"LegacyDisableThumbnailCache=1 会绕过缩略图缓存，拖慢显示并可能隐藏本扩展生成的预览。"));
    }
    bool polServer = DWORD1(HKEY_CURRENT_USER, kPoliciesSub, L"DisableThumbnailsInServer") ||
                     DWORD1(HKEY_LOCAL_MACHINE, kPoliciesSub, L"DisableThumbnailsInServer");
    if (polServer) {
        items.push_back(item(L"pol-server", L"服务端缩略图策略", EnvLevel::Warn, L"部分受限",
                             L"DisableThumbnailsInServer=1：网络共享位置（\\\\server\\share）中的文件不会显示缩略图。"));
    }
    if (!polThumb && !polLegacy && !polServer) {
        items.push_back(item(L"pol-ok", L"缩略图策略", EnvLevel::Ok, L"无限制",
                             L"未发现禁用缩略图的注册表策略。"));
    }

    // 4) Our COM registration
    std::wstring clsidKey = std::wstring(L"SOFTWARE\\Classes\\CLSID\\") + AppConst::kShellExtGuid;
    std::wstring server;
    bool regHKLM = getRegString(HKEY_LOCAL_MACHINE, (clsidKey + L"\\InProcServer32").c_str(), L"", server);
    bool regHKCU = !regHKLM && getRegString(HKEY_CURRENT_USER, (clsidKey + L"\\InProcServer32").c_str(), L"", server);
    std::wstring want = shellExtDllPath();
    if ((regHKLM || regHKCU) && lowerW(server) == lowerW(want)) {
        items.push_back(item(L"reg", L"缩略图扩展注册", EnvLevel::Ok, L"已注册",
                             server + (regHKLM ? L"（本机）" : L"（当前用户）")));
    } else if (regHKLM || regHKCU) {
        items.push_back(item(L"reg", L"缩略图扩展注册", EnvLevel::Warn, L"路径不一致",
                             L"注册指向 " + server + L"，当前程序位于 " + want + L"。点击主界面“应用”重新注册。"));
    } else {
        items.push_back(item(L"reg", L"缩略图扩展注册", EnvLevel::Missing, L"未注册",
                             L"缩略图扩展尚未注册，请在主界面勾选格式并点击“应用”。"));
    }

    addForeignShellexChecks(items);
}

// ---- foreign thumbnail handler detection ----------------------------------
// Scan each thumbnailable extension's shellex key for a handler CLSID that is
// not ours.  F3D and other mesh viewers register the same IThumbnailProvider
// handler GUID under <ext>\shellex and would shadow our thumbnail provider.
void addForeignShellexChecks(std::vector<EnvItem>& items) {
    std::wstring ourGuid = AppConst::kShellExtGuid;
    std::wstring foreignClsid;
    std::wstring affectedExts;
    int foreignCount = 0;

    for (const auto* fmt : allFormats()) {
        if (!fmt->thumbnailable) continue;
        std::wstring sub = std::wstring(L"SOFTWARE\\Classes\\.") + fmt->ext +
                           L"\\shellex\\" + kThumbHandlerGuid;
        std::wstring val;
        bool isForeign = false;
        if (getRegString(HKEY_LOCAL_MACHINE, sub, L"", val) && !val.empty()) {
            if (lowerW(val) != lowerW(ourGuid)) isForeign = true;
        } else if (getRegString(HKEY_CURRENT_USER, sub, L"", val) && !val.empty()) {
            if (lowerW(val) != lowerW(ourGuid)) isForeign = true;
        }
        if (isForeign) {
            if (foreignCount == 0) foreignClsid = val;
            if (!affectedExts.empty()) affectedExts.push_back(L'/');
            affectedExts += fmt->ext;
            ++foreignCount;
        }
    }

    if (foreignCount > 0) {
        std::wstring detail = L"检测到 " + std::to_wstring(foreignCount) +
            L" 个扩展名（." + affectedExts +
            L"）的缩略图处理器被第三方占用（CLSID " + foreignClsid +
            L"，可能为 F3D / 其他查看器），将覆盖本软件的缩略图。点击「重新注册」可恢复。";
        items.push_back(item(L"foreign-shellex", L"第三方缩略图处理器", EnvLevel::Warn,
                             L"存在冲突", detail, EnvAction::ReRegisterShellExt));
    }
}

// ---- OCCT pack manifest ----------------------------------------------------
bool parsePackFile(const std::wstring& file, OccPackSpec& spec) {
    std::vector<Byte> raw;
    if (!fileExists(file)) return false;
    if (!readFileBytes(file, raw, 64ULL << 10)) return false;
    // split raw bytes into lines
    std::vector<std::string> lines;
    {
        std::string cur;
        for (Byte b : raw) {
            if (b == '\n') { lines.push_back(cur); cur.clear(); }
            else if (b != '\r' && b != '\xEF' && b != '\xBB' && b != '\xBF') cur.push_back(static_cast<char>(b));
        }
        if (!cur.empty()) lines.push_back(cur);
    }
    auto trimA = [](std::string s) {
        size_t a = s.find_first_not_of(" \t\r\n");
        if (a == std::string::npos) return std::string();
        size_t b = s.find_last_not_of(" \t\r\n");
        return s.substr(a, b - a + 1);
    };
    size_t idx = 0;
    if (idx < lines.size()) { spec.url = trimW(utf8ToWide(trimA(lines[idx++]))); }
    if (idx < lines.size()) { spec.sha256Hex = trimA(lines[idx++]); }
    if (idx < lines.size()) { spec.version = trimW(utf8ToWide(trimA(lines[idx++]))); }
    return spec.valid();
}

}  // namespace

// ---------------------------------------------------------------------------
std::vector<EnvItem> detectEnvItems() {
    std::vector<EnvItem> items;

    // Bambu Studio handler
    std::wstring bs = findBambuStudio();
    if (bs.empty()) {
        items.push_back(item(L"bambustudio", L"Bambu Studio", EnvLevel::Error, L"未找到",
                             L"未检测到 Bambu Studio，双击打开 3D 文件将没有默认程序。请从 bambu3d.com 安装后重新扫描。"));
    } else {
        items.push_back(item(L"bambustudio", L"Bambu Studio", EnvLevel::Ok, L"已找到", bs));
    }

    // OCCT runtime pack (STEP real tessellation)。v1.2.2 起内核随安装包自带，
    // 这里只做存在性检查，不再提供下载。
    if (occPackInstalled()) {
        items.push_back(item(L"occt", L"CAD 内核 (OCCT)", EnvLevel::Ok, L"已就绪",
                             L"随安装包自带，STEP / IGES 可直接渲染。"));
    } else {
        items.push_back(item(L"occt", L"CAD 内核 (OCCT)", EnvLevel::Missing, L"缺失",
                             L"未找到 bs3d_occ.dll：请重新运行安装程序修复（内核随安装包自带）。",
                             EnvAction::RepairOcct));
    }

    addShellChecks(items);
    return items;
}

OccPackSpec occPackSpec() {
    OccPackSpec spec;
    // Compiled-in default: publish the pack (tools/build_occt_pack.ps1) and
    // set the release URL here before shipping, or distribute occpack.url.
    spec.url = L"";
    spec.version = L"";

    std::wstring f1 = programDir() + L"\\occpack.url";
    std::wstring f2 = programDataDir() + L"\\occpack.url";
    if (parsePackFile(f1, spec)) return spec;
    OccPackSpec s2;
    if (parsePackFile(f2, s2)) return s2;
    return spec;
}

bool saveOccPackOverride(const OccPackSpec& spec) {
    std::wstring dir = programDataDir();
    if (!ensureDirectory(dir)) return false;
    std::string body = wideToUtf8(spec.url) + "\n" + spec.sha256Hex + "\n" + wideToUtf8(spec.version) + "\n";
    return writeFileBytes(dir + L"\\occpack.url", body.data(), body.size());
}

void requestOccDownloadCancel() { g_cancel.store(true); }
bool occDownloadCancelRequested() { return g_cancel.load(); }

// ---------------------------------------------------------------------------
std::wstring fetchAndInstallOccPack(
    const OccPackSpec& spec,
    const std::function<void(const DownloadProgress&)>& progress) {
    g_cancel.store(false);
    if (!spec.valid()) return L"未配置下载地址：请先在“下载地址”栏填入 OCCT 插件包 URL。";

    URL_COMPONENTS uc = {};
    uc.dwStructSize = sizeof(uc);
    wchar_t host[256] = {}; wchar_t pathBuf[2048] = {}; wchar_t extra[1024] = {};
    uc.lpszHostName = host; uc.dwHostNameLength = 255;
    uc.lpszUrlPath = pathBuf; uc.dwUrlPathLength = 2047;
    uc.lpszExtraInfo = extra; uc.dwExtraInfoLength = 1023;
    if (!WinHttpCrackUrl(spec.url.c_str(), 0, 0, &uc)) return L"下载地址格式不正确。";
    if (uc.nScheme != INTERNET_SCHEME_HTTPS && uc.nScheme != INTERNET_SCHEME_HTTP)
        return L"仅支持 http/https 下载地址。";

    SafeHINTERNET session(WinHttpOpen(L"Bambu3DAssoc/1.0", WINHTTP_ACCESS_TYPE_DEFAULT_PROXY,
                                       WINHTTP_NO_PROXY_NAME, WINHTTP_NO_PROXY_BYPASS, 0));
    if (!session) return L"无法初始化下载组件（WinHTTP）。";
    WinHttpSetTimeouts(session.get(), 15000, 15000, 30000, 300000);
    DWORD protocol = WINHTTP_FLAG_SECURE_PROTOCOL_TLS1_2 | WINHTTP_FLAG_SECURE_PROTOCOL_TLS1_3;
    WinHttpSetOption(session.get(), WINHTTP_OPTION_SECURE_PROTOCOLS, &protocol, sizeof(protocol));

    SafeHINTERNET connect(WinHttpConnect(session.get(), host, uc.nPort, 0));
    if (!connect) return L"无法连接服务器。";
    std::wstring object = pathBuf;
    object += extra;
    SafeHINTERNET request(WinHttpOpenRequest(connect.get(), L"GET", object.c_str(), nullptr,
                                             WINHTTP_NO_REFERER, WINHTTP_DEFAULT_ACCEPT_TYPES,
                                             uc.nScheme == INTERNET_SCHEME_HTTPS ? WINHTTP_FLAG_SECURE : 0));
    if (!request) return L"无法创建下载请求。";
    DWORD redir = WINHTTP_OPTION_REDIRECT_POLICY_ALWAYS;
    WinHttpSetOption(request.get(), WINHTTP_OPTION_REDIRECT_POLICY, &redir, sizeof(redir));

    if (!WinHttpSendRequest(request.get(), WINHTTP_NO_ADDITIONAL_HEADERS, 0,
                            WINHTTP_NO_REQUEST_DATA, 0, 0, 0)) {
        DWORD e = GetLastError();
        wchar_t msg[128]; swprintf(msg, 128, L"发送请求失败（错误 %lu），请检查网络或代理。", e);
        return msg;
    }
    if (!WinHttpReceiveResponse(request.get(), nullptr)) {
        DWORD e = GetLastError();
        wchar_t msg[128]; swprintf(msg, 128, L"服务器无响应（错误 %lu）。", e);
        return msg;
    }
    wchar_t statusBuf[8] = {}; DWORD statusLen = 0;
    if (WinHttpQueryHeaders(request.get(), WINHTTP_QUERY_STATUS_CODE | WINHTTP_QUERY_FLAG_NUMBER,
                            WINHTTP_HEADER_NAME_BY_INDEX, statusBuf, &statusLen, WINHTTP_NO_HEADER_INDEX)) {
        // statusBuf holds the numeric status as wide chars? FLAG_NUMBER puts DWORD there:
        DWORD status = *reinterpret_cast<DWORD*>(statusBuf);
        if (status < 200 || status >= 300) {
            wchar_t msg[128]; swprintf(msg, 128, L"服务器返回 HTTP %lu，地址可能已失效。", status);
            return msg;
        }
    }
    uint64_t total = 0;
    {
        wchar_t cl[32] = {}; DWORD clLen = sizeof(cl);
        if (WinHttpQueryHeaders(request.get(), WINHTTP_QUERY_CONTENT_LENGTH,
                                WINHTTP_HEADER_NAME_BY_INDEX, cl, &clLen, WINHTTP_NO_HEADER_INDEX)) {
            total = _wtoi64(cl);
        }
    }
    const uint64_t kMaxBytes = 1024ULL << 30;  // 1 GiB sanity cap
    if (total > kMaxBytes) return L"文件过大，疑似地址错误。";

    std::vector<Byte> blob;
    blob.reserve(static_cast<size_t>(total > (16ULL << 20) ? 16ULL << 20 : total));
    uint64_t downloaded = 0;
    int lastPercent = -1;
    for (;;) {
        if (g_cancel.load()) return L"下载已取消。";
        DWORD available = 0;
        if (!WinHttpQueryDataAvailable(request.get(), &available)) {
            if (g_cancel.load()) return L"下载已取消。";
            return L"下载中断：无法读取更多数据。";
        }
        if (available == 0) break;
        if (available > (8u << 20)) available = 8u << 20;
        size_t old = blob.size();
        try {
            blob.resize(old + available);
        } catch (...) {
            return L"内存不足，下载失败。";
        }
        DWORD got = 0;
        if (!WinHttpReadData(request.get(), blob.data() + old, available, &got)) {
            if (g_cancel.load()) return L"下载已取消。";
            return L"下载中断：连接被重置。";
        }
        blob.resize(old + got);
        downloaded += got;
        if (downloaded > kMaxBytes) return L"文件超出大小限制，下载中止。";
        if (progress) {
            int percent = total > 0 ? static_cast<int>(downloaded * 100 / total) : -1;
            if (percent != lastPercent) {
                lastPercent = percent;
                DownloadProgress p;
                p.percent = percent;
                p.downloaded = downloaded;
                p.total = total;
                progress(p);
            }
        }
    }
    if (blob.empty()) return L"下载内容为空。";

    // ---- integrity ----------------------------------------------------------
    if (!spec.sha256Hex.empty()) {
        std::string got = lowerA(sha256Hex(blob.data(), blob.size()));
        std::string want = lowerA(spec.sha256Hex);
        if (got != want) {
            wchar_t msg[300];
            swprintf(msg, 300, L"SHA-256 校验失败，文件可能不完整或被篡改。\n期望：%hs\n实际：%hs",
                     want.c_str(), got.c_str());
            return msg;
        }
    }

    // ---- extract --------------------------------------------------------------
    zip::Reader zr = zip::open(blob);
    if (!zr.ok) return L"下载的文件不是有效的 ZIP 包：" + utf8ToWide(zr.error);

    std::wstring dir = occRuntimeDir();
    if (!ensureDirectory(dir)) return L"无法创建安装目录：" + dir;

    bool pluginFound = false;
    size_t extracted = 0;
    for (const auto& e : zr.entries) {
        if (g_cancel.load()) return L"安装已取消。";
        if (!e.name.empty() && (e.name.back() == '/')) continue;  // directory entry
        // sanitize + build wide path
        std::wstring rel;
        bool bad = false;
        size_t segStart = 0;
        std::string name = e.name;
        for (size_t i = 0; i <= name.size(); ++i) {
            if (i == name.size() || name[i] == '/') {
                std::string seg = name.substr(segStart, i - segStart);
                segStart = i + 1;
                if (seg.empty() || seg == "." ) continue;
                if (seg == ".." || seg.find(':') != std::string::npos) { bad = true; break; }
                if (!rel.empty()) rel += L'\\';
                rel += utf8ToWide(seg);
            }
        }
        if (bad || rel.empty()) continue;
        std::wstring target = dir + L"\\" + rel;
        size_t last = target.find_last_of(L'\\');
        if (last != std::wstring::npos) ensureDirectory(target.substr(0, last));
        std::vector<Byte> data;
        if (!zip::readEntry(zr, blob.data(), blob.size(), e, data, 512ULL << 20)) {
            wchar_t msg[256];
            swprintf(msg, 256, L"解压条目失败：%hs", e.name.c_str());
            return msg;
        }
        if (!writeFileBytes(target, data.data(), data.size())) {
            wchar_t msg[256];
            swprintf(msg, 256, L"写入文件失败：%hs", e.name.c_str());
            return msg;
        }
        ++extracted;
        std::wstring base = rel.substr(rel.find_last_of(L'\\') + 1);
        if (_wcsicmp(base.c_str(), L"bs3d_occ.dll") == 0) pluginFound = true;
    }
    if (!pluginFound) {
        return L"解压完成但未找到 bs3d_occ.dll，请确认下载地址指向正确的插件包。";
    }

    // stamp file for future reference
    std::string info = "url=" + wideToUtf8(spec.url) + "\nversion=" + wideToUtf8(spec.version) +
                       "\nsha256=" + spec.sha256Hex + "\n";
    writeFileBytes(dir + L"\\occ-pack.info", info.data(), info.size());

    if (progress) {
        DownloadProgress p;
        p.percent = 100;
        p.downloaded = downloaded;
        p.total = total;
        progress(p);
    }
    return L"";  // success
}

}  // namespace bs3d
