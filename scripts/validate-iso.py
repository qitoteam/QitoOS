#!/usr/bin/env python3
"""
validate-iso.py - verify that a built Qira OS image is actually bootable.

Checks the structural invariants that decide whether a PC firmware will boot
the disc, so a broken image is caught in CI instead of in a virtual machine:

  * the volume descriptor set is well formed,
  * an El Torito boot record and catalogue are present and self-consistent,
  * the boot image carries the 0xAA55 signature at the right offset,
  * the payload table names sectors that exist inside the image,
  * the kernel and ramdisk at those sectors look like what they claim to be.
"""

from __future__ import annotations

import argparse
import struct
import sys

SECTOR = 2048
QIRAFS_MAGIC = b"QIRAFS01"
PAYLOAD_SIGNATURE = b"QIRAPLD1"


class Validator:
    def __init__(self, verbose: bool = False) -> None:
        self.errors: list[str] = []
        self.warnings: list[str] = []
        self.verbose = verbose

    def check(self, description: str, condition: bool, detail: str = "") -> bool:
        if condition:
            if self.verbose:
                print(f"  ok    {description}")
        else:
            suffix = f": {detail}" if detail else ""
            self.errors.append(f"{description}{suffix}")
            print(f"  FAIL  {description}{suffix}")
        return condition

    def warn(self, description: str, condition: bool) -> None:
        if not condition:
            self.warnings.append(description)
            print(f"  warn  {description}")


def validate(path: str, verbose: bool) -> int:
    with open(path, "rb") as handle:
        image = handle.read()

    v = Validator(verbose)
    sectors = len(image) // SECTOR

    print(f"Validating {path}")
    print(f"  size: {len(image)} bytes ({sectors} sectors)")

    # --- basic geometry ---------------------------------------------------
    v.check("image is a whole number of sectors", len(image) % SECTOR == 0,
            f"{len(image) % SECTOR} bytes over")
    v.check("image is large enough to be an ISO", sectors >= 24, f"{sectors} sectors")

    if len(image) < 24 * SECTOR:
        print("\nimage is too small to inspect further")
        return 1

    # --- system area ------------------------------------------------------
    v.check("system area is reserved", len(image[: 16 * SECTOR]) == 16 * SECTOR)

    # --- primary volume descriptor ---------------------------------------
    pvd = image[16 * SECTOR : 17 * SECTOR]
    v.check("primary volume descriptor type", pvd[0] == 1, f"got {pvd[0]}")
    v.check("primary volume descriptor identifier", pvd[1:6] == b"CD001",
            repr(pvd[1:6]))
    v.check("primary volume descriptor version", pvd[6] == 1)

    volume_id = pvd[40:72].decode("ascii", "replace").strip()
    print(f"  volume id: {volume_id}")
    v.check("volume identifier is set", len(volume_id) > 0)

    recorded_sectors = struct.unpack("<I", pvd[80:84])[0]
    v.check("recorded volume size matches the file", recorded_sectors == sectors,
            f"header says {recorded_sectors}, file has {sectors}")

    block_size = struct.unpack("<H", pvd[128:130])[0]
    v.check("logical block size is 2048", block_size == SECTOR, str(block_size))

    # --- boot record ------------------------------------------------------
    boot_record = image[17 * SECTOR : 18 * SECTOR]
    v.check("boot record descriptor type", boot_record[0] == 0)
    v.check("boot record identifier", boot_record[1:6] == b"CD001")
    v.check("El Torito boot system identifier",
            boot_record[7:30] == b"EL TORITO SPECIFICATION",
            repr(boot_record[7:30]))

    catalog_lba = struct.unpack("<I", boot_record[71:75])[0]
    print(f"  boot catalogue: LBA {catalog_lba}")
    v.check("boot catalogue is inside the image", 0 < catalog_lba < sectors,
            str(catalog_lba))

    # --- supplementary descriptor and terminator -------------------------
    svd = image[18 * SECTOR : 19 * SECTOR]
    v.check("supplementary volume descriptor (Joliet)", svd[0] == 2)
    v.check("Joliet escape sequence", svd[88:91] == b"%/E", repr(svd[88:91]))

    terminator = image[19 * SECTOR : 20 * SECTOR]
    v.check("volume descriptor set terminator", terminator[0] == 0xFF)
    v.check("terminator identifier", terminator[1:6] == b"CD001")

    if not (0 < catalog_lba < sectors):
        print("\ncannot inspect the boot catalogue")
        return 1

    # --- boot catalogue ---------------------------------------------------
    catalog = image[catalog_lba * SECTOR : catalog_lba * SECTOR + 64]

    v.check("validation entry header", catalog[0] == 0x01, hex(catalog[0]))
    v.check("platform is 80x86", catalog[1] == 0x00, hex(catalog[1]))
    v.check("validation entry key bytes", catalog[30:32] == b"\x55\xaa",
            repr(catalog[30:32]))

    checksum = sum(struct.unpack("<16H", catalog[0:32])) & 0xFFFF
    v.check("validation entry checksum is zero", checksum == 0, hex(checksum))

    v.check("default entry is bootable", catalog[32] == 0x88, hex(catalog[32]))
    v.check("default entry uses no emulation", catalog[33] == 0x00,
            hex(catalog[33]))

    load_segment = struct.unpack("<H", catalog[34:36])[0]
    v.check("load segment is the default", load_segment in (0, 0x7C0),
            hex(load_segment))

    load_sectors = struct.unpack("<H", catalog[38:40])[0]
    boot_lba = struct.unpack("<I", catalog[40:44])[0]
    print(f"  boot image: LBA {boot_lba}, {load_sectors} virtual sectors")

    v.check("boot image LBA is inside the image", 0 < boot_lba < sectors,
            str(boot_lba))
    v.check("boot image load size is plausible", 1 <= load_sectors <= 512,
            str(load_sectors))

    if not (0 < boot_lba < sectors):
        print("\ncannot inspect the boot image")
        return 1

    # --- boot image -------------------------------------------------------
    boot = image[boot_lba * SECTOR : boot_lba * SECTOR + load_sectors * 512]

    v.check("boot image has the 0xAA55 signature",
            len(boot) >= 512 and boot[510] == 0x55 and boot[511] == 0xAA,
            f"{boot[510]:#04x} {boot[511]:#04x}" if len(boot) >= 512 else "too short")

    v.check("boot image is not blank", any(b != 0 for b in boot[:64]))

    # The whole loader must be pulled in by the firmware, not just sector one.
    v.check("load size covers the whole loader", load_sectors >= 2,
            f"{load_sectors} sectors would load only stage 1")

    payload_offset = boot.find(PAYLOAD_SIGNATURE)
    if v.check("payload descriptor table is present", payload_offset >= 0):
        fields = struct.unpack_from("<IIIIII", boot, payload_offset + 8)
        kernel_lba, kernel_sectors, kernel_size = fields[0], fields[1], fields[2]
        ramdisk_lba, ramdisk_sectors, ramdisk_size = fields[3], fields[4], fields[5]

        print(f"  kernel: LBA {kernel_lba}, {kernel_size} bytes "
              f"({kernel_sectors} sectors)")
        print(f"  ramdisk: LBA {ramdisk_lba}, {ramdisk_size} bytes "
              f"({ramdisk_sectors} sectors)")

        v.check("kernel LBA is inside the image", 0 < kernel_lba < sectors,
                str(kernel_lba))
        v.check("kernel has a plausible size", 4096 < kernel_size < 16 * 1024 * 1024,
                str(kernel_size))
        v.check("kernel sector count matches its size",
                kernel_sectors == (kernel_size + SECTOR - 1) // SECTOR,
                f"{kernel_sectors} vs {(kernel_size + SECTOR - 1) // SECTOR}")
        v.check("kernel fits inside the image",
                kernel_lba + kernel_sectors <= sectors)

        # The kernel is a flat binary; its first bytes must not be zero.
        kernel_head = image[kernel_lba * SECTOR : kernel_lba * SECTOR + 16]
        v.check("kernel image is not blank", any(b != 0 for b in kernel_head))

        if ramdisk_sectors:
            v.check("ramdisk LBA is inside the image", 0 < ramdisk_lba < sectors)
            v.check("ramdisk fits inside the image",
                    ramdisk_lba + ramdisk_sectors <= sectors)

            ramdisk_head = image[ramdisk_lba * SECTOR : ramdisk_lba * SECTOR + 8]
            v.check("ramdisk carries the QiraFS magic",
                    ramdisk_head == QIRAFS_MAGIC, repr(ramdisk_head))

            recorded = struct.unpack(
                "<Q", image[ramdisk_lba * SECTOR + 16 : ramdisk_lba * SECTOR + 24]
            )[0]
            v.check("ramdisk header size matches the payload table",
                    recorded == ramdisk_size, f"{recorded} vs {ramdisk_size}")
        else:
            v.warn("image has no ramdisk", False)

        # The kernel command line must have been patched in.
        cmdline_marker = boot.find(b"QIRACMDLINE:")
        v.check("kernel command line was patched", cmdline_marker < 0,
                "the placeholder marker is still present")

    # --- summary ----------------------------------------------------------
    print()
    if v.errors:
        print(f"\033[91mFAILED\033[0m: {len(v.errors)} problem(s) found")
        for error in v.errors:
            print(f"  - {error}")
        return 1

    if v.warnings:
        print(f"{len(v.warnings)} warning(s)")

    print("\033[92mPASSED\033[0m: the image is structurally bootable")
    return 0


def main() -> int:
    parser = argparse.ArgumentParser(description="Validate a Qira OS ISO image")
    parser.add_argument("--iso", required=True)
    parser.add_argument("--verbose", action="store_true")
    args = parser.parse_args()

    try:
        return validate(args.iso, args.verbose)
    except FileNotFoundError:
        print(f"error: {args.iso} does not exist", file=sys.stderr)
        return 2


if __name__ == "__main__":
    raise SystemExit(main())
