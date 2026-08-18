# Changelog

All notable changes to Qira OS are documented here.

The format is based on [Keep a Changelog](https://keepachangelog.com/en/1.1.0/),
and this project adheres to [Semantic Versioning](https://semver.org/spec/v2.0.0.html).

## [Unreleased]

Nothing yet.

---

## [0.3.0] — 2026-08-18 — "Aurora"

The polish release. Adds two native file formats, selectable typefaces,
five applications, a networking stack that reaches the internet, and a
boot splash — plus the first fully green test run.

### Added

**Typography**
- Four selectable bitmap typefaces — `qira-sans`, `qira-sans-bold`,
  `qira-mono` and `qira-mono-bold` — replacing the single hard-coded
  pixel font. 96 glyphs each in an 8 × 16 cell, with proper descenders
  and a slashed zero in the monospace faces.
- A font registry with independent UI and terminal selection, the
  `fonts` command, and the `desktop.font` and `terminal.font`
  configuration keys.
- `tools/genfont.py` generates the glyph data from reviewable ASCII art.

**QAC — Qira Application iCon**
- A native icon format: multi-size frames, an alpha channel, and three
  encodings (raw, RLE, palette-indexed) chosen automatically per frame.
- An in-kernel decoder with full bounds and checksum validation.
- Twelve bundled icons, rendered in window title bars and the
  application menu.
- `tools/mkqac.py` to build and inspect them; the `qac` shell command.
- See [docs/QAC.md](docs/QAC.md).

**LQX — Linked Qira Executables**
- A native executable format with a `QX` signature, an 88-byte header,
  a section table, and an import table resolved against 27 kernel
  exports at load time.
- A validating loader that enforces W^X, checks every offset before
  mapping, and verifies a whole-image checksum.
- `tools/mklqx.py` converts a linked ELF into an LQX image; the `lqx`
  shell command inspects one.
- See [docs/LQX.md](docs/LQX.md).

**Networking**
- TCP with connection state tracking, UDP, and a DNS resolver.
- An HTTP/1.1 client, exposed as `fetch`.
- A git smart-HTTP client supporting `clone` and `ls-remote`.

**Applications** — bringing the total to 15
- Browser, Image Viewer, Paint, Network Manager and Notes.

**Boot**
- A splash screen with staged progress reporting and a diagnostic
  failure screen.

**System**
- A system clipboard shared between the shell, the editor and Notes,
  with Ctrl-C/Ctrl-V bindings.
- A xoshiro256\*\* random number generator backing `/dev/random`,
  `/dev/urandom` and the `random` command. Not cryptographic.
- A kernel symbol table generated at link time (773 symbols), giving
  `panic()` a symbolised backtrace.
- Load-average tracking with a 60-second exponential decay.
- Twelve utility commands: `copy`, `paste`, `clipboard`, `random`,
  `checksum`, `hexdump`, `time`, `watch`, `free`, `load`, `seq`, `yes`.
- A stack guard on every kernel task, checked on the interrupt return
  path.

**Documentation**
- [docs/LQX.md](docs/LQX.md), [docs/QAC.md](docs/QAC.md),
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

- Unit tests: **114 checks, all passing**.
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
- A key/value configuration store persisted to `/etc/qira.conf`.
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
- QiraFS, the read-write in-memory filesystem, behind a VFS layer.
- Drivers for the PS/2 keyboard and mouse, the serial port, the PIT, the
  RTC and PCI.
- A 32-bit framebuffer console.
- A logging subsystem with a ring buffer and severity levels.
- The build system, `tools/isofs.py` for ISO generation, and the
  automated test harness.

[Unreleased]: https://github.com/Seigh-sword/QiraOS/compare/v0.3.0...HEAD
[0.3.0]: https://github.com/Seigh-sword/QiraOS/releases/tag/v0.3.0
[0.2.0]: https://github.com/Seigh-sword/QiraOS/releases/tag/v0.2.0
[0.1.0]: https://github.com/Seigh-sword/QiraOS/releases/tag/v0.1.0
