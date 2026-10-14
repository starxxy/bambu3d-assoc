// ---------------------------------------------------------------------------
// RETIRED (v1.3.0): 本软件已完全免费，「30 天试用 + 每日兑换码」付费方案退役，
// 本文件不再参与编译（见 CMakeLists.txt）。保留仅作历史记录，勿在发布代码中
// 重新引用。付费机制移除说明见 docs/分发与防盗专案.md 第 8 节。
// ---------------------------------------------------------------------------
#pragma once

#include "bs3d.h"

#include <string>

namespace bs3d {

// ---------------------------------------------------------------------------
// Offline license: 30-day trial + daily redeem code (内置密码表).
//
//   * Trial clock starts on the first call from either the manager exe or the
//     thumbnail DLL (per-user registry marker, throttled writes).
//   * After the trial the thumbnails render as a placeholder until the user
//     enters a redeem code. The code is date-bound: HMAC-SHA256(secret, date)
//     -> Crockford Base32 -> XXXX-XXXX-XXXX-XXXX, different every day, with a
//     +-1 day tolerance so a code stays usable around midnight.
//   * A successful redeem stores a machine-bound token (HMAC over the computer
//     name), so the registry value alone cannot activate another machine.
//
// Deliberate scope: raise the bar against casual sharing, not a fortress —
// the secret lives (obfuscated) inside the binary, there is no server.
// ---------------------------------------------------------------------------

struct LicenseInfo {
    bool activated = false;   // redeemed successfully (machine bound)
    bool trialActive = false; // inside the 30-day window and not activated
    int daysLeft = 0;         // trial days remaining (clamped 0..30)
};

// Read/create the trial markers and evaluate the current state. Cheap enough
// to call per thumbnail; safe in both the exe and the Explorer DLL process.
LicenseInfo licenseStatus();

// True when thumbnails may render (activated or inside the trial window).
bool licenseAllowsThumbnails();

// Validate a redeem code against today/yesterday/tomorrow and, on success,
// store the machine-bound activation token. `message` is user-facing Chinese.
bool redeemLicense(const std::wstring& code, std::wstring& message);

// Daily redeem code for `yyyymmdd` (nullptr/empty = today, local time).
// Exposed for the redeem_gen console tool and the Python cross-check.
std::wstring redeemCodeForDate(const wchar_t* yyyymmdd);

}  // namespace bs3d
