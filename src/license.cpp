// ---------------------------------------------------------------------------
// RETIRED (v1.3.0): 本软件已完全免费，「30 天试用 + 每日兑换码」付费方案退役，
// 本文件不再参与编译（见 CMakeLists.txt）。保留仅作历史记录，勿在发布代码中
// 重新引用。付费机制移除说明见 docs/分发与防盗专案.md 第 8 节。
// ---------------------------------------------------------------------------
#include "bs3d/license.h"

#include "bs3d/registry.h"
#include "bs3d/winutil.h"

#include <windows.h>
#include <bcrypt.h>
#pragma comment(lib, "bcrypt.lib")

#include <cwchar>
#include <cwctype>
#include <cstdint>
#include <cstdio>
#include <string>

namespace bs3d {
namespace {

// ---------------------------------------------------------------------------
// Embedded secret, XOR-obfuscated at rest so a plain strings(1) dump of the
// binary does not hand over the key. Reassembled on first use. Offline scheme:
// raises the bar against casual sharing, it is not a fortress.
//
// NOTE: 公开仓库版本已脱敏。原始密钥字节在此处以占位符替换 —— 该密钥属于
// v1.2.4 及更早版本「每日兑换码」付费机制，v1.3.0 起该机制已退役，本文件
// 不参与编译，密钥不再公开。此处保留数组形态仅为说明原有实现结构。
// ---------------------------------------------------------------------------
const unsigned char kSecretXor[32] = { 0 };   // 已脱敏：原密钥不公开
const unsigned char kSecretMask[32] = { 0 };  // 已脱敏：原密钥不公开

const unsigned char* secretBytes() {
    static unsigned char s[32];
    for (int i = 0; i < 32; ++i)
        s[i] = static_cast<unsigned char>(kSecretXor[i] ^ kSecretMask[i]);
    return s;
}

// ---------------------------------------------------------------------------
// HMAC-SHA256 (CNG, keyed hash). Byte-identical to Python's hmac module.
// ---------------------------------------------------------------------------
bool hmacSha256(const unsigned char* key, size_t keyLen,
                const void* msg, size_t msgLen, unsigned char out[32]) {
    BCRYPT_ALG_HANDLE alg = nullptr;
    if (BCryptOpenAlgorithmProvider(&alg, BCRYPT_SHA256_ALGORITHM, nullptr,
                                    BCRYPT_ALG_HANDLE_HMAC_FLAG) != 0)
        return false;
    BCRYPT_HASH_HANDLE h = nullptr;
    bool ok = BCryptCreateHash(alg, &h, nullptr, 0,
                               const_cast<PUCHAR>(key), static_cast<ULONG>(keyLen), 0) == 0;
    if (ok)
        ok = BCryptHashData(h, const_cast<PUCHAR>(static_cast<const unsigned char*>(msg)),
                            static_cast<ULONG>(msgLen), 0) == 0;
    if (ok) ok = BCryptFinishHash(h, out, 32, 0) == 0;
    if (h) BCryptDestroyHash(h);
    BCryptCloseAlgorithmProvider(alg, 0);
    return ok;
}

// Crockford Base32: no I/L/O/U (they read as 1/1/0/V when typed by hand).
const wchar_t* const kB32 = L"0123456789ABCDEFGHJKMNPQRSTVWXYZ";

// 10 digest bytes = 80 bits = exactly 16 base32 symbols.
std::wstring encodeDaily(const unsigned char dg[32]) {
    wchar_t sym[17];
    int oi = 0;
    uint64_t acc = 0;
    int bits = 0;
    for (int i = 0; i < 10; ++i) {
        acc = (acc << 8) | dg[i];
        bits += 8;
        while (bits >= 5) {
            sym[oi++] = kB32[(acc >> (bits - 5)) & 31];
            bits -= 5;
        }
    }
    sym[16] = 0;
    std::wstring s;
    s.reserve(19);
    for (int i = 0; i < 16; ++i) {
        if (i && (i % 4) == 0) s += L'-';
        s += sym[i];
    }
    return s;
}

constexpr ULONGLONG kDay100ns = 86400ULL * 10000000ULL;
constexpr int kTrialDays = 30;

ULONGLONG nowFileTime() {
    FILETIME ft;
    GetSystemTimeAsFileTime(&ft);
    ULARGE_INTEGER u;
    u.LowPart = ft.dwLowDateTime;
    u.HighPart = ft.dwHighDateTime;
    return u.QuadPart;
}

// ---- registry markers (per-user, same namespace as everything else) --------
bool getQwordMarker(const wchar_t* name, ULONGLONG& out) {
    std::wstring s;
    if (!getRegString(HKEY_CURRENT_USER, AppConst::kRootKey, name, s) || s.empty())
        return false;
    out = _wcstoui64(s.c_str(), nullptr, 10);
    return out != 0;
}

bool setQwordMarker(const wchar_t* name, ULONGLONG v) {
    wchar_t buf[24];
    _ui64tow(v, buf, 10);
    return setRegString(HKEY_CURRENT_USER, AppConst::kRootKey, name, buf);
}

std::wstring todayStamp() {
    SYSTEMTIME st;
    GetLocalTime(&st);
    wchar_t buf[16];
    swprintf(buf, 16, L"%04u%02u%02u", st.wYear, st.wMonth, st.wDay);
    return buf;
}

bool validStamp(const std::wstring& d) {
    if (d.size() != 8) return false;
    for (wchar_t c : d)
        if (c < L'0' || c > L'9') return false;
    return true;
}

// Shift a yyyymmdd stamp by whole days (local-time roundtrip, DST-safe enough
// for a +-1 day validation window).
bool shiftStamp(const std::wstring& stamp, int days, std::wstring& out) {
    if (!validStamp(stamp)) return false;
    SYSTEMTIME st = {};
    st.wYear = static_cast<WORD>(_wtoi(stamp.substr(0, 4).c_str()));
    st.wMonth = static_cast<WORD>(_wtoi(stamp.substr(4, 2).c_str()));
    st.wDay = static_cast<WORD>(_wtoi(stamp.substr(6, 2).c_str()));
    if (st.wYear < 2020 || st.wYear > 2200 || st.wMonth < 1 || st.wMonth > 12 ||
        st.wDay < 1 || st.wDay > 31)
        return false;
    FILETIME ft;
    if (!SystemTimeToFileTime(&st, &ft)) return false;
    ULARGE_INTEGER u;
    u.LowPart = ft.dwLowDateTime;
    u.HighPart = ft.dwHighDateTime;
    u.QuadPart += static_cast<ULONGLONG>(days) * kDay100ns;
    ft.dwLowDateTime = u.LowPart;
    ft.dwHighDateTime = u.HighPart;
    SYSTEMTIME st2;
    if (!FileTimeToSystemTime(&ft, &st2)) return false;
    wchar_t buf[16];
    swprintf(buf, 16, L"%04u%02u%02u", st2.wYear, st2.wMonth, st2.wDay);
    out = buf;
    return true;
}

std::wstring codeForStamp(const std::wstring& stamp) {
    std::string msg = "B3D1|";
    for (wchar_t c : stamp) msg.push_back(static_cast<char>(c));  // digits only
    unsigned char dg[32];
    if (!hmacSha256(secretBytes(), 32, msg.data(), msg.size(), dg)) return L"";
    return encodeDaily(dg);
}

// Uppercase, strip separators, apply Crockford transliteration (I/L->1, O->0,
// U->V) and drop everything that is not in the alphabet.
std::wstring normalizeCode(const std::wstring& in) {
    std::wstring s;
    s.reserve(in.size());
    for (wchar_t c : in) {
        wchar_t u = towupper(c);
        if (u >= L'0' && u <= L'9') {
            s += u;
            continue;
        }
        if (u >= L'A' && u <= L'Z') {
            if (u == L'I' || u == L'L') u = L'1';
            else if (u == L'O') u = L'0';
            else if (u == L'U') u = L'V';
            if (wcschr(kB32, u)) s += u;
        }
        // dashes / spaces / anything else: skipped
    }
    return s;
}

bool constTimeEqual(const std::wstring& a, const std::wstring& b) {
    if (a.size() != b.size()) return false;
    volatile unsigned char diff = 0;
    for (size_t i = 0; i < a.size(); ++i)
        diff |= static_cast<unsigned char>(a[i]) ^ static_cast<unsigned char>(b[i]);
    return diff == 0;
}

std::wstring machineTag() {
    wchar_t buf[MAX_COMPUTERNAME_LENGTH + 2] = {};
    DWORD n = MAX_COMPUTERNAME_LENGTH + 1;
    if (!GetComputerNameW(buf, &n) || n == 0) return L"UNKNOWN-HOST";
    std::wstring s(buf, n);
    for (auto& c : s) c = static_cast<wchar_t>(towupper(c));
    return s;
}

// First 16 bytes of HMAC(secret, "ACT|" + machineName), lowercase hex.
std::wstring activationToken() {
    std::string msg = "ACT|";
    for (wchar_t w : machineTag()) {
        msg.push_back(static_cast<char>(w & 0xFF));
        msg.push_back(static_cast<char>((w >> 8) & 0xFF));
    }
    unsigned char dg[32];
    if (!hmacSha256(secretBytes(), 32, msg.data(), msg.size(), dg)) return L"";
    static const char* hexd = "0123456789abcdef";
    std::wstring s;
    s.reserve(32);
    for (int i = 0; i < 16; ++i) {
        s += static_cast<wchar_t>(hexd[dg[i] >> 4]);
        s += static_cast<wchar_t>(hexd[dg[i] & 0xF]);
    }
    return s;
}

bool checkActivated() {
    std::wstring stored;
    if (!getRegString(HKEY_CURRENT_USER, AppConst::kRootKey, L"Activated", stored))
        return false;
    const std::wstring want = activationToken();
    return !want.empty() && constTimeEqual(stored, want);
}

}  // namespace

// ---------------------------------------------------------------------------
// public API
// ---------------------------------------------------------------------------

LicenseInfo licenseStatus() {
    LicenseInfo li;
    const ULONGLONG now = nowFileTime();

    ULONGLONG first = 0, last = 0;
    if (!getQwordMarker(L"TrialFirst", first)) {
        // First touch from either the exe or the thumbnail DLL: clock starts.
        first = now;
        setQwordMarker(L"TrialFirst", first);
        setQwordMarker(L"TrialLast", first);
        last = first;
    } else if (!getQwordMarker(L"TrialLast", last) || last < first) {
        last = first;
        setQwordMarker(L"TrialLast", last);
    } else if (now > last && (now - last) > 3600ULL * 10000000ULL) {
        // Persist progress at most once per hour per process (the DLL calls
        // this per thumbnail; a registry write per call would be wasteful).
        last = now;
        setQwordMarker(L"TrialLast", last);
    }

    li.activated = checkActivated();

    // Clock rollback cannot extend the trial: judge against the high-water mark.
    const ULONGLONG effective = now > last ? now : last;
    const ULONGLONG used = (effective > first) ? (effective - first) / kDay100ns : 0;
    int left = kTrialDays - static_cast<int>(used);
    if (left < 0) left = 0;
    if (left > kTrialDays) left = kTrialDays;
    li.daysLeft = left;
    li.trialActive = !li.activated && left > 0;
    return li;
}

bool licenseAllowsThumbnails() {
    const LicenseInfo li = licenseStatus();
    return li.activated || li.daysLeft > 0;
}

bool redeemLicense(const std::wstring& code, std::wstring& message) {
    message.clear();
    const std::wstring norm = normalizeCode(code);
    if (norm.size() != 16) {
        message = L"兑换码格式不正确：应为 16 位，形如 XXXX-XXXX-XXXX-XXXX。";
        return false;
    }
    const std::wstring today = todayStamp();
    std::wstring d;
    for (int off = -1; off <= 1; ++off) {  // +-1 day tolerance around midnight
        if (!shiftStamp(today, off, d)) continue;
        if (constTimeEqual(norm, normalizeCode(codeForStamp(d)))) {
            if (setRegString(HKEY_CURRENT_USER, AppConst::kRootKey,
                             L"Activated", activationToken())) {
                message = L"激活成功，缩略图功能已解锁。感谢支持！";
                return true;
            }
            message = L"激活写入失败，请检查权限后重试。";
            return false;
        }
    }
    message = L"兑换码无效或已过期。兑换码每日更新，请向作者索取当日兑换码。";
    return false;
}

std::wstring redeemCodeForDate(const wchar_t* yyyymmdd) {
    std::wstring stamp = (yyyymmdd && *yyyymmdd) ? yyyymmdd : todayStamp();
    if (!validStamp(stamp)) return L"";
    return codeForStamp(stamp);
}

}  // namespace bs3d
