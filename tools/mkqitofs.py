#!/usr/bin/env python3
"""
mkqitofs.py - build a QitoFS ramdisk image from a directory tree.

QitoFS is QitoOS's simple read-mostly archive format. The kernel mounts the
image at boot and copies it into the in-memory VFS, so the format only needs to
describe a flat list of paths with their contents and metadata.

Layout:

    struct qitofs_header {
        char     magic[8];      // "QITOFS01"
        uint32_t version;
        uint32_t entry_count;
        uint64_t total_size;
        uint64_t data_offset;   // start of the file data region
        uint32_t flags;
        uint32_t checksum;      // sum of all data bytes, 32-bit wrapping
        char     label[32];
    };                          // 72 bytes

    struct qitofs_entry {
        char     path[192];     // absolute, NUL terminated
        uint32_t type;          // 1 file, 2 directory
        uint32_t permissions;
        uint64_t size;
        uint64_t offset;        // from data_offset
        uint64_t modified;
        uint32_t uid;
        uint32_t gid;
    };                          // 232 bytes
"""

from __future__ import annotations

import argparse
import os
import struct
import sys
import time

MAGIC = b"QITOFS01"
VERSION = 1
HEADER_FMT = "<8sIIQQII32s"
HEADER_SIZE = struct.calcsize(HEADER_FMT)
ENTRY_FMT = "<192sIIQQQII"
ENTRY_SIZE = struct.calcsize(ENTRY_FMT)

TYPE_FILE = 1
TYPE_DIR = 2

# Files matching these are never packed into the image.
SKIP_NAMES = {".gitkeep", ".DS_Store", "Thumbs.db"}


def collect(root: str) -> list[dict]:
    """Walk *root* and return the entries to pack, directories before files."""
    entries: list[dict] = []

    for dirpath, dirnames, filenames in os.walk(root):
        dirnames.sort()
        filenames.sort()

        rel_dir = os.path.relpath(dirpath, root)
        if rel_dir != ".":
            entries.append(
                {
                    "path": "/" + rel_dir.replace(os.sep, "/"),
                    "type": TYPE_DIR,
                    "perms": 0o755,
                    "data": b"",
                }
            )

        for name in filenames:
            if name in SKIP_NAMES:
                continue
            full = os.path.join(dirpath, name)
            rel = os.path.relpath(full, root).replace(os.sep, "/")
            with open(full, "rb") as handle:
                data = handle.read()

            # Anything under /bin is executable.
            executable = rel.startswith("bin/") or os.access(full, os.X_OK)
            entries.append(
                {
                    "path": "/" + rel,
                    "type": TYPE_FILE,
                    "perms": 0o755 if executable else 0o644,
                    "data": data,
                    "modified": int(os.path.getmtime(full)),
                }
            )

    # Directories first so the kernel can create parents before children,
    # then by depth so nesting always resolves.
    entries.sort(key=lambda e: (e["type"] != TYPE_DIR, e["path"].count("/"), e["path"]))
    return entries


def build(entries: list[dict], label: str) -> bytes:
    data_offset = HEADER_SIZE + ENTRY_SIZE * len(entries)
    # Align the data region so file contents start on a 16-byte boundary.
    data_offset = (data_offset + 15) & ~15

    table = bytearray()
    blob = bytearray()
    checksum = 0

    for entry in entries:
        payload = entry["data"]
        offset = len(blob)

        path = entry["path"].encode("utf-8")
        if len(path) >= 192:
            raise SystemExit(f"path too long for QitoFS: {entry['path']}")

        table += struct.pack(
            ENTRY_FMT,
            path,
            entry["type"],
            entry["perms"],
            len(payload),
            offset,
            entry.get("modified", int(time.time())),
            0,
            0,
        )
        blob += payload
        # Keep each file 16-byte aligned inside the data region.
        padding = (-len(blob)) % 16
        blob += b"\x00" * padding

        checksum = (checksum + sum(payload)) & 0xFFFFFFFF

    total = data_offset + len(blob)
    header = struct.pack(
        HEADER_FMT,
        MAGIC,
        VERSION,
        len(entries),
        total,
        data_offset,
        0,
        checksum,
        label.encode("utf-8")[:32],
    )

    image = bytearray(header)
    image += table
    image += b"\x00" * (data_offset - len(image))
    image += blob
    return bytes(image)


def main() -> int:
    parser = argparse.ArgumentParser(description="Build a QitoFS ramdisk image")
    parser.add_argument("--root", required=True, help="directory to pack")
    parser.add_argument("--output", required=True)
    parser.add_argument("--version", default="dev")
    args = parser.parse_args()

    if not os.path.isdir(args.root):
        # An empty root filesystem is valid; produce an image with no entries.
        print(f"warning: {args.root} does not exist, writing an empty image",
              file=sys.stderr)
        entries: list[dict] = []
    else:
        entries = collect(args.root)

    image = build(entries, f"qitoos-{args.version}")

    os.makedirs(os.path.dirname(os.path.abspath(args.output)), exist_ok=True)
    with open(args.output, "wb") as handle:
        handle.write(image)

    files = sum(1 for e in entries if e["type"] == TYPE_FILE)
    dirs = sum(1 for e in entries if e["type"] == TYPE_DIR)
    print(
        "QitoFS image: %s (%d bytes, %d files, %d directories)"
        % (args.output, len(image), files, dirs)
    )
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
