# Contributing to QitoOS

Thanks for your interest. QitoOS is a from-scratch x86-64 operating system, and contributions are welcome — from typo fixes to device drivers, QTX programs, QTI icons, qtpkg packages.

- **Maintainer:** Seigh-sword
- **Contact:** zack.yt.7085@gmail.com
- **Licence:** Apache License 2.0
- **Repo:** https://github.com/qitoteam/QitoOS
- **Registry:** https://github.com/qitoteam/qtpkg-registry (package requests via issues tab)

---

## Before you start

If you are planning anything larger than a bug fix, **open an issue first**. A driver or subsystem touches design decisions easier to discuss before code exists.

Good first contributions:

- New UltraShell or QCSH commands (see [docs/SHELLS.md](docs/SHELLS.md))
- New desktop applications
- Additional test coverage
- Documentation corrections
- Extra QTI icons (real PNG/RGBA via `tools/mkqti.py`, 5 sizes 16/32/64/128/256) or new font face
- QTX programs using SDK (`sdk/include/qito/`, `examples/hello.c`)
- QDL libraries for `/lib/*.qdl`
- qtpkg packages (add entry to `/user/qtpkg/entry.var` with `[version] (url);` syntax)

---

## Setting up

You need Linux host — or WSL, or container — with GCC toolchain capable of freestanding x86-64 binaries, GNU `ld`, Make, Python 3.8+, emulator (QEMU or Bochs).

```sh
git clone https://github.com/qitoteam/QitoOS.git
cd QitoOS
./scripts/setup-dev.sh     # checks tools, reports missing
make                       # builds build/qito-os.iso
make run                   # boots it
```

No cross-compiler: host GCC with `-ffreestanding -nostdlib`, no host C library.

---

## The development loop

```sh
make            # kernel and ISO
make test-unit  # host-side checks, seconds, 147 tests
make test-boot  # boots ISO in emulator, 2-3 minutes, 87 checks
make test       # both
```

Run `make test-unit` constantly — fast, catches most. Run `make test-boot` before push.

Useful targets:

| Target | What it does |
|--------|--------------|
| `make iso` | build bootable image |
| `make run` | build and boot it |
| `make run-nographic` | boot without graphics |
| `make run-bochs` | boot in Bochs |
| `make screenshots` | regenerate `docs/screenshots/` |
| `make clean` | remove build output |
| `make info` | print build config |

### Debugging

Serial output is most useful:

```sh
make iso CMDLINE_EXTRA="loglevel=4 echo=serial"
qemu-system-x86_64 -cdrom build/qito-os.iso -serial stdio -m 512
```

Drive OS without keyboard (how tests work):

```sh
make iso CMDLINE_EXTRA="autorun=selftest;diag echo=serial"
```

Remember **underscores become spaces** inside `autorun`, so `ls_-l_/etc` runs `ls -l /etc`.

For GDB:

```sh
./scripts/run-qemu.sh --gdb
# in another terminal:
gdb build/qito-kernel.elf -ex 'target remote :1234'
```

---

## Coding standards

### Language

C11 (`-std=gnu11`) and GNU assembler syntax. No nasm and no Rust.

Kernel is freestanding: **no libc**. No `stdio.h`, `stdlib.h`, `string.h` from host. Use kernel's own facilities in `src/kernel/include/kernel/` and `src/lib/`.

### Style

- Four-space indentation, no tabs.
- Ninety-column soft limit.
- Braces same line for control flow, own line for function definitions.
- `snake_case` for functions/variables; `UPPER_CASE` for macros/constants.
- Prefix public functions with subsystem: `vfs_open`, `sched_yield`, `qti_decode`, `qtx_load`, `qdl_load`, `qtpkg_install`.
- Declare variables close to first use.
- Always brace body of `if`, even single statement.

### Comments

Explain **why**, not what. See header comments in `qtx.h` or `qti.h` for intended tone.

### Things that will bite you

Real problems that cost debugging time:

- **`offsetof` is not available.** Compute offsets by hand.
- **Kernel task stacks are 64 KB.** Buffer over ~1 KB in shell command will overflow. Use `shell_scratch()`.
- **`KEY_ENTER == '\n'` and `KEY_BACKSPACE == '\b'`.** Both as `case` labels is duplicate-case compile error.
- **Never call `shell_run()` from inside windowed terminal.** Reenters event loop.
- **Interrupt handlers must not allocate.** Heap not reentrant.
- **Editing `stage2.S` does not always trigger bootloader rebuild.** `rm -f build/boot.bin build/boot/stage2.o && make bootloader`.
- **Set `scheduler_running` after logging, with interrupts masked** – otherwise first timer tick preempts mid-write and corrupts output.
- **Boot sector layout:** offset `0x00 jmp+nop`, `0x03 signature QITO1`, `0x08 56-byte El Torito boot info table – reserved, must not hold code`, `0x40 code starts`, `0x1FE 0xAA55`, `0x200 stage2`. Code at offset 8 silently overwritten when booting from CD.
- **Physical memory contract must agree:** bounce `0x10000`, boot info `0x18000`, E820 map `0x19000`, PML4 `0x20000`, stack `0x90000`, kernel `0x100000`, ramdisk `0x2000000`, entry virtual `0xFFFFFFFF80100000`, RDI boot info, RAX magic QTBI `0x51544249`.
- **Selectors:** null `0x00`, kcode `0x08`, kdata `0x10`, udata `0x18`, ucode `0x20`, TSS `0x28`, IST 1=#DF 2=NMI 3=#PF, PIC remapped 32-47, int 0x80 DPL3.
- **Kernel cmdline 256 bytes** in three places: `bootinfo.h`, `CMDLINE_SIZE` in `mkiso.py`, `.space` directive in `stage2.S`.
- **Font geometry:** 8×16 cell, art grid 14 rows with 1 row top padding, caps rows 2-11, baseline 12, descenders 13-14, bold derived row|(row>>1), unit test asserts descenders descend.

---

## Testing requirements

Every functional change needs a test. Suites:

**`tests/run_unit_tests.py`** – host-side static and structural checks: format layouts (QTX 88-byte header `<2sBBHHIIQQIIIIIIII24s`, section 36B, import/symbol 32B, QTI 32B header, 16B entry, QitoFS), generated data, source-tree organisation, tool round-trips. Fast; run constantly. 147 tests.

**`tests/run_boot_tests.py`** – boots ISO in emulator, drives it through `autorun`, asserts on serial log and captured framebuffer (RLE base64 --QITO-FRAME-BEGIN). Checks QitoFS mount, icons load, QTX loader, QDL, qtpkg, fonts, etc.

**`selftest`** – assertions inside kernel, covering heap, PMM/VMM, scheduler, filesystem, strings, QTX/QDL/QTI, AHCI, gfx3d, etc. Add cases for anything that can only be verified at runtime.

Both suites must pass before PR can be merged. CI enforces.

---

## Submitting a pull request

1. Fork the repository and branch from `main`.
2. Make change, with tests.
3. Run `make test` and make sure green.
4. Update documentation. **This matters:** README must never claim feature not implemented. If you add command, format, app, update relevant doc in `docs/` (QTX.md, QDL.md, QTI.md, QTPKG.md, etc) and add entry to `CHANGELOG.md` under *Unreleased*.
5. Write clear commit message: short imperative summary line, blank line, then explanation of *why* needed.

   ```
   net: retry DNS queries on timeout

   A single UDP query is dropped often enough on a loaded host that
   name resolution failed intermittently. Retry twice with 500 ms
   timeout before reporting failure.
   ```

6. Open PR and describe what you changed, why, how you tested.

### Review

Maintainer reviews PRs. Expect questions about design decisions – point of review, not criticism.

---

## Reporting bugs

Open an issue with:

- what you did, expected, happened;
- output of `make info`;
- emulator or hardware you ran on;
- serial log captured with `loglevel=4 echo=serial`, if you can.

A serial log makes difference between bug fixed and cannot be reproduced.

### Package requests

QitoOS uses `qtpkg` now, not git. Package requests go through repo issues tab: https://github.com/qitoteam/QitoOS/issues and registry https://github.com/qitoteam/qtpkg-registry.

For qtpkg issues, include:

- entry.var line that failed (with line number)
- profile URL and error message (including TLS not supported yet if https)
- payload URL

### Security issues

Email zack.yt.7085@gmail.com rather than public issue.

Be realistic about threat model: QitoOS is hobby OS. It has xoshiro256** RNG (not crypto), SHA-256 + Ed25519 for packages (real SHA-256, Ed25519 stub API ready), W^X, checksum validation, Ring3 fault isolation kills task not kernel, snapshots/rollback. No ASLR yet, no full TLS (honest error). Do not run on anything you care about.

---

## qtpkg, qcc, qasm specifics

- **qtpkg**: entry file `/user/qtpkg/entry.var` syntax `pkg = [version] (url),[version] (url);` # comment, one per line, ; terminated. Parser with line numbers in `src/kernel/sys/qtpkg.c`. Each URL points at `.qtpkg_profile` manifest with name, version, description, arch, dependencies, install paths, checksums, payload URL, signature. Commands: install, update, -os update, upgrade, -fix os, -fix --driver amd64|intel, list/search/remove/info/rollback. TLS honest: https gives clear error, plain HTTP mirrors work. See `docs/QTPKG.md`.

- **qcc/qasm**: installed via qtpkg, not bundled. qasm is genuine working x86-64 assembler useful subset (mov, add, sub, lea, jmp, call, ret, etc, directives .section .text .data etc). qcc is real C subset compiler (int/char/void/pointers/arrays/structs, if/else/while/for/return, + - * / % & | ^ etc, #include/#define). Documented exactly in `sdk/include/qito/qcc.h` and `docs/QTX.md`. Do not overclaim. Host versions in `sdk/bin/` and `tools/qasm.py`/`tools/qcc.py` are thin drivers over GCC that emit QTX (88-byte header, format X/D, checksum, W^X). Inside QitoOS, shell commands `qasm` and `qcc` produce minimal valid QTX for MVP, host versions do full compile.

- **SDK**: `sdk/` folder with headers for C where users can create programs/apps using SDK: `include/qito/` console.h, fs.h, string.h, stdlib.h, qtx.h, qdl.h, qti.h, qcc.h, lib/libq.a, crt0.o, qtx.ld, bin/qasm+qcc, examples/hello.c. See `sdk/README.md`.

---

## Licence

By contributing you agree contributions are licensed under Apache License 2.0, same terms covering rest of project.
