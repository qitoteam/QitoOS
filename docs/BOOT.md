# The Qira OS boot process

From power-on to the desktop, in the order it actually happens. Qira OS
uses its own two-stage BIOS bootloader rather than GRUB or Limine, so
every step below is code in this repository.

- **Stage 1:** [`src/boot/stage1.S`](../src/boot/stage1.S)
- **Stage 2:** [`src/boot/stage2.S`](../src/boot/stage2.S)
- **Boot info contract:** [`src/boot/bootinfo.h`](../src/boot/bootinfo.h)
- **Kernel entry:** [`src/kernel/arch/x86_64/entry.S`](../src/kernel/arch/x86_64/entry.S)
- **Image builder:** [`tools/mkiso.py`](../tools/mkiso.py)

---

## Overview

```
BIOS  ──►  stage 1        512 B, real mode, loaded at 0x7C00
           │  loads stage 2 from the El Torito boot image
           ▼
           stage 2        real mode → unreal → protected → long mode
           │  E820 map, VBE framebuffer, disk reads, page tables
           ▼
           kernel entry   long mode, RDI = boot info, magic "QBOI"
           │
           ▼
           kmain()        CPU, memory, devices, filesystem, desktop
```

---

## Stage 1 — the boot sector

512 bytes, loaded by the BIOS at `0x7C00` in 16-bit real mode.

Its layout is unusual in one important respect:

| Offset | Contents |
| ---: | --- |
| `0x00` | `jmp` over the header, plus a `nop` |
| `0x03` | `"QIRA1"` signature |
| `0x08` | **56-byte El Torito boot information table** |
| `0x40` | actual code starts here |
| `0x1FE` | `0xAA55` boot signature |

> **Offset 8 is reserved and must not hold code.** The El Torito
> specification has the BIOS — or the ISO mastering tool — write a boot
> information table at offset 8 of the boot image. Anything placed there
> is silently overwritten. This cost real debugging time: the boot sector
> worked when written to a floppy image and crashed when booted from CD,
> because the table landed on top of the code. Stage 1 code therefore
> starts at `.org 0x40`.

Stage 1 does the minimum: sets up a stack, saves the BIOS drive number,
and uses INT 13h extended reads (AH=42h) to pull stage 2 off the disk.
It then jumps to stage 2 at offset `0x200`.

---

## The payload table

`tools/mkiso.py` needs to tell the loader where the kernel and ramdisk
ended up on the disc. It patches a table at **offset 512** of the boot
image, marked by the signature `QIRAPLD1`:

| Offset | Size | Field |
| ---: | ---: | --- |
| 0 | 8 | `"QIRAPLD1"` |
| 8 | 4 | `kernel_lba` |
| 12 | 4 | `kernel_sectors` |
| 16 | 4 | `kernel_size` |
| 20 | 4 | `ramdisk_lba` |
| 24 | 4 | `ramdisk_sectors` |
| 28 | 4 | `ramdisk_size` |
| 32 | 4 | reserved |

All fields are little-endian `uint32`. This is why the ISO can be rebuilt
with a different kernel without reassembling the bootloader.

---

## Stage 2 — getting to long mode

Stage 2 is where the real work happens. In order:

1. **Enter unreal mode.** Briefly switch to protected mode, load a
   descriptor with a 4 GiB limit into `FS`, then switch back to real
   mode. The segment limit persists, so 16-bit code can now address all
   of physical memory through `%fs:` while still making BIOS calls. This
   is how the loader copies a 330 KB kernel above the 1 MiB line without
   having given up INT 13h yet.

2. **Query the memory map.** INT 15h/E820, stored at `0x19000`.

3. **Set the video mode.** VBE (INT 10h/4F) is used to find and select a
   1024 × 768 × 32 linear framebuffer. The mode information — address,
   pitch, bit depth — goes into the boot info block. If no suitable VBE
   mode exists, the loader records that fact and the kernel falls back to
   a text console.

4. **Load the kernel and ramdisk.** Read in chunks through a bounce
   buffer at `0x10000`, copying each chunk up to its final home with
   unreal-mode addressing. The kernel lands at `0x100000`, the ramdisk at
   `0x2000000`.

5. **Build page tables.** A PML4 at `0x20000` with two mappings of the
   first 1 GiB of physical memory: an identity map (so the code keeps
   running across the mode switch) and a higher-half map at
   `0xFFFFFFFF80000000` (where the kernel is linked).

6. **Fill the boot info block** at `0x18000` and enter long mode: set
   `CR4.PAE`, set `EFER.LME`, load `CR3`, set `CR0.PG`, far-jump to
   64-bit code.

7. **Jump to the kernel** at virtual `0xFFFFFFFF80100000` with `RDI`
   pointing at the boot info block and `RAX` holding the magic `QBOI`.

---

## The physical memory contract

Addresses the loader and kernel agree on. Changing one means changing
both.

| Address | Contents |
| ---: | --- |
| `0x10000` | bounce buffer for disk reads |
| `0x18000` | boot info block |
| `0x19000` | E820 memory map |
| `0x20000` | PML4 and initial page tables |
| `0x90000` | long-mode stack |
| `0x100000` | kernel image |
| `0x2000000` | ramdisk (QiraFS image) |

Kernel entry point: virtual `0xFFFFFFFF80100000`, `RDI = 0x18000`,
`RAX = "QBOI"`.

---

## The kernel command line

Stage 2 embeds a **256-byte** field introduced by the marker
`QIRACMDLINE:`. `mkiso.py` finds the marker and overwrites the whole
field with a NUL-terminated string, so boot options can be changed by
rebuilding the ISO without reassembling the loader.

> The field was originally 128 bytes, which silently truncated the
> automated-test command line and dropped its trailing `capture=` option —
> producing a test failure whose cause was several layers away from its
> symptom. If you change the size, change it in **three** places:
> `src/boot/bootinfo.h`, `CMDLINE_SIZE` in `tools/mkiso.py`, and the
> `.space` directive in `src/boot/stage2.S`.

### Supported options

| Option | Effect |
| --- | --- |
| `root=qirafs` | mount the ramdisk as the root filesystem |
| `console=fb` | use the framebuffer console |
| `loglevel=<n>` | 0 quiet … 4 debug |
| `echo=serial` | mirror shell input and output to COM1 |
| `autorun=a;b;c` | run shell commands at startup, separated by `;` |
| `capture=<ms>[,<ms>]` | stream a framebuffer capture at the given uptimes |

> **Underscores become spaces inside `autorun`.** The command line is
> space-separated, so an argument containing a space must be written with
> underscores: `ls_-l_/etc` runs `ls -l /etc`. The substitution is
> unconditional, which means a literal underscore in an argument does not
> survive — `/etc/mo_td` becomes `/etc/mo td`. Paths like
> `/bin/hello.lqx` are fine.

Build an image with custom options:

```sh
make iso CMDLINE_EXTRA="loglevel=4 autorun=selftest"
```

---

## Kernel initialisation

`kmain()` in [`src/kernel/main.c`](../src/kernel/main.c) runs these phases.
Once the framebuffer is up, each one advances the splash screen.

| # | Phase | What happens |
| ---: | --- | --- |
| 1 | boot info | validate the magic, copy the block out of low memory |
| 2 | CPU | `cpu_detect`, GDT, IDT, PIC remap to vectors 32–47 |
| 3 | memory | physical allocator, virtual memory, kernel heap |
| 4 | time | PIT and RTC, then `sti` |
| 5 | devices | PCI enumeration, keyboard, mouse, serial, storage, network |
| 6 | filesystem | mount the QiraFS ramdisk, then devfs and procfs |
| 7 | assets | fonts, QAC icons, the LQX loader |
| 8 | services | configuration, logging, packages, networking stack |
| 9 | userspace | create the init task |
| 10 | scheduler | `sched_start()` — the first timer tick switches away |

Typical startup on the reference Bochs configuration is about 370–780 ms
depending on how much the emulator is instrumented.

### Segment selectors

| Selector | Descriptor |
| ---: | --- |
| `0x00` | null |
| `0x08` | kernel code |
| `0x10` | kernel data |
| `0x18` | user data |
| `0x20` | user code |
| `0x28` | TSS |

Interrupt stack table entries: IST 1 for `#DF`, IST 2 for NMI, IST 3 for
`#PF`. The `int 0x80` syscall gate is installed at DPL 3.

---

## The splash screen

If a framebuffer is available, `splash_begin()` paints a background and a
progress bar, and each initialisation phase calls `splash_update()` with a
label and a percentage. `splash_fail()` paints a diagnostic screen when a
phase fails, so a boot failure produces something readable rather than a
frozen logo.

Implementation: [`src/kernel/gfx/splash.c`](../src/kernel/gfx/splash.c).

---

## Debugging a boot failure

**Serial output first.** Add `loglevel=4 echo=serial` and capture COM1.
Almost every boot problem is visible there.

```sh
make iso CMDLINE_EXTRA="loglevel=4 echo=serial"
qemu-system-x86_64 -cdrom build/qira-os.iso -serial stdio -m 512
```

**Check the image structure** without booting it:

```sh
python3 scripts/validate-iso.py build/qira-os.iso
python3 tools/checkboot.py build/boot.bin
```

**If stage 2 never runs**, suspect stage 1: the payload table, the INT 13h
read, or code straying below offset `0x40`.

**If the kernel never runs**, suspect the page tables or the long-mode
transition. Bochs is more informative than QEMU here — it reports the
exact reason for a triple fault.

**Force a bootloader rebuild.** Editing `stage2.S` does not always
invalidate the target:

```sh
rm -f build/boot.bin build/boot/stage2.o && make bootloader
```

Likewise, changing only the command line does not rebuild the ISO unless
the `build/.cmdline` stamp changes — delete the ISO if in doubt.
