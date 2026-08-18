#!/usr/bin/env python3
"""
gensyms.py - build the kernel symbol table.

Reads the symbols out of the linked kernel ELF and emits a C array the kernel
links back in on a second pass. With this table a panic can report
"fault in heap_free+0x1c" instead of a bare address, which is the difference
between a usable bug report and a hex dump.

The two-pass arrangement is the standard trick: adding the table changes the
addresses slightly, so the generated file pads itself to a stable size and the
kernel is linked twice.

Usage:
    gensyms.py --input build/qira-kernel.elf --output build/ksyms.c
"""

from __future__ import annotations

import argparse
import os
import struct
import sys

# ELF symbol types and bindings we care about.
STT_FUNC = 2
STT_OBJECT = 1


def read_symbols(path: str) -> list[tuple[int, str]]:
    with open(path, "rb") as handle:
        data = handle.read()

    if data[:4] != b"\x7fELF" or data[4] != 2:
        raise SystemExit(f"error: {path} is not a 64-bit ELF file")

    shoff = struct.unpack_from("<Q", data, 40)[0]
    shentsize = struct.unpack_from("<H", data, 58)[0]
    shnum = struct.unpack_from("<H", data, 60)[0]

    sections = []
    for index in range(shnum):
        base = shoff + index * shentsize
        fields = struct.unpack_from("<IIQQQQIIQQ", data, base)
        sections.append(
            {
                "type": fields[1],
                "offset": fields[4],
                "size": fields[5],
                "link": fields[6],
                "entsize": fields[9],
            }
        )

    # Find the symbol table (SHT_SYMTAB is 2).
    symtab = None
    for section in sections:
        if section["type"] == 2:
            symtab = section
            break

    if not symtab:
        return []

    strtab = sections[symtab["link"]]
    names = data[strtab["offset"] : strtab["offset"] + strtab["size"]]

    symbols = []
    count = symtab["size"] // 24

    for index in range(count):
        base = symtab["offset"] + index * 24
        name_off, info, _other, shndx, value, _size = struct.unpack_from(
            "<IBBHQQ", data, base
        )

        kind = info & 0xF
        if kind != STT_FUNC:
            continue
        if shndx == 0 or value == 0:
            continue

        end = names.find(b"\x00", name_off)
        name = names[name_off:end].decode("ascii", "replace")

        # Skip compiler-internal and local labels.
        if not name or name.startswith((".L", "$", "__func__")):
            continue

        symbols.append((value, name))

    # Sorted by address, and deduplicated on address.
    symbols.sort(key=lambda item: item[0])

    unique = []
    seen = set()
    for address, name in symbols:
        if address in seen:
            continue
        seen.add(address)
        unique.append((address, name))

    return unique


def emit(symbols: list[tuple[int, str]], path: str, reserve: int) -> None:
    lines = []
    add = lines.append

    add("/*")
    add(" * GENERATED FILE - do not edit.")
    add(" *")
    add(" * The kernel symbol table, produced by tools/gensyms.py from the")
    add(" * linked kernel. It lets panics and backtraces name functions")
    add(" * instead of printing bare addresses.")
    add(" */")
    add("")
    add("#include <kernel/types.h>")
    add("#include <kernel/random.h>")
    add("")
    add("/* Placed in its own section so the linker can bracket the table. */")
    add('#define KSYM __attribute__((section(".ksyms"), used))')
    add("")
    add(f"/* {len(symbols)} symbols */")
    add("KSYM const struct kernel_symbol kernel_symbols[] = {")

    for address, name in symbols:
        escaped = name.replace("\\", "\\\\").replace('"', '\\"')
        add(f'    {{0x{address:016x}ull, "{escaped}"}},')

    add("};")
    add("")
    add(f"const int kernel_symbol_count = {len(symbols)};")
    add("")

    text = "\n".join(lines)

    # Pad to a fixed size so the second link does not shift addresses again.
    if reserve > 0:
        padding = reserve - len(text)
        if padding > 0:
            text += "\n/*" + ("x" * (padding - 6)) + "*/\n"

    os.makedirs(os.path.dirname(os.path.abspath(path)), exist_ok=True)
    with open(path, "w") as handle:
        handle.write(text)


def main() -> int:
    parser = argparse.ArgumentParser(description="Generate the kernel symbol table")
    parser.add_argument("--input", required=True, help="linked kernel ELF")
    parser.add_argument("--output", required=True, help="C file to write")
    parser.add_argument(
        "--empty", action="store_true", help="emit an empty table for the first pass"
    )
    args = parser.parse_args()

    if args.empty or not os.path.exists(args.input):
        emit([], args.output, 0)
        print(f"wrote {args.output} (empty, first pass)")
        return 0

    symbols = read_symbols(args.input)
    emit(symbols, args.output, 0)
    print(f"wrote {args.output} ({len(symbols)} symbols)")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
