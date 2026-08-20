# QitoOS architecture

How the system is put together, from the first instruction the firmware runs
to a window on screen.

---

## 1. The boot chain

### 1.1 Firmware to stage 1

The ISO is bootable through **El Torito no-emulation mode**. The firmware
reads the boot catalogue, loads the boot image at `0000:7C00` and jumps to it
in 16-bit real mode with `DL` holding the BIOS drive number.

The boot image is not just a sector: the catalogue asks for **8 virtual
sectors (4 KiB)**, so the firmware pulls in both stages before transferring
control. This avoids stage 1 having to load stage 2 by hand.

Stage 1's layout is dictated by El Torito, which reserves bytes 8–63 of the
boot image for a **boot information table** that the ISO writer patches in:

| Offset | Contents |
| --- | --- |
| `0x00` | `jmp stage1_main` followed by `nop` |
| `0x03` | `"QITO1"` signature |
| `0x08` | 56-byte boot information table (written by `tools/mkiso.py`) |
| `0x40` | Stage 1 code |
| `0x1FE` | `0xAA55` |
| `0x200` | Stage 2 begins |

> Getting this wrong is silent and total. An earlier revision placed code at
> offset 8; the ISO writer overwrote it with the boot info table and the
> machine hung with no output at all. Code must start at `0x40`.

Stage 1 normalises the segment registers, sets up a stack below `0x7C00`,
saves the boot drive, initialises COM1 so later failures are visible, and
jumps to stage 2.

### 1.2 Stage 2

Stage 2 does the real work of preparing the machine:

1. **A20 gate.** Tried via the BIOS, then the keyboard controller, then Fast
   A20 at port `0x92`, verifying after each attempt.
2. **Unreal mode.** Briefly enters protected mode to load segment descriptors
   with 4 GiB limits, then returns to real mode. The limits persist, so BIOS
   calls still work while any address below 4 GiB is reachable — which is how
   the kernel gets loaded to `0x100000` without a protected-mode disk driver.
3. **Memory map.** `INT 15h, AX=E820h` is walked into a table at `0x19000`.
4. **ACPI RSDP.** Searched for in the EBDA and the BIOS area, recorded for the
   kernel.
5. **Loading.** The payload table embedded at offset 512 of the boot image
   names the LBA and length of the kernel and the ramdisk. Both are read with
   `INT 13h` extensions through a bounce buffer at `0x10000` and copied up
   past 1 MiB using unreal-mode addressing.
6. **Graphics.** `INT 10h, AX=4F00h` fetches VBE information; the mode list is
   searched for the best linear-framebuffer 32bpp mode, preferring 1024x768.
   The chosen mode is set and its details recorded.
7. **Page tables.** A PML4 at `0x20000`, one PDPT and four page directories
   identity-map the low 4 GiB with 2 MiB pages. The same PDPT is also mapped
   at PML4 entry 511, which places the kernel's higher-half window over the
   same physical memory.
8. **Long mode.** Enable PAE, set `EFER.LME` and `EFER.NXE`, load `CR3`, set
   `CR0.PG|CR0.PE`, and far jump into 64-bit code.

Control passes to the kernel entry point with `RDI` pointing at the boot
information block and `RSI` holding the magic value `QTBI`.

### 1.3 The boot protocol

`src/boot/bootinfo.h` defines the contract, and is compiled into both the
loader and the kernel so the two cannot disagree:

```c
struct qito_boot_info {
    uint32_t magic;          /* 'QTBI' */
    uint32_t version;
    uint64_t kernel_phys, kernel_size;
    uint64_t ramdisk_phys, ramdisk_size;
    uint64_t e820_addr;
    uint32_t e820_count;
    uint64_t acpi_rsdp;
    /* framebuffer geometry and channel layout */
    uint64_t fb_addr;
    uint32_t fb_width, fb_height, fb_pitch;
    uint8_t  fb_bpp, fb_valid;
    uint8_t  fb_red_shift, fb_red_size;     /* and green, blue */
    uint8_t  boot_drive;
    char     cmdline[128];
};
```

The physical memory layout the loader establishes:

| Address | Contents |
| --- | --- |
| `0x00500` | Stage 2 stack |
| `0x07C00` | Stage 1 and 2 as loaded by the firmware |
| `0x10000` | Disk bounce buffer |
| `0x18000` | Boot information block |
| `0x19000` | E820 memory map |
| `0x20000` | PML4 |
| `0x21000` | PDPT |
| `0x22000` | Four page directories |
| `0x90000` | Long-mode stack |
| `0x100000` | Kernel image |
| `0x2000000` | Ramdisk |

---

## 2. Kernel startup

`kmain()` in `src/kernel/main.c` brings the system up in eight stages, each
depending only on those before it:

```
1. console      serial output first, so any later failure is visible
2. cpu          GDT, IDT, TSS, PIC, CPUID feature detection
3. memory       physical frames, page tables, kernel heap
4. time         RTC, TSC calibration, PIT; interrupts enabled here
5. devices      PCI, framebuffer, console, input, power, audio
6. filesystem   VFS, ramdisk, /dev, /proc
7. services     config, syscalls, IPC, service manager, network, packages
8. userspace    scheduler, init task, shells, desktop
```

The boot information block sits in memory the page allocator will later hand
out, so `kmain()` copies it into a static buffer before anything else runs.

Interrupts stay disabled until stage 4, once the IDT is valid and the timer is
programmed. Everything before that point runs single-threaded with no
possibility of preemption.

---

## 3. Memory management

### 3.1 Physical frames

`src/kernel/mm/pmm.c` keeps a bitmap with one bit per 4 KiB frame, capped at
4 GiB. A set bit means allocated. The bitmap itself is placed in the first
usable E820 region large enough to hold it, after the kernel and ramdisk.

Regions the firmware did not mark usable start out allocated, as do the
kernel image, the ramdisk, the bitmap and everything below 1 MiB. Allocation
is first-fit from a rolling hint; freed pages are zeroed so no stale data
leaks between users.

### 3.2 Virtual memory

`src/kernel/mm/vmm.c` manages 4-level page tables. PML4 entries 256–511 — the
kernel half — are allocated up front and copied into every address space, so
a kernel mapping made later is visible everywhere without synchronisation.

The page walker deliberately **refuses to split 2 MiB or 1 GiB pages**. The
bootloader's identity map uses large pages; silently shattering one would
produce a mapping the rest of the kernel does not expect. Callers must not map
inside the identity region.

### 3.3 The heap

`src/kernel/mm/heap.c` is a first-fit free list starting at
`0xFFFFC00000000000`, initially 4 MiB and growing 1 MiB at a time to a 256 MiB
ceiling. Every block carries magic values at both ends; `kfree()` checks them
and panics on corruption or a double free, which turns a class of bug that is
normally invisible into an immediate, located failure.

Pages are mapped `PRESENT | WRITE | NX`: heap memory is never executable.

---

## 4. Interrupts

All 256 IDT vectors point at stubs generated by a macro in
`src/kernel/arch/x86_64/isr.S`. Each pushes its vector number and an error
code (zero for exceptions that do not supply one) and jumps to a shared
`isr_common`, which saves every general-purpose register and calls into C.

`interrupt_dispatch()` receives a pointer to that saved frame. **Returning a
different pointer switches context** — this is the entire mechanism behind
preemption, and it means there is no separate assembly context switcher.

The 8259A PICs are remapped to vectors 32–47 so hardware interrupts do not
collide with CPU exceptions. Three IST stacks handle the cases where the
current stack may be unusable: `#DF`, `#PF` and NMI.

Order of operations in the dispatcher matters: statistics, then the handler,
then the end-of-interrupt, then the scheduler decision. Sending EOI before the
handler runs would allow re-entry.

---

## 5. Scheduling

`src/kernel/sched/sched.c` implements preemptive round-robin scheduling across
five priority levels, each with its own FIFO ready queue. The highest
non-empty queue always wins; within a queue tasks rotate. A time slice is
`2 + priority * 2` ticks, so higher-priority work also gets longer to run.

Each task has a 32 KiB kernel stack with its initial interrupt frame written
to the top, so a brand-new task is indistinguishable from a preempted one and
the same return path starts it.

PID 1 is the idle task, which simply halts the CPU until the next interrupt.

---

## 6. Filesystem

The VFS is a tree of `struct fs_node` linked by child and sibling pointers.
Directory entries are returned in creation order. Backends supply an `fs_ops`
table with any of `read`, `write`, `truncate`, `open`, `close` and `refresh`.

`refresh` is what makes `/proc` work: it runs before any read, stat or
readdir, letting procfs regenerate a node's contents from live kernel state at
the moment it is read rather than caching a stale snapshot.

**QitoFS** is the root filesystem, unpacked from the boot ramdisk. The image
is a 72-byte header, a table of 232-byte entries, then the file data:

```
header   magic "QITOFS01", version, entry count, total size,
         data offset, flags, checksum, volume label
entry    absolute path, type, permissions, size, offset,
         modification time, uid, gid
```

Entries are ordered directories-first and then by depth, so a parent always
exists before its children are created. The checksum over the file data is
verified at mount time.

**devfs** provides the standard character devices. **procfs** exposes nine
generated nodes covering version, memory, CPU, uptime, tasks, interrupts,
filesystems, statistics and display state.

---

## 7. Graphics

The framebuffer driver keeps a **software back buffer** in ordinary RAM.
Drawing touches only that buffer; `fb_flush()` copies the dirty rectangle to
the VESA linear framebuffer, which is mapped uncached (`PCD`) and
non-executable.

This matters for performance: writes to video memory over the PCI bus are
roughly two orders of magnitude slower than writes to RAM, so compositing in
RAM and copying once per frame is dramatically faster than drawing directly.

Pixels are 32-bit `0x00RRGGBB`. The channel shifts come from the VBE mode
information the bootloader recorded.

> Those channel fields are a `(MaskSize, FieldPosition)` pair, mask size
> first. Reading them in the wrong order made every shift equal 8, which
> rendered the entire desktop in shades of green while the kernel log looked
> perfectly healthy. The boot tests now assert that no single colour channel
> dominates a captured frame.

The font is an 8x16 bitmap generated at build time by `tools/genfont.py` from
ASCII-art glyph definitions, which keeps the glyphs reviewable in a diff.

### Capturing what is on screen

Headless emulators produce no image file, so the kernel can stream its own
framebuffer out of COM1: run-length encoded, base64 wrapped, between
recognisable markers. A 1024x768 frame is 2.3 MB raw but compresses to roughly
50 KB, which transfers in a few seconds. `tools/grabframe.py` decodes it into a
PNG. This is how the README screenshots are produced and how the boot tests
verify rendering.

---

## 8. The desktop

`src/user/desktop/desktop.c` runs one loop:

```
1. drain the input event queue and route events
2. give visible applications a tick
3. if anything changed, composite and flush
4. sleep 16 ms  (roughly 60 frames per second)
```

Compositing draws the wallpaper, then windows back to front, then the panel,
any open menu, notifications and finally the cursor. Rather than reordering
the window array — which would invalidate pointers applications hold — a list
of indices is sorted by z-order each frame.

Applications are `struct app_ops` tables, not processes. Each provides `draw`,
optional input handlers, an optional `tick`, and open/close hooks. The
compositor sets a clip rectangle to the client area before calling `draw`, so
an application cannot scribble over its own decorations or another window.

---

## 9. The shells

Both shells share `src/user/shells/shell_core.c`, which handles tokenising,
quoting, variable expansion, pipelines, redirection, aliases, history and line
editing. Each shell supplies only a command table:

```c
struct shell_command {
    const char *name, *summary, *usage, *details;
    int (*handler)(struct shell *sh, int argc, char **argv);
    uint32_t flags;      /* CMD_HIDDEN, CMD_PRIVILEGED */
};
```

Because the metadata lives with the command, `help` and tab completion work
for anything added without further wiring.

### Pipelines without processes

Commands are C functions, so there is no `fork()`. A pipeline runs each stage
with its output captured into a `struct shell_sink`; the captured text is
handed to the next stage through the `QITO_PIPE_INPUT` variable, which
commands like `grep` and `sort` read when given no file argument.

The observable behaviour matches a real pipeline, and the same sink mechanism
lets one shell run a command on behalf of the other, and lets the terminal
application redirect shell output into a window.

---

## 10. Building the ISO

The project writes its own ISO 9660 images rather than depending on
`xorriso` or `grub-mkrescue`. `tools/isofs.py` implements the subset of
ECMA-119 that firmware actually requires:

- Primary volume descriptor, boot record, Joliet supplementary descriptor and
  terminator at sectors 16 through 19
- Little- and big-endian path tables
- A directory tree with correctly formed `.` and `..` entries
- An El Torito boot catalogue whose validation entry checksums to zero
- The 56-byte boot information table patched into the boot image

`tools/mkiso.py` builds in two passes: it lays out the image once to discover
the sector each payload lands on, patches those addresses into the loader's
payload table, then writes the final image. `scripts/validate-iso.py` checks
the result structurally, and the boot tests confirm it actually boots.

---

## Source map

| Path | Contents |
| --- | --- |
| `src/boot/` | Bootloader and the boot protocol |
| `src/kernel/arch/x86_64/` | Entry point, descriptor tables, ISRs, PIC, CPUID |
| `src/kernel/mm/` | Physical, virtual and heap allocators |
| `src/kernel/sched/` | Scheduler |
| `src/kernel/sys/` | Syscalls, IPC, configuration, services, packages, power |
| `src/kernel/fs/` | VFS, QitoFS, devfs, procfs |
| `src/kernel/drivers/` | Serial, keyboard, mouse, PCI, audio |
| `src/kernel/net/` | IPv4 stack and NE2000 |
| `src/kernel/gfx/` | Rasteriser, console, framebuffer capture |
| `src/kernel/time/` | PIT, RTC, TSC |
| `src/kernel/log/` | Log ring buffer and panic |
| `src/lib/libq/` | Freestanding string and printf implementations |
| `src/user/desktop/` | Compositor and window manager |
| `src/user/apps/` | Built-in applications |
| `src/user/shells/` | Shell engine, QCSH and UltraShell |
