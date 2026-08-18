#!/usr/bin/env python3
"""
mkiso.py - build the bootable Qira OS ISO image.

Takes the flat bootloader blob, the flattened kernel image and the QiraFS
ramdisk, lays them out on an ISO 9660 filesystem, patches the bootloader's
payload table so stage 2 knows where to find everything, and writes an El
Torito no-emulation bootable image.

Usage:
    mkiso.py --boot build/boot.bin --kernel build/qira-kernel.bin \\
             --ramdisk build/qirafs.img --output build/qira-os.iso
"""

from __future__ import annotations

import argparse
import os
import struct
import sys

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))

from isofs import SECTOR, IsoBuilder, align_up  # noqa: E402

PAYLOAD_SIGNATURE = b"QIRAPLD1"
CMDLINE_MARKER = b"QIRACMDLINE:"
CMDLINE_SIZE = 256


def find_payload_table(boot: bytes) -> int:
    """Locate the payload descriptor stage 2 exposes for the ISO builder."""
    offset = boot.find(PAYLOAD_SIGNATURE)
    if offset < 0:
        raise SystemExit(
            "error: payload table signature not found in the bootloader image"
        )
    if boot.find(PAYLOAD_SIGNATURE, offset + 1) >= 0:
        raise SystemExit("error: payload table signature is ambiguous")
    return offset


def patch_payload_table(
    boot: bytearray,
    offset: int,
    kernel_lba: int,
    kernel_sectors: int,
    kernel_size: int,
    ramdisk_lba: int,
    ramdisk_sectors: int,
    ramdisk_size: int,
) -> None:
    """Fill in the LBA/length fields stage 2 reads at boot time."""
    struct.pack_into(
        "<IIIIIII",
        boot,
        offset + 8,
        kernel_lba,
        kernel_sectors,
        kernel_size,
        ramdisk_lba,
        ramdisk_sectors,
        ramdisk_size,
        0,
    )


def patch_cmdline(boot: bytearray, cmdline: str) -> bool:
    """
    Overwrite the kernel command line embedded in stage 2.

    The loader reserves a 128-byte field that begins with a marker string; the
    whole field (marker included) is replaced with the NUL-terminated command
    line, so the kernel receives exactly what was requested at build time.
    """
    offset = boot.find(CMDLINE_MARKER)
    if offset < 0:
        return False

    encoded = cmdline.encode("ascii", "ignore")[: CMDLINE_SIZE - 1]
    boot[offset : offset + CMDLINE_SIZE] = encoded.ljust(CMDLINE_SIZE, b"\x00")
    return True


def read_file(path: str) -> bytes:
    with open(path, "rb") as handle:
        return handle.read()


def main() -> int:
    parser = argparse.ArgumentParser(description="Build the Qira OS ISO image")
    parser.add_argument("--boot", required=True, help="flat bootloader binary")
    parser.add_argument("--kernel", required=True, help="flat kernel image")
    parser.add_argument("--ramdisk", help="QiraFS ramdisk image (optional)")
    parser.add_argument("--output", required=True, help="ISO file to write")
    parser.add_argument("--volume-id", default="QIRAOS", help="ISO volume label")
    parser.add_argument(
        "--version", default="dev", help="version string recorded on the image"
    )
    parser.add_argument(
        "--cmdline",
        default="root=qirafs console=fb",
        help="kernel command line embedded in the bootloader",
    )
    args = parser.parse_args()

    boot = bytearray(read_file(args.boot))
    kernel = read_file(args.kernel)
    ramdisk = read_file(args.ramdisk) if args.ramdisk else b""

    if len(boot) < 512 or boot[510] != 0x55 or boot[511] != 0xAA:
        raise SystemExit("error: bootloader is missing the 0xAA55 boot signature")

    table_offset = find_payload_table(boot)

    if not patch_cmdline(boot, args.cmdline):
        raise SystemExit("error: kernel command line marker not found in the loader")

    builder = IsoBuilder(volume_id=args.volume_id, app_id="QIRA OS")

    # The boot image must be added first so that later passes can find it.
    builder.add_boot_image("boot/qiraboot.bin", bytes(boot))
    kernel_node = builder.add_file("boot/qkernel.bin", kernel)
    ramdisk_node = builder.add_file("boot/qirafs.img", ramdisk) if ramdisk else None

    # A couple of human-readable artefacts so a mounted ISO is self-describing.
    builder.add_file(
        "readme.txt",
        (
            "Qira OS %s\r\n"
            "A from-scratch x86_64 operating system.\r\n\r\n"
            "This disc is bootable on BIOS machines and virtual machines.\r\n"
            "Boot it with:\r\n"
            "    qemu-system-x86_64 -cdrom qira-os.iso -m 512\r\n\r\n"
            "https://github.com/Seigh-sword/QiraOS\r\n" % args.version
        ).encode("ascii"),
    )
    builder.add_file(
        "boot/version.txt", ("qira-os %s\r\n" % args.version).encode("ascii")
    )

    # First pass: lay the image out so the payload extents are known.
    builder.build()

    kernel_lba = kernel_node.extent
    kernel_sectors = align_up(len(kernel)) // SECTOR
    ramdisk_lba = ramdisk_node.extent if ramdisk_node else 0
    ramdisk_sectors = align_up(len(ramdisk)) // SECTOR if ramdisk_node else 0

    patch_payload_table(
        boot,
        table_offset,
        kernel_lba,
        kernel_sectors,
        len(kernel),
        ramdisk_lba,
        ramdisk_sectors,
        len(ramdisk),
    )

    # Second pass with the patched loader. Sizes are unchanged, so the layout
    # computed above still holds.
    builder2 = IsoBuilder(volume_id=args.volume_id, app_id="QIRA OS")
    builder2.add_boot_image("boot/qiraboot.bin", bytes(boot))
    kernel_node2 = builder2.add_file("boot/qkernel.bin", kernel)
    ramdisk_node2 = builder2.add_file("boot/qirafs.img", ramdisk) if ramdisk else None
    builder2.add_file(
        "readme.txt",
        (
            "Qira OS %s\r\n"
            "A from-scratch x86_64 operating system.\r\n\r\n"
            "This disc is bootable on BIOS machines and virtual machines.\r\n"
            "Boot it with:\r\n"
            "    qemu-system-x86_64 -cdrom qira-os.iso -m 512\r\n\r\n"
            "https://github.com/Seigh-sword/QiraOS\r\n" % args.version
        ).encode("ascii"),
    )
    builder2.add_file(
        "boot/version.txt", ("qira-os %s\r\n" % args.version).encode("ascii")
    )

    image = builder2.build()

    # Sanity check: the second layout must match the first.
    if kernel_node2.extent != kernel_lba:
        raise SystemExit("error: ISO layout was not stable between passes")
    if ramdisk_node2 is not None and ramdisk_node2.extent != ramdisk_lba:
        raise SystemExit("error: ramdisk layout was not stable between passes")

    os.makedirs(os.path.dirname(os.path.abspath(args.output)), exist_ok=True)
    with open(args.output, "wb") as handle:
        handle.write(image)

    print("Qira OS image: %s" % args.output)
    print("  volume id      : %s" % args.volume_id)
    print("  total size     : %d bytes (%d sectors)" % (len(image), len(image) // SECTOR))
    print("  bootloader     : %d bytes" % len(boot))
    print("  command line   : %s" % args.cmdline)
    print("  kernel         : LBA %d, %d bytes" % (kernel_lba, len(kernel)))
    if ramdisk:
        print("  ramdisk        : LBA %d, %d bytes" % (ramdisk_lba, len(ramdisk)))
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
