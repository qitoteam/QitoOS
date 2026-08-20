#!/usr/bin/env python3
"""
isofs.py - a small, dependency-free ISO 9660 + El Torito image writer.

QitoOS ships its own ISO generator so the build does not depend on
``xorriso``/``genisoimage`` being installed. The writer implements the subset
of ECMA-119 (ISO 9660) that PC firmware and common tooling actually need:

  * a Primary Volume Descriptor with a correct path table and directory tree,
  * Joliet (supplementary volume descriptor, UCS-2 names) for long file names,
  * an El Torito boot catalogue with a no-emulation boot entry,
  * the boot-info table patch that lets a boot image discover its own LBA.

The image is laid out as:

    sector 0..15      system area (zero)
    sector 16         Primary Volume Descriptor
    sector 17         Boot Record Volume Descriptor (El Torito)
    sector 18         Supplementary Volume Descriptor (Joliet)
    sector 19         Volume Descriptor Set Terminator
    sector 20         El Torito boot catalogue
    ...               path tables, directory records, file data

Only what QitoOS needs is implemented; this is deliberately not a general
purpose mastering tool.
"""

from __future__ import annotations

import os
import struct
import time
from dataclasses import dataclass, field
from typing import Dict, List, Optional

SECTOR = 2048


def align_up(value: int, boundary: int = SECTOR) -> int:
    """Round *value* up to the next multiple of *boundary*."""
    return (value + boundary - 1) // boundary * boundary


def both_endian16(value: int) -> bytes:
    """ISO 9660 stores 16-bit numbers in both byte orders."""
    return struct.pack("<H", value) + struct.pack(">H", value)


def both_endian32(value: int) -> bytes:
    """ISO 9660 stores 32-bit numbers in both byte orders."""
    return struct.pack("<I", value) + struct.pack(">I", value)


def iso_datetime(stamp: Optional[float] = None) -> bytes:
    """17-byte ``dec-datetime`` field used by volume descriptors."""
    t = time.gmtime(stamp if stamp is not None else time.time())
    text = "%04d%02d%02d%02d%02d%02d00" % (
        t.tm_year,
        t.tm_mon,
        t.tm_mday,
        t.tm_hour,
        t.tm_min,
        t.tm_sec,
    )
    return text.encode("ascii") + b"\x00"


def dir_datetime(stamp: Optional[float] = None) -> bytes:
    """7-byte directory-record timestamp."""
    t = time.gmtime(stamp if stamp is not None else time.time())
    return bytes(
        [
            max(0, min(255, t.tm_year - 1900)),
            t.tm_mon,
            t.tm_mday,
            t.tm_hour,
            t.tm_min,
            t.tm_sec,
            0,
        ]
    )


def pad_str(text: str, length: int) -> bytes:
    """Space-padded a-characters field."""
    return text.encode("ascii", "replace")[:length].ljust(length, b" ")


def pad_ucs2(text: str, length: int) -> bytes:
    """Space-padded UCS-2BE field used by the Joliet descriptor."""
    return text.encode("utf-16-be")[:length].ljust(length, b"\x00")


def iso_name(name: str, is_dir: bool) -> str:
    """
    Map a POSIX file name onto a conservative ISO 9660 level-1 identifier.

    Directories get an 8 character name, files get ``NAME.EXT;1``.
    """
    allowed = "ABCDEFGHIJKLMNOPQRSTUVWXYZ0123456789_"

    def sanitise(text: str) -> str:
        return "".join(c if c in allowed else "_" for c in text.upper())

    if is_dir:
        return sanitise(name)[:8] or "_"

    # Split on the final dot *before* sanitising, otherwise the separator
    # itself would be replaced and the extension lost.
    stem, dot, ext = name.rpartition(".")
    if not dot:
        stem, ext = name, ""

    stem = sanitise(stem)[:8] or "_"
    ext = sanitise(ext)[:3]
    return f"{stem}.{ext};1" if ext else f"{stem}.;1"


@dataclass
class Node:
    """A file or directory in the image being built."""

    name: str
    is_dir: bool
    data: bytes = b""
    children: List["Node"] = field(default_factory=list)

    # Filled in during layout.
    extent: int = 0
    length: int = 0
    dir_number: int = 0
    parent_number: int = 1

    def add(self, child: "Node") -> "Node":
        self.children.append(child)
        return child

    def sorted_children(self) -> List["Node"]:
        # ISO 9660 requires directory records sorted by identifier.
        return sorted(self.children, key=lambda n: iso_name(n.name, n.is_dir))


class IsoBuilder:
    """Assembles an ISO 9660 image from an in-memory tree."""

    def __init__(self, volume_id: str = "QITOOS", app_id: str = "QITO OS") -> None:
        self.root = Node("", True)
        self.volume_id = volume_id
        self.app_id = app_id
        self.boot_image: Optional[str] = None
        self.boot_load_size: int = 4
        self.patch_boot_info: bool = True
        self._timestamp = time.time()

    # -- tree construction -------------------------------------------------
    def _walk_to(self, path: str, create: bool) -> Node:
        node = self.root
        parts = [p for p in path.strip("/").split("/") if p]
        for part in parts:
            match = next(
                (c for c in node.children if c.is_dir and c.name == part), None
            )
            if match is None:
                if not create:
                    raise KeyError(f"no such directory: {path}")
                match = node.add(Node(part, True))
            node = match
        return node

    def add_dir(self, path: str) -> Node:
        """Create (or fetch) a directory at *path*."""
        return self._walk_to(path, create=True)

    def add_file(self, path: str, data: bytes) -> Node:
        """Add a file containing *data* at *path*."""
        directory, _, name = path.strip("/").rpartition("/")
        parent = self._walk_to(directory, create=True) if directory else self.root
        node = Node(name, False, data)
        node.length = len(data)
        parent.add(node)
        return node

    def add_boot_image(
        self, path: str, data: bytes, load_size: Optional[int] = None
    ) -> Node:
        """
        Add *data* as the El Torito no-emulation boot image.

        ``load_size`` is the number of 512-byte virtual sectors the firmware
        must pull in at 0x7C00. When omitted it is derived from the image size
        so the whole loader (stage 1 *and* stage 2) is resident before control
        transfers to it.
        """
        node = self.add_file(path, data)
        self.boot_image = path.strip("/")
        self.boot_load_size = (
            load_size if load_size is not None else (len(data) + 511) // 512
        )
        return node

    # -- layout ------------------------------------------------------------
    def _all_dirs(self) -> List[Node]:
        """Breadth-first list of directories; the path table needs this order."""
        out: List[Node] = [self.root]
        index = 0
        while index < len(out):
            current = out[index]
            index += 1
            for child in current.sorted_children():
                if child.is_dir:
                    out.append(child)
        for number, node in enumerate(out, start=1):
            node.dir_number = number
        for node in out:
            for child in node.children:
                if child.is_dir:
                    child.parent_number = node.dir_number
        return out

    def _dir_record(
        self, node: Node, joliet: bool, special: Optional[int] = None
    ) -> bytes:
        """
        Build one directory record.

        *special* is 0 for the "." entry and 1 for the ".." entry, which use a
        single zero/one byte identifier instead of a name.
        """
        if special is not None:
            identifier = bytes([special])
        elif joliet:
            identifier = node.name.encode("utf-16-be")
        else:
            identifier = iso_name(node.name, node.is_dir).encode("ascii")

        length = 33 + len(identifier)
        if length % 2:
            length += 1  # pad byte keeps records 16-bit aligned

        flags = 0x02 if node.is_dir else 0x00
        size = node.length if not node.is_dir else node.length

        record = bytearray()
        record.append(length)
        record.append(0)  # extended attribute length
        record += both_endian32(node.extent)
        record += both_endian32(size)
        record += dir_datetime(self._timestamp)
        record.append(flags)
        record.append(0)  # file unit size
        record.append(0)  # interleave gap
        record += both_endian16(1)  # volume sequence number
        record.append(len(identifier))
        record += identifier
        while len(record) < length:
            record.append(0)
        return bytes(record)

    def _dir_size(self, node: Node, joliet: bool) -> int:
        """Total byte size of a directory's records, rounded to a sector."""
        total = len(self._dir_record(node, joliet, special=0))
        total += len(self._dir_record(node, joliet, special=1))
        for child in node.sorted_children():
            total += len(self._dir_record(child, joliet))
        return align_up(total)

    def _path_table(self, dirs: List[Node], joliet: bool, little: bool) -> bytes:
        """Serialise the L-path or M-path table."""
        out = bytearray()
        for node in dirs:
            if node is self.root:
                identifier = b"\x00"
            elif joliet:
                identifier = node.name.encode("utf-16-be")
            else:
                identifier = iso_name(node.name, True).encode("ascii")
            extent = node.joliet_extent if joliet else node.extent  # type: ignore[attr-defined]
            order = "<" if little else ">"
            out.append(len(identifier))
            out.append(0)
            out += struct.pack(order + "I", extent)
            out += struct.pack(order + "H", node.parent_number)
            out += identifier
            if len(identifier) % 2:
                out.append(0)
        return bytes(out)

    def build(self) -> bytes:
        """Render the complete image and return it as bytes."""
        dirs = self._all_dirs()
        files = [
            node
            for node in self._iter_nodes(self.root)
            if not node.is_dir
        ]

        # Sector 0-15 system area, 16-19 volume descriptors, 20 boot catalogue.
        boot_catalog_lba = 20
        cursor = 21

        # -- primary (ISO 9660) directory extents --
        for node in dirs:
            node.length = self._dir_size(node, joliet=False)
            node.extent = cursor
            cursor += node.length // SECTOR

        # -- Joliet directory extents (a parallel tree over the same files) --
        for node in dirs:
            node.joliet_length = self._dir_size(node, joliet=True)  # type: ignore[attr-defined]
            node.joliet_extent = cursor  # type: ignore[attr-defined]
            cursor += node.joliet_length // SECTOR  # type: ignore[attr-defined]

        # -- path tables --
        pt_l = self._path_table(dirs, joliet=False, little=True)
        pt_m = self._path_table(dirs, joliet=False, little=False)
        jpt_l = self._path_table(dirs, joliet=True, little=True)
        jpt_m = self._path_table(dirs, joliet=True, little=False)

        pt_l_lba, cursor = cursor, cursor + align_up(len(pt_l)) // SECTOR
        pt_m_lba, cursor = cursor, cursor + align_up(len(pt_m)) // SECTOR
        jpt_l_lba, cursor = cursor, cursor + align_up(len(jpt_l)) // SECTOR
        jpt_m_lba, cursor = cursor, cursor + align_up(len(jpt_m)) // SECTOR

        # -- file data --
        for node in files:
            node.extent = cursor
            node.length = len(node.data)
            cursor += align_up(len(node.data)) // SECTOR

        total_sectors = max(cursor, 32)

        image = bytearray(total_sectors * SECTOR)

        def put(lba: int, payload: bytes) -> None:
            image[lba * SECTOR : lba * SECTOR + len(payload)] = payload

        # -- file contents (written first so the boot image can be patched) --
        boot_node: Optional[Node] = None
        for node in files:
            put(node.extent, node.data)
            if self.boot_image and self._path_of(node) == self.boot_image:
                boot_node = node

        # -- directory records --
        for node in dirs:
            self._write_dir(image, node, dirs, joliet=False)
            self._write_dir(image, node, dirs, joliet=True)

        put(pt_l_lba, pt_l)
        put(pt_m_lba, pt_m)
        put(jpt_l_lba, jpt_l)
        put(jpt_m_lba, jpt_m)

        # -- volume descriptors --
        put(16, self._pvd(total_sectors, len(pt_l), pt_l_lba, pt_m_lba))
        put(17, self._boot_record(boot_catalog_lba))
        put(18, self._svd(total_sectors, len(jpt_l), jpt_l_lba, jpt_m_lba))
        put(19, self._terminator())

        # -- El Torito boot catalogue --
        if boot_node is not None:
            put(boot_catalog_lba, self._boot_catalog(boot_node.extent))
            if self.patch_boot_info:
                self._patch_boot_info_table(image, boot_node, total_sectors)

        return bytes(image)

    # -- helpers -----------------------------------------------------------
    def _iter_nodes(self, node: Node):
        for child in node.sorted_children():
            yield child
            if child.is_dir:
                yield from self._iter_nodes(child)

    def _path_of(self, target: Node) -> str:
        def search(node: Node, prefix: str) -> Optional[str]:
            for child in node.children:
                path = f"{prefix}/{child.name}" if prefix else child.name
                if child is target:
                    return path
                if child.is_dir:
                    found = search(child, path)
                    if found:
                        return found
            return None

        return search(self.root, "") or ""

    def _write_dir(
        self, image: bytearray, node: Node, dirs: List[Node], joliet: bool
    ) -> None:
        extent = node.joliet_extent if joliet else node.extent  # type: ignore[attr-defined]
        length = node.joliet_length if joliet else node.length  # type: ignore[attr-defined]

        parent = next((d for d in dirs if d.dir_number == node.parent_number), self.root)

        # "." refers to this directory, ".." to the parent.
        self_node = Node(node.name, True)
        self_node.extent = extent
        self_node.length = length
        parent_node = Node(parent.name, True)
        parent_node.extent = (
            parent.joliet_extent if joliet else parent.extent  # type: ignore[attr-defined]
        )
        parent_node.length = (
            parent.joliet_length if joliet else parent.length  # type: ignore[attr-defined]
        )

        buf = bytearray()
        buf += self._dir_record(self_node, joliet, special=0)
        buf += self._dir_record(parent_node, joliet, special=1)

        for child in node.sorted_children():
            entry = Node(child.name, child.is_dir)
            if child.is_dir:
                entry.extent = (
                    child.joliet_extent if joliet else child.extent  # type: ignore[attr-defined]
                )
                entry.length = (
                    child.joliet_length if joliet else child.length  # type: ignore[attr-defined]
                )
            else:
                entry.extent = child.extent
                entry.length = child.length
            record = self._dir_record(entry, joliet)
            # A directory record may not straddle a sector boundary.
            if (len(buf) % SECTOR) + len(record) > SECTOR:
                buf += b"\x00" * (SECTOR - len(buf) % SECTOR)
            buf += record

        image[extent * SECTOR : extent * SECTOR + len(buf)] = buf

    def _root_record(self, joliet: bool) -> bytes:
        node = Node("", True)
        node.extent = (
            self.root.joliet_extent if joliet else self.root.extent  # type: ignore[attr-defined]
        )
        node.length = (
            self.root.joliet_length if joliet else self.root.length  # type: ignore[attr-defined]
        )
        return self._dir_record(node, joliet, special=0)

    def _pvd(
        self, total_sectors: int, pt_size: int, pt_l_lba: int, pt_m_lba: int
    ) -> bytes:
        d = bytearray(SECTOR)
        d[0] = 1  # primary volume descriptor
        d[1:6] = b"CD001"
        d[6] = 1
        d[8:40] = pad_str("", 32)  # system identifier
        d[40:72] = pad_str(self.volume_id, 32)
        d[80:88] = both_endian32(total_sectors)
        d[120:124] = both_endian16(1)  # volume set size
        d[124:128] = both_endian16(1)  # volume sequence number
        d[128:132] = both_endian16(SECTOR)
        d[132:140] = both_endian32(pt_size)
        d[140:144] = struct.pack("<I", pt_l_lba)
        d[144:148] = struct.pack("<I", 0)  # optional L path table
        d[148:152] = struct.pack(">I", pt_m_lba)
        d[152:156] = struct.pack(">I", 0)  # optional M path table
        d[156:190] = self._root_record(joliet=False)
        d[190:318] = pad_str("", 128)  # volume set identifier
        d[318:446] = pad_str(self.app_id, 128)  # publisher
        d[446:574] = pad_str(self.app_id, 128)  # data preparer
        d[574:702] = pad_str(self.app_id, 128)  # application identifier
        d[702:739] = pad_str("", 37)
        d[739:776] = pad_str("", 37)
        d[776:813] = pad_str("", 37)
        d[813:830] = iso_datetime(self._timestamp)
        d[830:847] = iso_datetime(self._timestamp)
        d[847:864] = b"0" * 16 + b"\x00"
        d[864:881] = b"0" * 16 + b"\x00"
        d[881] = 1  # file structure version
        return bytes(d)

    def _svd(
        self, total_sectors: int, pt_size: int, pt_l_lba: int, pt_m_lba: int
    ) -> bytes:
        d = bytearray(SECTOR)
        d[0] = 2  # supplementary volume descriptor
        d[1:6] = b"CD001"
        d[6] = 1
        d[8:40] = pad_ucs2("", 32)
        d[40:72] = pad_ucs2(self.volume_id, 32)
        d[80:88] = both_endian32(total_sectors)
        d[88:120] = b"%/E".ljust(32, b"\x00")  # UCS-2 level 3 escape sequence
        d[120:124] = both_endian16(1)
        d[124:128] = both_endian16(1)
        d[128:132] = both_endian16(SECTOR)
        d[132:140] = both_endian32(pt_size)
        d[140:144] = struct.pack("<I", pt_l_lba)
        d[144:148] = struct.pack("<I", 0)
        d[148:152] = struct.pack(">I", pt_m_lba)
        d[152:156] = struct.pack(">I", 0)
        d[156:190] = self._root_record(joliet=True)
        d[190:318] = pad_ucs2("", 128)
        d[318:446] = pad_ucs2(self.app_id, 128)
        d[446:574] = pad_ucs2(self.app_id, 128)
        d[574:702] = pad_ucs2(self.app_id, 128)
        d[813:830] = iso_datetime(self._timestamp)
        d[830:847] = iso_datetime(self._timestamp)
        d[847:864] = b"0" * 16 + b"\x00"
        d[864:881] = b"0" * 16 + b"\x00"
        d[881] = 1
        return bytes(d)

    def _boot_record(self, catalog_lba: int) -> bytes:
        d = bytearray(SECTOR)
        d[0] = 0  # boot record
        d[1:6] = b"CD001"
        d[6] = 1
        d[7:39] = b"EL TORITO SPECIFICATION".ljust(32, b"\x00")
        d[71:75] = struct.pack("<I", catalog_lba)
        return bytes(d)

    def _terminator(self) -> bytes:
        d = bytearray(SECTOR)
        d[0] = 0xFF
        d[1:6] = b"CD001"
        d[6] = 1
        return bytes(d)

    def _boot_catalog(self, boot_lba: int) -> bytes:
        d = bytearray(SECTOR)

        # Validation entry.
        entry = bytearray(32)
        entry[0] = 0x01  # header id
        entry[1] = 0x00  # platform: 80x86
        entry[4:28] = pad_str("QitoOS", 24)
        entry[30] = 0x55
        entry[31] = 0xAA
        checksum = sum(struct.unpack("<16H", bytes(entry))) & 0xFFFF
        entry[28:30] = struct.pack("<H", (-checksum) & 0xFFFF)
        d[0:32] = entry

        # Default (initial/default) entry: no emulation.
        boot = bytearray(32)
        boot[0] = 0x88  # bootable
        boot[1] = 0x00  # no emulation
        boot[2:4] = struct.pack("<H", 0)  # load segment (0 => 0x7C00)
        boot[4] = 0x00  # system type
        boot[6:8] = struct.pack("<H", self.boot_load_size)
        boot[8:12] = struct.pack("<I", boot_lba)
        d[32:64] = boot

        return bytes(d)

    def _patch_boot_info_table(
        self, image: bytearray, boot_node: Node, total_sectors: int
    ) -> None:
        """
        Write the 56-byte "boot info table" at offset 8 of the boot image.

        This is the same convention isolinux/GRUB use: the loader can read its
        own LBA and length without knowing how the ISO was mastered.
        """
        base = boot_node.extent * SECTOR
        if len(boot_node.data) < 64:
            return
        checksum = 0
        data = image[base + 64 : base + align_up(len(boot_node.data), 4)]
        for offset in range(0, len(data) - 3, 4):
            checksum = (checksum + struct.unpack_from("<I", data, offset)[0]) & 0xFFFFFFFF
        table = struct.pack(
            "<IIII",
            16,  # LBA of the primary volume descriptor
            boot_node.extent,  # LBA of the boot file
            len(boot_node.data),  # boot file length in bytes
            checksum,
        )
        image[base + 8 : base + 8 + len(table)] = table


__all__ = ["IsoBuilder", "Node", "SECTOR", "align_up"]
