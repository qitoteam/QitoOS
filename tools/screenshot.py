#!/usr/bin/env python3
"""
screenshot.py - capture the QitoOS framebuffer from a running emulator.

Bochs' ``nogui`` display writes nothing to disk, so instead of scraping the
emulator we ask the kernel for a copy of its framebuffer: the QCSH
``screenshot`` command writes a raw dump to a file, and the boot harness can
also request one over the serial control channel.

This tool converts such a dump (or a raw framebuffer region extracted from an
emulator memory snapshot) into a PNG, using only the Python standard library.
"""

from __future__ import annotations

import argparse
import binascii
import struct
import sys
import zlib


def write_png(path: str, width: int, height: int, rgb_rows: list[bytes]) -> None:
    """Write 8-bit RGB rows as a PNG file."""

    def chunk(tag: bytes, data: bytes) -> bytes:
        return (
            struct.pack(">I", len(data))
            + tag
            + data
            + struct.pack(">I", binascii.crc32(tag + data) & 0xFFFFFFFF)
        )

    raw = b"".join(b"\x00" + row for row in rgb_rows)

    png = b"\x89PNG\r\n\x1a\n"
    png += chunk(b"IHDR", struct.pack(">IIBBBBB", width, height, 8, 2, 0, 0, 0))
    png += chunk(b"IDAT", zlib.compress(raw, 6))
    png += chunk(b"IEND", b"")

    with open(path, "wb") as handle:
        handle.write(png)


def convert(
    source: str, output: str, width: int, height: int, pitch: int | None
) -> None:
    with open(source, "rb") as handle:
        data = handle.read()

    pitch_px = pitch if pitch else width
    needed = pitch_px * height * 4
    if len(data) < needed:
        raise SystemExit(
            f"error: {source} holds {len(data)} bytes, need {needed} "
            f"for {width}x{height} at 32bpp"
        )

    rows = []
    for y in range(height):
        base = y * pitch_px * 4
        row = bytearray()
        for x in range(width):
            offset = base + x * 4
            # Stored as 0x00RRGGBB little endian: B, G, R, X.
            blue = data[offset]
            green = data[offset + 1]
            red = data[offset + 2]
            row += bytes((red, green, blue))
        rows.append(bytes(row))

    write_png(output, width, height, rows)
    print(f"wrote {output} ({width}x{height})")


def main() -> int:
    parser = argparse.ArgumentParser(
        description="Convert a QitoOS framebuffer dump to PNG"
    )
    parser.add_argument("--input", required=True, help="raw 32bpp framebuffer dump")
    parser.add_argument("--output", required=True, help="PNG file to write")
    parser.add_argument("--width", type=int, required=True)
    parser.add_argument("--height", type=int, required=True)
    parser.add_argument(
        "--pitch", type=int, default=None, help="row stride in pixels (default: width)"
    )
    args = parser.parse_args()

    convert(args.input, args.output, args.width, args.height, args.pitch)
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
