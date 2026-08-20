#!/usr/bin/env python3
"""
checkboot.py - sanity check the linked bootloader blob.

Catches the mistakes that turn into an unbootable image and are painful to
diagnose from inside an emulator:

  * a missing or misplaced 0xAA55 boot signature,
  * stage 1 overflowing its 512-byte sector,
  * a missing payload descriptor table,
  * a loader too large for the space reserved on the ISO.
"""

from __future__ import annotations

import sys

MAX_BOOT_SIZE = 62 * 1024  # comfortably inside the low-memory window


def main(path: str) -> int:
    with open(path, "rb") as handle:
        data = handle.read()

    problems = []

    if len(data) < 512:
        problems.append(f"image is only {len(data)} bytes, need at least 512")
    elif data[510] != 0x55 or data[511] != 0xAA:
        problems.append(
            "missing 0xAA55 boot signature at offset 510 "
            f"(found {data[510]:#04x} {data[511]:#04x})"
        )

    if data.find(b"QITOPLD1") < 0:
        problems.append("payload descriptor table 'QITOPLD1' not found")

    if len(data) > MAX_BOOT_SIZE:
        problems.append(
            f"bootloader is {len(data)} bytes, over the {MAX_BOOT_SIZE} byte budget"
        )

    if problems:
        print("bootloader validation FAILED:", file=sys.stderr)
        for problem in problems:
            print(f"  - {problem}", file=sys.stderr)
        return 1

    print(
        "  CHECK bootloader ok (%d bytes, stage2 %d bytes)"
        % (len(data), len(data) - 512)
    )
    return 0


if __name__ == "__main__":
    if len(sys.argv) != 2:
        print("usage: checkboot.py <boot.bin>", file=sys.stderr)
        raise SystemExit(2)
    raise SystemExit(main(sys.argv[1]))
