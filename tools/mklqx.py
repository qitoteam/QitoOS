#!/usr/bin/env python3
"""
mklqx.py - link a Linked Qira Executable (.lqx).

Takes an ELF object or executable produced by the host toolchain, extracts the
sections Qira cares about, and repackages them behind a QX header. Imports of
kernel services are recorded by name so the loader can resolve them at load
time; see src/kernel/include/kernel/lqx.h for the format and the rationale.

Usage:
    mklqx.py --input program.elf --output program.lqx --name hello
    mklqx.py --info program.lqx
"""

from __future__ import annotations

import argparse
import struct
import sys

QX_SIGNATURE = b"QX"
LQX_VERSION = 1
LQX_MACHINE_X86_64 = 0x8664

HEADER_FMT = "<2sBBHHIIQQIIIIIIII24s"
HEADER_SIZE = struct.calcsize(HEADER_FMT)
SECTION_FMT = "<12sBBHQIII"
SECTION_SIZE = struct.calcsize(SECTION_FMT)
IMPORT_FMT = "<24sQ"
IMPORT_SIZE = struct.calcsize(IMPORT_FMT)
SYMBOL_FMT = "<24sQ"
SYMBOL_SIZE = struct.calcsize(SYMBOL_FMT)

# Flags
FLAG_EXECUTABLE = 0x0001
FLAG_LIBRARY = 0x0002
FLAG_SERVICE = 0x0004
FLAG_DESKTOP = 0x0008
FLAG_PRIVILEGED = 0x0010

# Section kinds
SEC_CODE, SEC_DATA, SEC_RODATA, SEC_BSS, SEC_RESOURCE = 1, 2, 3, 4, 5

# Section permission flags
P_READ, P_WRITE, P_EXEC = 0x01, 0x02, 0x04

# Offset of the checksum field, which is excluded from the sum.
CHECKSUM_OFFSET = 56

assert HEADER_SIZE == 88, f"header is {HEADER_SIZE} bytes, expected 88"
assert SECTION_SIZE == 36, f"section is {SECTION_SIZE} bytes, expected 36"
assert IMPORT_SIZE == 32, f"import is {IMPORT_SIZE} bytes"
assert SYMBOL_SIZE == 32, f"symbol is {SYMBOL_SIZE} bytes"


# --- minimal ELF reader ----------------------------------------------------
#
# Only enough of ELF64 to pull out section contents; this is not a general
# purpose parser and deliberately refuses anything it does not understand.


class ElfFile:
    def __init__(self, data: bytes) -> None:
        if len(data) < 64 or data[:4] != b"\x7fELF":
            raise ValueError("not an ELF file")
        if data[4] != 2:
            raise ValueError("not a 64-bit ELF file")
        if data[5] != 1:
            raise ValueError("not a little-endian ELF file")

        self.data = data
        self.entry = struct.unpack_from("<Q", data, 24)[0]

        shoff = struct.unpack_from("<Q", data, 40)[0]
        shentsize = struct.unpack_from("<H", data, 58)[0]
        shnum = struct.unpack_from("<H", data, 60)[0]
        shstrndx = struct.unpack_from("<H", data, 62)[0]

        self.sections = []
        for index in range(shnum):
            base = shoff + index * shentsize
            name_off, kind, flags, addr, offset, size, link, info, align, entsize = (
                struct.unpack_from("<IIQQQQIIQQ", data, base)
            )
            self.sections.append(
                {
                    "name_off": name_off,
                    "type": kind,
                    "flags": flags,
                    "addr": addr,
                    "offset": offset,
                    "size": size,
                    "link": link,
                    "entsize": entsize,
                }
            )

        # Resolve section names.
        strtab = self.sections[shstrndx]
        table = data[strtab["offset"] : strtab["offset"] + strtab["size"]]
        for section in self.sections:
            end = table.find(b"\x00", section["name_off"])
            section["name"] = table[section["name_off"] : end].decode(
                "ascii", "replace"
            )

    def find(self, name: str):
        for section in self.sections:
            if section["name"] == name:
                return section
        return None

    def contents(self, section) -> bytes:
        if section["type"] == 8:  # SHT_NOBITS, i.e. .bss
            return b""
        return self.data[section["offset"] : section["offset"] + section["size"]]

    def symbols(self) -> list[tuple[str, int, int]]:
        """Return (name, address, info) for every symbol table entry."""
        symtab = self.find(".symtab")
        if not symtab:
            return []

        strtab = self.sections[symtab["link"]]
        names = self.data[strtab["offset"] : strtab["offset"] + strtab["size"]]

        out = []
        count = symtab["size"] // 24
        for index in range(count):
            base = symtab["offset"] + index * 24
            name_off, info, _other, _shndx, value, _size = struct.unpack_from(
                "<IBBHQQ", self.data, base
            )
            end = names.find(b"\x00", name_off)
            name = names[name_off:end].decode("ascii", "replace")
            if name:
                out.append((name, value, info))
        return out


# --- building --------------------------------------------------------------

# ELF section name -> (LQX kind, permissions)
SECTION_MAP = {
    ".text": (SEC_CODE, P_READ | P_EXEC),
    ".rodata": (SEC_RODATA, P_READ),
    ".data": (SEC_DATA, P_READ | P_WRITE),
    ".bss": (SEC_BSS, P_READ | P_WRITE),
    ".qira_resource": (SEC_RESOURCE, P_READ),
}


def build(
    elf: ElfFile,
    name: str,
    flags: int,
    imports: list[tuple[str, int]],
    stack_size: int,
) -> bytes:
    sections = []
    payload = bytearray()

    for elf_name, (kind, permissions) in SECTION_MAP.items():
        section = elf.find(elf_name)
        if not section or section["size"] == 0:
            continue

        contents = elf.contents(section)
        file_offset = 0
        file_size = 0

        if kind != SEC_BSS:
            # Payload offsets are absolute in the finished file, so they are
            # fixed up once the table sizes are known.
            file_offset = len(payload)
            file_size = len(contents)
            payload += contents
            # Keep every section 16-byte aligned in the file.
            while len(payload) % 16:
                payload.append(0)

        sections.append(
            {
                "name": elf_name[:12],
                "kind": kind,
                "flags": permissions,
                "align": 16,
                "addr": section["addr"],
                "file_offset": file_offset,
                "file_size": file_size,
                "memory_size": section["size"],
            }
        )

    if not sections:
        raise SystemExit("error: the input has no sections Qira can use")

    # Collect exported symbols, skipping the compiler's internal ones.
    symbols = []
    for symbol_name, address, info in elf.symbols():
        if symbol_name.startswith(("$", ".L", "__")):
            continue
        if (info & 0xF) != 2:  # STT_FUNC only
            continue
        if len(symbols) >= 128:
            break
        symbols.append((symbol_name[:24], address))

    section_offset = HEADER_SIZE
    import_offset = section_offset + len(sections) * SECTION_SIZE
    symbol_offset = import_offset + len(imports) * IMPORT_SIZE
    payload_offset = symbol_offset + len(symbols) * SYMBOL_SIZE

    # Now that the tables are sized, make the section offsets absolute.
    for section in sections:
        if section["kind"] != SEC_BSS:
            section["file_offset"] += payload_offset

    load_base = min(s["addr"] for s in sections)
    total_size = payload_offset + len(payload)

    header = struct.pack(
        HEADER_FMT,
        QX_SIGNATURE,
        ord("L"),
        LQX_VERSION,
        LQX_MACHINE_X86_64,
        flags,
        HEADER_SIZE,
        total_size,
        elf.entry,
        load_base,
        len(sections),
        section_offset,
        len(imports),
        import_offset,
        len(symbols),
        symbol_offset,
        0,  # checksum, filled in below
        stack_size,
        name.encode("ascii", "ignore")[:24],
    )

    body = bytearray(header)
    for section in sections:
        body += struct.pack(
            SECTION_FMT,
            section["name"].encode("ascii", "ignore"),
            section["kind"],
            section["flags"],
            section["align"],
            section["addr"],
            section["file_offset"],
            section["file_size"],
            section["memory_size"],
        )
    for import_name, patch_address in imports:
        body += struct.pack(
            IMPORT_FMT, import_name.encode("ascii", "ignore")[:24], patch_address
        )
    for symbol_name, address in symbols:
        body += struct.pack(
            SYMBOL_FMT, symbol_name.encode("ascii", "ignore")[:24], address
        )
    body += payload

    # The checksum covers the whole file with its own field treated as zero.
    checksum = 0
    for index, byte in enumerate(body):
        if CHECKSUM_OFFSET <= index < CHECKSUM_OFFSET + 4:
            continue
        checksum = (checksum + byte) & 0xFFFFFFFF

    struct.pack_into("<I", body, CHECKSUM_OFFSET, checksum)
    return bytes(body)


# --- inspection ------------------------------------------------------------


def describe(path: str) -> int:
    with open(path, "rb") as handle:
        data = handle.read()

    if len(data) < HEADER_SIZE:
        print("error: file is too small to be an LQX image", file=sys.stderr)
        return 1

    fields = struct.unpack(HEADER_FMT, data[:HEADER_SIZE])
    (
        signature, fmt, version, machine, flags,
        header_size, total_size,
        entry, load_base,
        section_count, section_offset,
        import_count, import_offset,
        symbol_count, symbol_offset,
        checksum, stack_size, name,
    ) = fields

    if signature != QX_SIGNATURE:
        print("error: missing the QX signature", file=sys.stderr)
        return 1

    print(f"{path}")
    print(f"  Signature      QX/{chr(fmt)} version {version}")
    print(f"  Name           {name.rstrip(bytes([0])).decode()}")
    print(f"  Machine        {'x86_64' if machine == LQX_MACHINE_X86_64 else machine}")
    print(f"  Size           {total_size} bytes (file is {len(data)})")
    print(f"  Entry point    0x{entry:x}")
    print(f"  Link base      0x{load_base:x}")
    print(f"  Stack          {stack_size or 'default'}")

    attributes = []
    for bit, label in [
        (FLAG_EXECUTABLE, "executable"),
        (FLAG_LIBRARY, "library"),
        (FLAG_SERVICE, "service"),
        (FLAG_DESKTOP, "desktop"),
        (FLAG_PRIVILEGED, "privileged"),
    ]:
        if flags & bit:
            attributes.append(label)
    print(f"  Attributes     {', '.join(attributes) or 'none'}")

    # Verify the checksum the way the kernel does.
    computed = 0
    for index, byte in enumerate(data):
        if CHECKSUM_OFFSET <= index < CHECKSUM_OFFSET + 4:
            continue
        computed = (computed + byte) & 0xFFFFFFFF
    print(f"  Checksum       0x{checksum:08x} "
          f"({'valid' if computed == checksum else 'MISMATCH'})")

    kinds = {1: "code", 2: "data", 3: "rodata", 4: "bss", 5: "resource"}
    print(f"\n  Sections ({section_count})")
    print(f"    {'NAME':<14} {'KIND':<9} {'ADDRESS':<18} {'FILE':>8} {'MEMORY':>8}  PERM")
    for index in range(section_count):
        base = section_offset + index * SECTION_SIZE
        sname, kind, sflags, _align, addr, foff, fsize, msize = struct.unpack_from(
            SECTION_FMT, data, base
        )
        permissions = "".join(
            [
                "r" if sflags & P_READ else "-",
                "w" if sflags & P_WRITE else "-",
                "x" if sflags & P_EXEC else "-",
            ]
        )
        label = sname.rstrip(bytes([0])).decode()
        print(f"    {label:<14} {kinds.get(kind, '?'):<9} 0x{addr:016x} "
              f"{fsize:>8} {msize:>8}  {permissions}")

    if import_count:
        print(f"\n  Imports ({import_count})")
        for index in range(import_count):
            base = import_offset + index * IMPORT_SIZE
            iname, patch = struct.unpack_from(IMPORT_FMT, data, base)
            print(f"    {iname.rstrip(bytes([0])).decode():<24} patched at 0x{patch:x}")

    if symbol_count:
        print(f"\n  Symbols ({symbol_count})")
        for index in range(min(symbol_count, 20)):
            base = symbol_offset + index * SYMBOL_SIZE
            sym, addr = struct.unpack_from(SYMBOL_FMT, data, base)
            print(f"    0x{addr:016x}  {sym.rstrip(bytes([0])).decode()}")
        if symbol_count > 20:
            print(f"    ... and {symbol_count - 20} more")

    return 0


def main() -> int:
    parser = argparse.ArgumentParser(description="Link a Qira executable")
    parser.add_argument("--input", help="ELF object or executable")
    parser.add_argument("--output", help="destination .lqx file")
    parser.add_argument("--name", default="program")
    parser.add_argument("--info", help="describe an existing .lqx file")
    parser.add_argument("--library", action="store_true")
    parser.add_argument("--service", action="store_true")
    parser.add_argument("--desktop", action="store_true")
    parser.add_argument("--privileged", action="store_true")
    parser.add_argument("--stack", type=int, default=0)
    parser.add_argument(
        "--import", dest="imports", action="append", default=[],
        metavar="NAME@ADDR",
        help="a kernel service to resolve, and where to patch it",
    )
    args = parser.parse_args()

    if args.info:
        return describe(args.info)

    if not args.input or not args.output:
        parser.print_help()
        return 1

    flags = 0
    if args.library:
        flags |= FLAG_LIBRARY
    else:
        flags |= FLAG_EXECUTABLE
    if args.service:
        flags |= FLAG_SERVICE
    if args.desktop:
        flags |= FLAG_DESKTOP
    if args.privileged:
        flags |= FLAG_PRIVILEGED

    imports = []
    for spec in args.imports:
        if "@" not in spec:
            raise SystemExit(f"error: --import needs NAME@ADDRESS, got {spec!r}")
        import_name, address = spec.rsplit("@", 1)
        imports.append((import_name, int(address, 0)))

    with open(args.input, "rb") as handle:
        elf = ElfFile(handle.read())

    image = build(elf, args.name, flags, imports, args.stack)

    with open(args.output, "wb") as handle:
        handle.write(image)

    print(f"wrote {args.output} ({len(image)} bytes)")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
