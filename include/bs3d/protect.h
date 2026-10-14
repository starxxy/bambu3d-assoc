#pragma once

namespace bs3d {
namespace protect {

// ---------------------------------------------------------------------------
// One-shot startup guard for the manager exe (see src/protect.cpp).
//
//   * anti-debug: user-mode debugger checks (Release builds only — Debug
//     builds never fight the developer's debugger)
//   * binary integrity: CRC32 self-check against the slot stamped post-build
//     by tools/stamp_integrity.py
//
// Returns false when the process must exit immediately. The thumbnail DLL
// deliberately runs no guard: it lives inside Explorer and must never pop
// UI or terminate its host process.
// ---------------------------------------------------------------------------
bool startupGuard();

// Plain-text bilingual notice addressed to whoever (human or AI) ends up
// reverse-engineering the binary. Deliberately NOT obfuscated — it must be
// readable in a strings dump or decompiler. Referenced from DllMain of the
// thumbnail DLL purely so the string is linked into that binary as well.
const char* aiNotice();

}  // namespace protect
}  // namespace bs3d
