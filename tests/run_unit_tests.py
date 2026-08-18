#!/usr/bin/env python3
"""
run_unit_tests.py - host-side unit tests for Qira OS.

Some of the operating system's logic is pure computation that does not need a
CPU in long mode to exercise: the ISO 9660 writer, the QiraFS packer, the
bootloader validator and the font generator all run on the build host. Testing
them here catches mistakes in seconds instead of after a 90-second boot.

Kernel code that genuinely needs hardware (the allocator, scheduler, drivers)
is covered instead by the in-kernel `selftest` command, which the boot tests
run inside the emulator.
"""

from __future__ import annotations

import os
import struct
import subprocess
import sys
import tempfile

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
sys.path.insert(0, os.path.join(ROOT, "tools"))

import isofs  # noqa: E402
import mkqirafs  # noqa: E402
import mkqac  # noqa: E402
import mklqx  # noqa: E402


class TestRunner:
    """Minimal test harness so the suite has no external dependencies."""

    def __init__(self) -> None:
        self.passed = 0
        self.failed = 0
        self.failures: list[str] = []

    def check(self, description: str, condition: bool, detail: str = "") -> None:
        if condition:
            self.passed += 1
            print(f"  \033[92mpass\033[0m  {description}")
        else:
            self.failed += 1
            self.failures.append(description)
            suffix = f"  ({detail})" if detail else ""
            print(f"  \033[91mFAIL\033[0m  {description}{suffix}")

    def equal(self, description: str, actual, expected) -> None:
        self.check(
            description, actual == expected, f"got {actual!r}, expected {expected!r}"
        )

    def section(self, title: str) -> None:
        print(f"\n\033[1m{title}\033[0m")

    def summary(self) -> int:
        total = self.passed + self.failed
        print(f"\n{total} tests: {self.passed} passed, {self.failed} failed")
        if self.failures:
            print("\nFailures:")
            for failure in self.failures:
                print(f"  - {failure}")
        return 1 if self.failed else 0


def test_isofs(t: TestRunner) -> None:
    t.section("ISO 9660 writer")

    t.equal("both-endian 16-bit encoding", isofs.both_endian16(1),
            b"\x01\x00\x00\x01")
    t.equal("both-endian 32-bit encoding", isofs.both_endian32(1),
            b"\x01\x00\x00\x00\x00\x00\x00\x01")
    t.equal("align_up rounds to a sector", isofs.align_up(1), 2048)
    t.equal("align_up leaves exact sizes", isofs.align_up(4096), 4096)
    t.equal("file identifiers get a version", isofs.iso_name("kernel.bin", False),
            "KERNEL.BIN;1")
    t.equal("directory identifiers are bare", isofs.iso_name("boot", True), "BOOT")
    t.equal("illegal characters are replaced", isofs.iso_name("a-b.txt", False),
            "A_B.TXT;1")

    builder = isofs.IsoBuilder(volume_id="QIRATEST")
    boot_payload = bytes(range(256)) * 8 + b"\x00" * (512 - 64)
    boot_payload = bytearray(boot_payload[:2048])
    boot_payload[510] = 0x55
    boot_payload[511] = 0xAA

    builder.add_boot_image("boot/loader.bin", bytes(boot_payload))
    kernel = builder.add_file("boot/kernel.bin", b"K" * 5000)
    builder.add_file("readme.txt", b"hello")
    builder.add_dir("docs")
    builder.add_file("docs/guide.txt", b"guide")

    image = builder.build()

    t.check("image is sector aligned", len(image) % 2048 == 0, str(len(image)))
    t.check("image has a plausible size", len(image) >= 32 * 2048, str(len(image)))
    t.equal("primary volume descriptor at sector 16",
            image[16 * 2048 : 16 * 2048 + 6], b"\x01CD001")
    t.equal("boot record at sector 17", image[17 * 2048 : 17 * 2048 + 6],
            b"\x00CD001")
    t.equal("supplementary descriptor at sector 18",
            image[18 * 2048 : 18 * 2048 + 6], b"\x02CD001")
    t.equal("terminator at sector 19", image[19 * 2048 : 19 * 2048 + 6],
            b"\xffCD001")
    t.equal("volume identifier recorded",
            image[16 * 2048 + 40 : 16 * 2048 + 48], b"QIRATEST")

    catalog = image[20 * 2048 : 20 * 2048 + 64]
    t.equal("boot catalogue validation entry", catalog[0], 0x01)
    t.equal("boot catalogue platform is x86", catalog[1], 0x00)
    t.equal("boot catalogue signature", catalog[30:32], b"\x55\xaa")
    t.equal("default entry is bootable", catalog[32], 0x88)
    t.equal("default entry is no-emulation", catalog[33], 0x00)

    checksum = sum(struct.unpack("<16H", catalog[0:32])) & 0xFFFF
    t.equal("validation entry checksum is correct", checksum, 0)

    boot_lba = struct.unpack("<I", catalog[40:44])[0]
    t.check("boot image LBA is inside the image", 0 < boot_lba < len(image) // 2048,
            str(boot_lba))

    t.equal("kernel contents are written",
            image[kernel.extent * 2048 : kernel.extent * 2048 + 4], b"KKKK")

    # The boot info table must be patched into the boot image.
    table = image[boot_lba * 2048 + 8 : boot_lba * 2048 + 24]
    pvd_lba, image_lba, length, _ = struct.unpack("<IIII", table)
    t.equal("boot info table records the PVD", pvd_lba, 16)
    t.equal("boot info table records its own LBA", image_lba, boot_lba)
    t.equal("boot info table records the length", length, len(boot_payload))


def test_qirafs(t: TestRunner) -> None:
    t.section("QiraFS packer")

    with tempfile.TemporaryDirectory() as tmp:
        os.makedirs(os.path.join(tmp, "etc"))
        os.makedirs(os.path.join(tmp, "bin"))
        os.makedirs(os.path.join(tmp, "home", "user"))

        with open(os.path.join(tmp, "etc", "motd"), "w") as handle:
            handle.write("welcome\n")
        with open(os.path.join(tmp, "home", "user", "notes.txt"), "w") as handle:
            handle.write("x" * 100)
        with open(os.path.join(tmp, "bin", "script"), "w") as handle:
            handle.write("echo hi\n")

        entries = mkqirafs.collect(tmp)
        image = mkqirafs.build(entries, "test")

        header = struct.unpack(mkqirafs.HEADER_FMT,
                               image[: mkqirafs.HEADER_SIZE])
        magic, version, count, total, data_offset, _, checksum, label = header

        t.equal("magic", magic, b"QIRAFS01")
        t.equal("version", version, 1)
        t.equal("entry count matches", count, len(entries))
        t.equal("total size matches the image", total, len(image))
        t.check("data offset is past the entry table",
                data_offset >= mkqirafs.HEADER_SIZE + mkqirafs.ENTRY_SIZE * count,
                str(data_offset))
        t.equal("label is recorded", label.rstrip(b"\x00"), b"test")

        # Recompute the checksum the way the kernel does.
        computed = 0
        for index in range(count):
            base = mkqirafs.HEADER_SIZE + index * mkqirafs.ENTRY_SIZE
            fields = struct.unpack(
                mkqirafs.ENTRY_FMT, image[base : base + mkqirafs.ENTRY_SIZE]
            )
            path, kind, _perms, size, offset, _mtime, _uid, _gid = fields
            if kind != mkqirafs.TYPE_FILE:
                continue
            start = data_offset + offset
            computed = (computed + sum(image[start : start + size])) & 0xFFFFFFFF

        t.equal("payload checksum verifies", computed, checksum)

        paths = []
        for index in range(count):
            base = mkqirafs.HEADER_SIZE + index * mkqirafs.ENTRY_SIZE
            fields = struct.unpack(
                mkqirafs.ENTRY_FMT, image[base : base + mkqirafs.ENTRY_SIZE]
            )
            paths.append(fields[0].rstrip(b"\x00").decode())

        t.check("motd is packed", "/etc/motd" in paths, str(paths))
        t.check("nested file is packed", "/home/user/notes.txt" in paths, str(paths))
        t.check("directories are packed", "/etc" in paths, str(paths))

        # Directories must come before the files inside them.
        etc_dir = paths.index("/etc")
        etc_file = paths.index("/etc/motd")
        t.check("directories precede their contents", etc_dir < etc_file,
                f"{etc_dir} vs {etc_file}")

        # Executables under /bin get the executable bit.
        for index in range(count):
            base = mkqirafs.HEADER_SIZE + index * mkqirafs.ENTRY_SIZE
            fields = struct.unpack(
                mkqirafs.ENTRY_FMT, image[base : base + mkqirafs.ENTRY_SIZE]
            )
            if fields[0].rstrip(b"\x00") == b"/bin/script":
                t.equal("files under /bin are executable", fields[2] & 0o111, 0o111)
                break


def test_empty_qirafs(t: TestRunner) -> None:
    t.section("QiraFS edge cases")

    image = mkqirafs.build([], "empty")
    header = struct.unpack(mkqirafs.HEADER_FMT, image[: mkqirafs.HEADER_SIZE])
    t.equal("an empty image is still valid", header[0], b"QIRAFS01")
    t.equal("an empty image has no entries", header[2], 0)
    t.equal("an empty image has a zero checksum", header[6], 0)


def test_checkboot(t: TestRunner) -> None:
    t.section("Bootloader validator")

    checkboot = os.path.join(ROOT, "tools", "checkboot.py")

    with tempfile.TemporaryDirectory() as tmp:
        # A valid image.
        good = bytearray(4096)
        good[510] = 0x55
        good[511] = 0xAA
        good[600:608] = b"QIRAPLD1"
        good_path = os.path.join(tmp, "good.bin")
        with open(good_path, "wb") as handle:
            handle.write(good)

        result = subprocess.run(
            [sys.executable, checkboot, good_path], capture_output=True, text=True
        )
        t.equal("accepts a valid loader", result.returncode, 0)

        # Missing signature.
        bad = bytearray(good)
        bad[510] = 0x00
        bad_path = os.path.join(tmp, "bad.bin")
        with open(bad_path, "wb") as handle:
            handle.write(bad)

        result = subprocess.run(
            [sys.executable, checkboot, bad_path], capture_output=True, text=True
        )
        t.equal("rejects a missing boot signature", result.returncode, 1)

        # Missing payload table.
        no_table = bytearray(good)
        no_table[600:608] = b"XXXXXXXX"
        no_table_path = os.path.join(tmp, "notable.bin")
        with open(no_table_path, "wb") as handle:
            handle.write(no_table)

        result = subprocess.run(
            [sys.executable, checkboot, no_table_path],
            capture_output=True,
            text=True,
        )
        t.equal("rejects a missing payload table", result.returncode, 1)


def test_grabframe(t: TestRunner) -> None:
    t.section("Frame decoder")

    sys.path.insert(0, os.path.join(ROOT, "tools"))
    import grabframe  # noqa: E402

    # Two runs: three red pixels then one blue one.
    encoded = bytes([3, 255, 0, 0, 1, 0, 0, 255])
    decoded = grabframe.decode_rle24(encoded, 4)
    t.equal("run-length decoding expands runs", decoded,
            b"\xff\x00\x00" * 3 + b"\x00\x00\xff")

    import base64

    payload = base64.b64encode(encoded).decode()
    log = (
        "some kernel output\n"
        "--QIRA-FRAME-BEGIN 2x2 rle24 test\n" + payload + "\n"
        "--QIRA-FRAME-END--\n"
        "more output\n"
    )
    frames = grabframe.extract(log)
    t.equal("one frame is found", len(frames), 1)
    if frames:
        t.equal("frame width", frames[0]["width"], 2)
        t.equal("frame height", frames[0]["height"], 2)
        t.equal("frame label", frames[0]["label"], "test")
        t.equal("frame payload size", len(frames[0]["data"]), 2 * 2 * 3)

    t.equal("a log with no frames yields nothing",
            len(grabframe.extract("nothing here")), 0)


def test_qac(t: TestRunner) -> None:
    t.section("QAC icon format")

    # A tiny image with two colours exercises every encoder.
    red, blue = 0xFFFF0000, 0xFF0000FF
    pixels = [red] * 8 + [blue] * 8

    raw = mkqac.encode_raw(pixels)
    t.equal("raw encoding is four bytes per pixel", len(raw), 16 * 4)

    rle = mkqac.encode_rle(pixels)
    t.equal("run-length collapses runs", len(rle), 2 * 5)

    indexed = mkqac.encode_indexed(pixels)
    t.check("palette encoding succeeds", indexed is not None)
    if indexed:
        data, palette_size = indexed
        t.equal("palette holds both colours", palette_size, 2)
        t.equal("palette encoding is a byte per pixel", len(data), 2 * 4 + 16)

    encoding, _data, _palette = mkqac.best_encoding(pixels)
    t.check("the smallest encoding is chosen",
            encoding in (mkqac.QAC_RLE, mkqac.QAC_INDEX), str(encoding))

    # An image needing more than 256 colours cannot be palettised.
    many = [i << 8 for i in range(300)]
    t.check("palette encoding declines past 256 colours",
            mkqac.encode_indexed(many) is None)

    # Build a real file and verify its structure.
    art = mkqac.ICONS["terminal"]
    full = mkqac.art_to_pixels(art, mkqac.PALETTE)
    t.equal("artwork is 32x32", len(full), 32 * 32)

    frames = {size: mkqac.scale(full, 32, size) for size in (32, 24, 16)}
    t.equal("scaling produces the right pixel count", len(frames[16]), 16 * 16)

    image = mkqac.build(frames, "terminal")

    header = struct.unpack(mkqac.HEADER_FMT, image[: mkqac.HEADER_SIZE])
    magic, version, count, payload, checksum, _flags, name = header

    t.equal("magic", magic, b"QACI")
    t.equal("version", version, 1)
    t.equal("frame count", count, 3)
    t.equal("name is recorded", name.rstrip(b"\x00"), b"terminal")

    body = image[mkqac.HEADER_SIZE + count * mkqac.ENTRY_SIZE :]
    t.equal("payload size matches the header", len(body), payload)
    t.equal("checksum verifies", sum(body) & 0xFFFFFFFF, checksum)

    # Frames must be ordered largest first, with offsets inside the payload.
    previous = 999
    for index in range(count):
        base = mkqac.HEADER_SIZE + index * mkqac.ENTRY_SIZE
        w, h, enc, _pal, _res, offset, size = struct.unpack(
            mkqac.ENTRY_FMT, image[base : base + mkqac.ENTRY_SIZE]
        )
        t.check(f"frame {index} is square ({w}x{h})", w == h)
        t.check(f"frame {index} is smaller than the last", w < previous)
        t.check(f"frame {index} lies inside the payload",
                offset + size <= payload)
        t.check(f"frame {index} uses a known encoding", enc in (0, 1, 2))
        previous = w

    # Decoding an RLE frame must reproduce the pixels exactly.
    sys.path.insert(0, os.path.join(ROOT, "tools"))
    import grabframe  # noqa: E402

    round_trip = mkqac.encode_rle(pixels)
    decoded = []
    for offset in range(0, len(round_trip), 5):
        run = round_trip[offset]
        colour = (
            round_trip[offset + 1]
            | (round_trip[offset + 2] << 8)
            | (round_trip[offset + 3] << 16)
            | (round_trip[offset + 4] << 24)
        )
        decoded += [colour] * run
    t.equal("run-length round trips exactly", decoded, pixels)
    UNUSED = grabframe  # keep the import meaningful


def test_lqx(t: TestRunner) -> None:
    t.section("LQX executable format")

    t.equal("QX header is 88 bytes", mklqx.HEADER_SIZE, 88)
    t.equal("section entry is 36 bytes", mklqx.SECTION_SIZE, 36)
    t.equal("import entry is 32 bytes", mklqx.IMPORT_SIZE, 32)
    t.equal("checksum sits at offset 56", mklqx.CHECKSUM_OFFSET, 56)

    # Build a real object with the host toolchain and link it.
    with tempfile.TemporaryDirectory() as tmp:
        source = os.path.join(tmp, "program.c")
        with open(source, "w") as handle:
            handle.write(
                "__attribute__((section(\".data\"))) void (*puts_fn)(const char *) = 0;\n"
                "int main(void) { if (puts_fn) puts_fn(\"hi\"); return 7; }\n"
            )

        obj = os.path.join(tmp, "program.o")
        elf = os.path.join(tmp, "program.elf")

        compile_result = subprocess.run(
            ["gcc", "-c", "-o", obj, source, "-ffreestanding", "-nostdlib",
             "-fno-pie", "-m64", "-O2"],
            capture_output=True, text=True,
        )
        if not t.equal("test program compiles", compile_result.returncode, 0):
            return

        link_result = subprocess.run(
            ["gcc", "-o", elf, obj, "-nostdlib", "-static", "-e", "main",
             "-Ttext=0x400000"],
            capture_output=True, text=True,
        )
        if not t.equal("test program links", link_result.returncode, 0):
            return

        with open(elf, "rb") as handle:
            parsed = mklqx.ElfFile(handle.read())

        t.check("ELF entry point was read", parsed.entry != 0)
        t.check("ELF has a text section", parsed.find(".text") is not None)

        image = mklqx.build(parsed, "test", mklqx.FLAG_EXECUTABLE, [], 0)

        fields = struct.unpack(mklqx.HEADER_FMT, image[: mklqx.HEADER_SIZE])
        (signature, fmt, version, machine, flags, header_size, total_size,
         entry, load_base, section_count, section_offset, import_count,
         _import_offset, symbol_count, _symbol_offset, checksum, _stack,
         name) = fields

        t.equal("signature", signature, b"QX")
        t.equal("format is linked", chr(fmt), "L")
        t.equal("version", version, 1)
        t.equal("machine is x86_64", machine, mklqx.LQX_MACHINE_X86_64)
        t.equal("marked executable", flags & mklqx.FLAG_EXECUTABLE,
                mklqx.FLAG_EXECUTABLE)
        t.equal("header size is recorded", header_size, mklqx.HEADER_SIZE)
        t.equal("total size matches the file", total_size, len(image))
        t.equal("name is recorded", name.rstrip(b"\x00"), b"test")
        t.check("entry point is set", entry != 0)
        t.check("sections were emitted", section_count >= 1)
        t.equal("no imports were requested", import_count, 0)
        t.check("symbols were captured", symbol_count >= 1)

        # The checksum must verify the way the kernel computes it.
        computed = 0
        for index, byte in enumerate(image):
            if mklqx.CHECKSUM_OFFSET <= index < mklqx.CHECKSUM_OFFSET + 4:
                continue
            computed = (computed + byte) & 0xFFFFFFFF
        t.equal("checksum verifies", computed, checksum)

        # Every section must lie inside the file, and none may be both
        # writable and executable.
        for index in range(section_count):
            base = section_offset + index * mklqx.SECTION_SIZE
            (sname, kind, sflags, _align, _addr, foff, fsize,
             msize) = struct.unpack_from(mklqx.SECTION_FMT, image, base)

            label = sname.rstrip(b"\x00").decode()
            t.check(f"section {label} has a known kind", 1 <= kind <= 5)
            t.check(f"section {label} fits in memory", msize >= fsize)
            if kind != 4:  # not BSS
                t.check(f"section {label} lies inside the file",
                        foff + fsize <= len(image))
            t.check(f"section {label} is not writable and executable",
                    not ((sflags & mklqx.P_WRITE) and (sflags & mklqx.P_EXEC)))

        # Imports must be recorded with their patch addresses.
        with_imports = mklqx.build(
            parsed, "test", mklqx.FLAG_EXECUTABLE,
            [("console_puts", 0x402000)], 0
        )
        fields = struct.unpack(mklqx.HEADER_FMT,
                               with_imports[: mklqx.HEADER_SIZE])
        t.equal("import count is recorded", fields[11], 1)

        iname, patch = struct.unpack_from(
            mklqx.IMPORT_FMT, with_imports, fields[12]
        )
        t.equal("import name", iname.rstrip(b"\x00"), b"console_puts")
        t.equal("import patch address", patch, 0x402000)


def test_fonts(t: TestRunner) -> None:
    t.section("Font generation")

    genfont_path = os.path.join(ROOT, "tools", "genfont.py")
    with tempfile.TemporaryDirectory() as tmp:
        output = os.path.join(tmp, "font_data.c")
        result = subprocess.run(
            [sys.executable, genfont_path, "--output", output],
            capture_output=True, text=True,
        )
        t.equal("generator exits cleanly", result.returncode, 0)

        with open(output) as handle:
            source = handle.read()

        t.check("registry is emitted", "qira_fonts[]" in source)
        t.check("face count is emitted", "qira_font_count" in source)

        for face in ["qira_sans", "qira_sans_bold", "qira_mono",
                     "qira_mono_bold"]:
            t.check(f"{face} glyphs are emitted", f"{face}_glyphs" in source)

        import re

        blocks = re.findall(r"\{((?:0x[0-9A-F]{2}, ?){15}0x[0-9A-F]{2})\}",
                            source)
        t.equal("four faces of 96 glyphs", len(blocks), 4 * 96)

        def glyph(face_index: int, char: str) -> list[int]:
            index = face_index * 96 + (ord(char) - 32)
            return [int(b, 16) for b in blocks[index].split(", ")]

        t.check("space is blank", all(b == 0 for b in glyph(0, " ")))
        t.check("A has ink", any(b != 0 for b in glyph(0, "A")))

        # A descender must reach below the baseline, which is row 13.
        g_glyph = glyph(0, "g")
        t.check("lowercase g has a descender", any(g_glyph[13:]))

        # Bold must be at least as heavy as regular, everywhere.
        regular = glyph(0, "B")
        bold = glyph(1, "B")
        t.check("bold is derived from regular",
                all((bold[i] & regular[i]) == regular[i]
                    for i in range(16)))
        t.check("bold is heavier",
                sum(bin(b).count("1") for b in bold)
                > sum(bin(b).count("1") for b in regular))

        # The mono zero is slashed, so it must differ from the sans zero.
        t.check("mono zero differs from sans zero",
                glyph(2, "0") != glyph(0, "0"))
        t.check("mono zero is slashed",
                sum(bin(b).count("1") for b in glyph(2, "0"))
                > sum(bin(b).count("1") for b in glyph(0, "0")))


def test_source_layout(t: TestRunner) -> None:
    """Guard the repository structure the build system depends on."""
    t.section("Repository layout")

    required = [
        "src/boot/stage1.S",
        "src/boot/stage2.S",
        "src/boot/bootinfo.h",
        "src/kernel/main.c",
        "src/kernel/arch/x86_64/kernel.ld",
        "src/kernel/arch/x86_64/entry.S",
        "src/user/shells/qcsh/qcsh.c",
        "src/user/shells/ultrashell/ultrashell.c",
        "src/user/desktop/desktop.c",
        "tools/mkiso.py",
        "tools/mkqirafs.py",
        "Makefile",
        "LICENSE",
        "README.md",
    ]

    for path in required:
        t.check(f"{path} exists", os.path.isfile(os.path.join(ROOT, path)))

    # No single source file should dominate the codebase.
    sources = []
    for directory, _dirs, files in os.walk(os.path.join(ROOT, "src")):
        for name in files:
            if name.endswith((".c", ".S", ".h")):
                full = os.path.join(directory, name)
                sources.append((full, sum(1 for _ in open(full, errors="replace"))))

    t.check("the source tree is split into many files", len(sources) >= 30,
            f"{len(sources)} files")

    total_lines = sum(lines for _, lines in sources)
    largest = max(sources, key=lambda item: item[1])
    share = largest[1] / max(total_lines, 1)
    t.check(
        "no file holds more than 15% of the code",
        share < 0.15,
        f"{os.path.relpath(largest[0], ROOT)} is {share:.0%}",
    )


def main() -> int:
    print("\033[1mQira OS host unit tests\033[0m")

    t = TestRunner()
    test_isofs(t)
    test_qirafs(t)
    test_empty_qirafs(t)
    test_fonts(t)
    test_qac(t)
    test_lqx(t)
    test_checkboot(t)
    test_grabframe(t)
    test_source_layout(t)

    return t.summary()


if __name__ == "__main__":
    raise SystemExit(main())
