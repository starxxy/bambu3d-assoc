# Post-build tool: patch the 4-byte CRC32 integrity slot in a built binary.
#
# src/protect.cpp embeds the 8-byte tag 'B3D1STMP' followed by a 4-byte slot.
# This tool computes CRC32 (IEEE, zlib) of the whole file with the slot zeroed
# and writes it into the slot. At startup bs3d_assoc.exe recomputes the same
# CRC and refuses to run on mismatch (tampering / rebranding).
#
# Idempotent: the CRC is always computed with the slot zeroed, so re-running
# on an already-stamped file reproduces the same value.
#
# Wired into CMake as a POST_BUILD step for bs3d_assoc and bs3d_thumb:
#   py tools/stamp_integrity.py <file> [more files...]
import sys
import zlib

TAG = b"B3D1STMP"


def stamp(path: str) -> int:
    with open(path, "rb") as f:
        data = bytearray(f.read())
    i = data.find(TAG)
    if i < 0:
        print(f"stamp: tag not found in {path} (target does not link protect.cpp?) "
              f"- skipped")
        return 0
    slot = i + len(TAG)
    data[slot:slot + 4] = b"\x00\x00\x00\x00"
    crc = zlib.crc32(bytes(data)) & 0xFFFFFFFF
    data[slot:slot + 4] = crc.to_bytes(4, "little")
    with open(path, "r+b") as f:
        f.write(bytes(data))
    print(f"stamp: {path} crc32=0x{crc:08X}")
    return 0


def main() -> int:
    rc = 0
    for path in sys.argv[1:]:
        rc |= stamp(path)
    return rc


if __name__ == "__main__":
    sys.exit(main())
