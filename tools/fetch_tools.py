"""Fetch a prebuilt vcpkg.exe from GitHub releases and install OpenCASCADE into e:\\occt.

Usage: python tools\\fetch_tools.py
Exits 0 on success, non-zero otherwise. Safe to re-run.
"""

from __future__ import annotations

import io
import os
import subprocess
import sys
import urllib.request

VCPKG_DEST = os.path.join(os.environ.get("TEMP", "C:\\Temp"), "vcpkg")
VCPKG_EXE = os.path.join(VCPKG_DEST, "vcpkg.exe")
INSTALL_ROOT = r"e:\occt"
RELEASES = ("2024.10.12", "2024.06.15", "2024.04.26", "2024.02.22", "2023.12.12")


def download(url: str, out_path: str) -> bool:
    print(f"[fetch] downloading {url} ...", flush=True)
    try:
        urllib.request.urlretrieve(url, out_path)
    except Exception as exc:  # noqa: BLE001
        print(f"[fetch] failed: {exc}", flush=True)
        return False
    print(f"[fetch] downloaded {os.path.getsize(out_path)} bytes", flush=True)
    return True


def main() -> int:
    os.makedirs(VCPKG_DEST, exist_ok=True)
    os.makedirs(INSTALL_ROOT, exist_ok=True)

    if not os.path.isfile(VCPKG_EXE):
        got = False
        for tag in RELEASES:
            url = f"https://github.com/microsoft/vcpkg/releases/download/{tag}/vcpkg.exe"
            if download(url, VCPKG_EXE):
                got = True
                break
        if not got:
            print("[fetch] could not download vcpkg.exe", flush=True)
            return 3

    v = subprocess.run([VCPKG_EXE, "version"], capture_output=True, text=True)
    print(f"[fetch] vcpkg version: {(v.stdout or v.stderr).strip()}", flush=True)

    print("[fetch] installing opencascade:x64-windows (may download a large archive) ...", flush=True)
    r = subprocess.run(
        [
            VCPKG_EXE, "install", "opencascade:x64-windows",
            f"--x-install-root={INSTALL_ROOT}", "--disable-metrics",
        ],
        cwd=VCPKG_DEST,
        stdout=subprocess.PIPE,
        stderr=subprocess.STDOUT,
        text=True,
        errors="replace",
    )
    with io.open(os.path.join(os.environ.get("TEMP", "C:\\Temp"), "occt_install.log"),
                 "w", encoding="utf-8", errors="replace") as fh:
        fh.write(r.stdout or "")
    print((r.stdout or "")[-4000:], flush=True)
    print(f"[fetch] install dir listing:", flush=True)
    for sub in os.listdir(INSTALL_ROOT):
        print("[fetch]  -", sub, flush=True)
    print(f"[fetch] exit code: {r.returncode}", flush=True)
    return r.returncode


if __name__ == "__main__":
    sys.exit(main())
