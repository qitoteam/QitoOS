#!/usr/bin/env python3
"""
grabframe.py - extract framebuffer captures from a QitoOS serial log.

The kernel streams the composited desktop out of COM1 as base64 RGB between
``--QITO-FRAME-BEGIN`` and ``--QITO-FRAME-END--`` markers (see
src/kernel/gfx/capture.c). This tool finds those blocks and writes each one out
as a PNG, which is how the project produces screenshots and how the automated
boot tests check what actually appeared on screen.

Usage:
    grabframe.py --log serial.log --output docs/screenshots/desktop.png
"""

from __future__ import annotations

import argparse
import base64
import binascii
import os
import re
import struct
import sys
import zlib

BEGIN = re.compile(r"--QITO-FRAME-BEGIN (\d+)x(\d+) (\w+) (\S*)")
END = "--QITO-FRAME-END--"


def write_png(path: str, width: int, height: int, rgb: bytes) -> None:
    """Write packed RGB bytes as a PNG."""

    def chunk(tag: bytes, data: bytes) -> bytes:
        return (
            struct.pack(">I", len(data))
            + tag
            + data
            + struct.pack(">I", binascii.crc32(tag + data) & 0xFFFFFFFF)
        )

    stride = width * 3
    raw = bytearray()
    for y in range(height):
        raw.append(0)  # filter type: none
        raw += rgb[y * stride : (y + 1) * stride]

    png = b"\x89PNG\r\n\x1a\n"
    png += chunk(b"IHDR", struct.pack(">IIBBBBB", width, height, 8, 2, 0, 0, 0))
    png += chunk(b"IDAT", zlib.compress(bytes(raw), 9))
    png += chunk(b"IEND", b"")

    os.makedirs(os.path.dirname(os.path.abspath(path)), exist_ok=True)
    with open(path, "wb") as handle:
        handle.write(png)


def decode_rle24(data: bytes, pixel_count: int) -> bytes:
    """
    Expand the kernel's run-length encoding into packed RGB.

    Each record is four bytes: a repeat count of 1-255 followed by the red,
    green and blue components of the pixel.
    """
    out = bytearray()
    for offset in range(0, len(data) - 3, 4):
        count = data[offset]
        pixel = data[offset + 1 : offset + 4]
        out += pixel * count
        if len(out) >= pixel_count * 3:
            break
    return bytes(out)


def extract(log_text: str) -> list[dict]:
    """Return every complete frame found in the log."""
    frames = []
    lines = log_text.replace("\r", "").split("\n")

    index = 0
    while index < len(lines):
        match = BEGIN.search(lines[index])
        if not match:
            index += 1
            continue

        width = int(match.group(1))
        height = int(match.group(2))
        label = match.group(4) or "frame"

        payload = []
        index += 1
        while index < len(lines) and END not in lines[index]:
            payload.append(lines[index].strip())
            index += 1

        if index >= len(lines):
            print("warning: truncated frame, ignoring", file=sys.stderr)
            break

        try:
            data = base64.b64decode("".join(payload), validate=False)
        except (binascii.Error, ValueError) as error:
            print(f"warning: could not decode a frame: {error}", file=sys.stderr)
            index += 1
            continue

        encoding = match.group(3)
        expected = width * height * 3

        if encoding == "rle24":
            data = decode_rle24(data, width * height)
        elif encoding != "rgb24":
            print(f"warning: unknown encoding '{encoding}', skipping",
                  file=sys.stderr)
            index += 1
            continue

        if len(data) < expected:
            print(
                f"warning: frame is {len(data)} bytes, expected {expected}; padding",
                file=sys.stderr,
            )
            data = data.ljust(expected, b"\x00")

        frames.append(
            {"width": width, "height": height, "label": label, "data": data[:expected]}
        )
        index += 1

    return frames


def main() -> int:
    parser = argparse.ArgumentParser(
        description="Extract QitoOS framebuffer captures from a serial log"
    )
    parser.add_argument("--log", required=True, help="serial log to read")
    parser.add_argument("--output", required=True, help="PNG path (or prefix)")
    parser.add_argument(
        "--index", type=int, default=-1, help="which frame to write (default: last)"
    )
    parser.add_argument(
        "--all", action="store_true", help="write every frame, numbering the files"
    )
    args = parser.parse_args()

    with open(args.log, "r", errors="replace") as handle:
        frames = extract(handle.read())

    if not frames:
        print("error: no framebuffer captures found in the log", file=sys.stderr)
        return 1

    print(f"found {len(frames)} frame(s)")

    if args.all:
        stem, ext = os.path.splitext(args.output)
        for number, frame in enumerate(frames):
            path = f"{stem}-{number}{ext or '.png'}"
            write_png(path, frame["width"], frame["height"], frame["data"])
            print(f"wrote {path} ({frame['width']}x{frame['height']}, {frame['label']})")
        return 0

    frame = frames[args.index]
    write_png(args.output, frame["width"], frame["height"], frame["data"])
    print(
        "wrote %s (%dx%d, %s)"
        % (args.output, frame["width"], frame["height"], frame["label"])
    )
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
