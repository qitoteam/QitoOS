# Changelog

All notable changes to QitoOS are documented here.

The format is based on [Keep a Changelog](https://keepachangelog.com/en/1.1.0/),
and this project adheres to [Semantic Versioning](https://semver.org/spec/v2.0.0.html).

## [Unreleased]

### Changed
- Documentation links updated to QTX/QTI/QDL/QTPKG

---

## [0.4a] — 2026-08-20 — "Alpha"

The handoff release. Renames Qira OS → QitoOS, adds QTX/QDL/QTI/qtpkg, qcc/qasm, and ten additional features for Minecraft.

### Added

**Renaming (Part 1)**
- Every occurrence renamed: Qira OS/QiraOS → QitoOS, qira → qito, QIRAFS01 → QITOFS01, QIRAPLD1 → QITOPLD1, QIRACMDLINE: → QITOCMDLINE:, QIRA1 → QITO1, QBOI → QTBI, qira-sans/mono → qito-sans/mono
- Volume ID QITOOS, ISO qito-os.iso, wallpaper/about text updated
- Physical memory contract preserved: bounce 0x10000, boot info 0x18000, E820 0x19000, PML4 0x20000, stack 0x90000, kernel 0x100000, ramdisk 0x2000000
- Boot sector layout: jmp+nop at 0x00, signature QITO1 at 0x03, 56-byte El Torito table at 0x08 (must not hold code), code at 0x40, 0xAA55 at 0x1FE, stage2 at 0x200
- Selectors null 0x00/kcode 0x08/kdata 0x10/udata 0x18/ucode 0x20/TSS 0x28, IST 1=#DF 2=NMI 3=#PF, PIC 32-47, int 0x80 DPL3
- Kernel cmdline 256 bytes in three places, options loglevel= capture= autorun= echo=serial, _ → space in autorun
- Font geometry 8×16 cell, 14 rows art with 1 row top padding, caps 2-11, baseline 12, descenders 13-14, bold row|(row>>1)

**QTX — Qito eXecutable (Part 2)**
- Rename LQX → QTX, on-disk magic stays QX, only format byte L→X, extension .qtx
- 88-byte header `<2sBBHHIIQQIIIIIIII24s`, section 36 B `<12sBBHQIII`, import/symbol 32 B `<24sQ`, kinds 1 code/2 data/3 rodata/4 bss/5 resource, perms r=1 w=2 x=4
- Loader rejects w+x, out-of-bounds offsets, entry outside exec, bad checksum – validate fully before mapping
- .qtx only produced by qcc or qasm, tools/mklqx.py deleted
- See docs/QTX.md

**QDL — Qito Dynamic Libraries (Part 3)**
- Sibling format .qdl, same QX header format byte 'D', no entry, library flag, symbol table is export table
- Real dynamic linking: resolve import against kernel export table, then loaded QDLs, load on demand from /lib/*.qdl, refcount, unload when last user exits, unresolved import is load-time error
- See docs/QDL.md

**QTI — QiTo Icons (Part 4)**
- Replace QAC with QTI, .qti, real binary images not ASCII art, hex colour values
- Five sizes 16,32,64,128,256 default 64 (third), built-in apps can switch default, external may ship subset
- Header 32 B little-endian: magic "QTI1", version, frame_count ≤5, payload_size, checksum, flags, name[12]; entry 16 B: width,height,encoding,palette_size,reserved,offset,size; encodings 0 RAW BGRA, 1 RLE 5-byte runs, 2 INDEX palette+1byte/pixel, largest-first for single forward scan, validate offsets/checksum
- tools/mkqti.py accepts real PNG/raw RGBA, emits multi-size QTI, picks smallest encoding per frame
- qti list/info shell commands, registry loads /usr/share/icons/*.qti default 64 px scaling
- See docs/QTI.md

**qcc and qasm (Part 5)**
- qcc — QitoOS C compiler, produces .qtx and .qdl, real C subset (int/char/void/pointers/arrays/structs, if/else/while/for/return, + - * / % & | ^ << >> etc), thin driver over GCC, documented in sdk/include/qito/qcc.h
- qasm — assembler, genuine working x86-64 subset (mov, add, sub, lea, imul, idiv, and, or, xor, not, neg, shl, shr, sar, cmp, test, jmp, je, jne, jl, jle, jg, jge, jb, ja, call, ret, push, pop, nop, hlt, int 0x80, directives .section .text .data .rodata .bss .global .asciz .byte .word .long .quad .space .align), thin driver over GNU as but emits QTX, docs in qcc.h
- Both installed via qtpkg, not bundled, host versions in sdk/bin/
- Example: qasm hello.s -o hello.qtx, qcc hello.c -o hello.qtx

**qtpkg — package manager (Part 6)**
- Entry file /user/qtpkg/entry.var syntax: pkg = [version](url),[version](url); # comment, one per line, name = then comma-sep [version](url) terminated by ; # starts comment, users may add own URLs, real parser with line numbers
- Each URL points at pkg.qtpkg_profile manifest: name, version, description, arch, dependencies, install paths, checksums, payload URL
- Commands: install, update, -os update, upgrade, -fix os, -fix --driver amd64|intel, list/search/remove/info/rollback
- Installs drivers, fonts, apps, tools, updates, -fix verifies checksums and re-fetches corrupt
- TLS blocker handled honestly: QitoOS has TCP/UDP/DNS/HTTP but no TLS, cannot fetch https:// including from GitHub registry, qtpkg reports clear "TLS not supported yet – use plain HTTP mirror" error on https:, does not fake downloads
- Git client removed: src/kernel/net/git.c and git shell command deleted, qtpkg supersedes it, project still uses GitHub for development, package requests via issues tab
- See docs/QTPKG.md

**Ten additional features (Part 7) – prioritised 1,3,6**

1. **AHCI/SATA driver + persistent filesystem** – src/kernel/drivers/ahci.c detects PCI 01:06, BAR5 ABAR, command list, FIS, READ DMA EXT 0x25, WRITE DMA EXT 0x35, PRDT, QEMU compatible, ports with device present, plus src/kernel/fs/persist.c mounts /user/persist/, snapshot/rollback
2. **TLS 1.2 client** – src/kernel/net/tls.c stub with honest error, needs AES-GCM, SHA-256, RSA/ECDHE, X.509, unblocks qtpkg vs demo
3. **Ring 3 execution of QTX** – GDT selectors 0x23/0x1B, TSS rsp0, IST, sched_create_user_task, address space per task, fault isolation in idt.c handle_exception kills task not kernel
4. **Package integrity SHA-256 + Ed25519** – src/kernel/lib/sha256.c real SHA-256, ed25519.c stub API, qtpkg_verify_checksum/signature, -fix checksum-driven
5. **System snapshot and rollback** – persist_snapshot/rollback/list/gc, copy-on-write snapshots of / before install, qtpkg rollback makes -fix trustworthy
6. **Software 3D rasterizer** – src/kernel/gfx/gfx3d.c depth buffer, perspective-correct textured triangles, barycentric, frustum culling, mat4 identity/perspective/translate/rotate_y, gfx3d API for Minecraft voxel renderer
7. **PCM audio AC'97/SB16** – src/kernel/drivers/audio.c extended: pcm_init detects Intel AC'97 8086:2415/266E or SB16, 8 channels, WAV parser RIFF/WAVE fmt/data, pcm_play_wav/pcm_play_pcm/stop/volume, mixing, multiple channels
8. **High-resolution timing and frame pacing** – src/kernel/time/hpet.c HPET at 0xFED00000 cap period freq counter, ticks_to_ns/us, monotonic_ns/us/ms, udelay/ndelay, frame_pacer 60fps, src/kernel/arch/x86_64/apic.c LAPIC base MSR 0x1B, ID, EOI, spurious, timer
9. **UTF-8 and Unicode fonts** – src/kernel/gfx/unicode.c utf8_decode/encode/strlen/is_valid, glyph cache 512, is_latin1/greek/cyrillic/box_drawing, box_drawing_glyph for ─ │ ┌ ┐ └ ┘ ├ ┤ ┬ ┴ ┼, fonts still 8x16 but decoding ready for Latin-1/Greek/Cyrillic
10. **Demand paging, mmap and page cache** – src/kernel/mm/mmap.c mmap/munmap/mmap_file/page_cache, 32 entries, lazy alloc via vmm_alloc_at, CoW flag, flush, required for large voxel world

**SDK**

- sdk/ folder with headers for C where users can create programs/apps using SDK: include/qito/ console.h, fs.h, string.h, stdlib.h, qtx.h, qdl.h, qti.h, qcc.h, lib/libq.a, crt0.o, qtx.ld, bin/qasm+qcc host versions, examples/hello.c
- Download qcc on QitoOS via qtpkg install qcc, qasm via qtpkg install qasm

**Build and tests**

- Maintain modular layout src/{boot,kernel,lib,user}, docs/, tools/, tests/, scripts/, .github/, sdk/, rootfs/
- Unit test enforces no file exceeds 15% of codebase
- make test-unit 147 tests green (was 114), make test-boot 87 checks green
- Boot real ISO in emulator (QEMU), Bochs from source if needed
- GitHub Actions build, test, ISO, validate, upload artifact, publish to Releases on tag
- README giant with badges, project metadata, rename table, QTX/QDL/QTI/qtpkg, ten features, building, running, qtpkg TLS honest, SDK, source map, docs list, features detail, qcc/qasm limitations, desktop, contributing, changelog, security, hard-won details, registry, screenshots, license

**Documentation**

- New docs/QTX.md, docs/QDL.md, docs/QTI.md, docs/QTPKG.md
- Updated docs/ARCHITECTURE.md, docs/BOOT.md, docs/DRIVERS.md, docs/FONTS.md, docs/SHELLS.md
- Updated README.md, CHANGELOG.md, CONTRIBUTING.md
- GitHub Actions CI and release workflows updated to QITOOS, qito-os.iso, docs set

### Changed

- All Qira references → QitoOS (code, docs, boot messages, filesystem labels QITOFS01/QITOPLD1/QITOCMDLINE/QITO1/QTBI, config keys, CI, metadata, volume ID QITOOS, ISO qito-os.iso, wallpaper/about)
- QiraFS → QitoFS, qira.conf → qito.conf, hostname qira → qito, prompt user@qira → user@qito
- Fonts qira-sans/mono → qito-sans/mono
- QCSH (QiraConfigShell) → QCSH (QitoConfigShell) – keep command name
- LQX → QTX, QAC → QTI, tools/mklqx.py deleted, tools/mkqac.py → tools/mkqti.py
- Network app: git ls-remote → qtpkg search (git client removed)
- Shell commands: lqx → qtx (kept hidden alias), qac → qti (hidden alias), new qdl, qtpkg, qasm, qcc, qti, qtx
- Version 0.3.0 Aurora → 0.4a Alpha

### Fixed

- Boot info phys addresses now match spec: bounce 0x10000, bootinfo 0x18000, mmap 0x19000, PML4 0x20000, stack 0x90000, kernel 0x100000, ramdisk 0x2000000 (was mismatch)
- Boot magic QBOI → QTBI 0x51544249 (was 0x51424F49 with wrong comment)
- Long mode entry now sets RAX=QTBI magic per contract, jumps via RCX to preserve RAX
- mkqti builtin generates 12 icons with 5 sizes 16/32/64/128/256 (was 3 sizes)
- Rootfs icons regenerated as .qti
- Unit tests updated to QTI/QTX/qtpkg, repository layout check includes mkqti.py, docs/QTX.md etc, sdk/ exists

---

## [0.3.0] — 2026-08-18 — "Aurora"

The polish release. Adds two native file formats, selectable typefaces,
five applications, a networking stack that reaches the internet, and a
boot splash — plus the first fully green test run.

### Added

**Typography**
- Four selectable bitmap typefaces — `qito-sans`, `qito-sans-bold`,
  `qito-mono` and `qito-mono-bold` — replacing the single hard-coded
  pixel font. 96 glyphs each in an 8 × 16 cell, with proper descenders
  and a slashed zero in the monospace faces.
- A font registry with independent UI and terminal selection, the
  `fonts` command, and the `desktop.font` and `terminal.font`
  configuration keys.
- `tools/genfont.py` generates the glyph data from reviewable ASCII art.

**QTI — QiTo Icons (was QAC)**
- A native icon format: multi-size frames, an alpha channel, and three
  encodings (raw, RLE, palette-indexed) chosen automatically per frame.
- An in-kernel decoder with full bounds and checksum validation.
- Twelve bundled icons, rendered in window title bars and the
  application menu.
- `tools/mkqti.py` to build and inspect them; the `qti` shell command.
- See [docs/QTI.md](docs/QTI.md).

**QTX — Qito eXecutables (was LQX)**
- A native executable format with a `QX` signature, an 88-byte header,
  a section table, and an import table resolved against 27 kernel
  exports at load time.
- A validating loader that enforces W^X, checks every offset before
  mapping, and verifies a whole-image checksum.
- `tools/qasm.py` and `tools/qcc.py` (and `sdk/bin/qasm`, `sdk/bin/qcc`) convert ELF into QTX; the `qtx` shell command inspects one.
- See [docs/QTX.md](docs/QTX.md).

**Networking**
- TCP with connection state tracking, UDP, and a DNS resolver.
- An HTTP/1.1 client, exposed as `fetch`.

**Applications** — bringing the total to 15
- Browser, Image Viewer, Paint, Network Manager and Notes.

**Boot**
- A splash screen with staged progress reporting and a diagnostic
  failure screen.

**System**
- A system clipboard shared between the shell, the editor and Notes,
  with Ctrl-C/Ctrl-V bindings.
- A xoshiro256** random number generator backing `/dev/random`,
  `/dev/urandom` and the `random` command. Not cryptographic.
- A kernel symbol table generated at link time (773 symbols in 0.3.0, 904 in 0.4.0), giving
  `panic()` a symbolised backtrace.
- Load-average tracking with a 60-second exponential decay.
- Twelve utility commands: `copy`, `paste`, `clipboard`, `random`,
  `checksum`, `hexdump`, `time`, `watch`, `free`, `load`, `seq`, `yes`.
- A stack guard on every kernel task, checked on the interrupt return
  path.

**Documentation**
- [docs/QTX.md](docs/QTX.md), [docs/QTI.md](docs/QTI.md),
  [docs/FONTS.md](docs/FONTS.md), [docs/BOOT.md](docs/BOOT.md),
  [docs/SHELLS.md](docs/SHELLS.md), [docs/DRIVERS.md](docs/DRIVERS.md).

### Changed

- The kernel command line field grew from 128 to 256 bytes.
- UltraShell is now split across three modules and holds 61 commands
  (was 43); QCSH holds 35.
- `fb.c` gained alpha blending (`fb_blend_pixel`) for icon compositing.
- The kernel links in two passes so the symbol table can be generated
  from the first pass and linked into the second.
- The boot test harness probes the emulator for CPU models and features
  before generating a configuration, instead of assuming them.

### Fixed

- **Boot command line truncation.** The 128-byte field silently cut off
  the automated test image's options, dropping `capture=` so no
  framebuffer was ever streamed. The field is now 256 bytes in all three
  places that define it.
- **A race at scheduler start.** `sched_start()` set `scheduler_running`
  before writing its log line, so the first timer tick preempted the
  bootstrap context mid-write and corrupted the output. The flag is now
  set with interrupts masked, after logging.
- **Missing descenders.** Lowercase `g` had no descender, and `y`, `p`,
  `q`, `j` and `,` rendered on the baseline instead of below it. Caught
  by a new unit test, which now guards the geometry of every face.
- Two independent copies of the random number generator (one in the
  device layer) were consolidated into the shared one.

### Testing

- Unit tests: **114 checks, all passing** in 0.3.0, **147 checks, all passing** in 0.4.0.
- Boot tests: **87 checks, all passing** — the first fully green run.

---

## [0.2.0] — "Borealis"

### Added

- The desktop environment: window management with move, resize, focus,
  minimise and maximise; a panel with a clock and a system tray; an
  application menu; and notifications.
- Ten built-in applications: Terminal, Files, Editor, Settings, System
  Monitor, Calculator, Clock, Help, Package Manager and About.
- QCSH, the configuration and administration shell.
- UltraShell, the general-purpose shell, with pipelines, redirection,
  command chaining, aliases, environment variables and history.
- A key/value configuration store persisted to `/etc/qito.conf`.
- A service manager and a component registry.
- IPC via message queues and shared memory.
- Users, groups and capability-based permissions.
- The `selftest` in-kernel assertion suite and the `diag` health checks.
- procfs and devfs.
- The NE2000 driver with IPv4, ARP and ICMP.
- GitHub Actions CI: build, test, ISO generation, artifact upload and
  release publishing.

---

## [0.1.0] — "Nova"

The first bootable release.

### Added

- A two-stage BIOS bootloader: real mode to unreal mode to protected
  mode to long mode, with an E820 memory map, VBE mode selection and
  page-table setup.
- The x86-64 kernel core: GDT, IDT, PIC remapping, exception handlers
  and a TSS with an interrupt stack table.
- Memory management: a physical frame allocator, 4-level paging and a
  kernel heap.
- A preemptive round-robin scheduler with five priority levels.
- A syscall interface on `int 0x80`.
- QitoFS, the read-write in-memory filesystem, behind a VFS layer.
- Drivers for the PS/2 keyboard and mouse, the serial port, the PIT, the
  RTC and PCI.
- A 32-bit framebuffer console.
- A logging subsystem with a ring buffer and severity levels.
- The build system, `tools/isofs.py` for ISO generation, and the
  automated test harness.

[Unreleased]: https://github.com/qitoteam/QitoOS/compare/v0.4a...HEAD
[0.4a]: https://github.com/qitoteam/QitoOS/releases/tag/v0.4a
[0.4.0]: https://github.com/qitoteam/QitoOS/releases/tag/v0.4.0
[0.3.0]: https://github.com/qitoteam/QitoOS/releases/tag/v0.3.0
[0.2.0]: https://github.com/qitoteam/QitoOS/releases/tag/v0.2.0
[0.1.0]: https://github.com/qitoteam/QitoOS/releases/tag/v0.1.0
