# Contributing to Qira OS

Thanks for your interest. Qira OS is a from-scratch x86-64 operating
system, and contributions are welcome — from typo fixes to device
drivers.

- **Maintainer:** Seigh-sword
- **Contact:** zack.yt.7085@gmail.com
- **Licence:** Apache License 2.0

---

## Before you start

If you are planning anything larger than a bug fix, **open an issue
first**. A driver or a subsystem touches design decisions that are easier
to discuss before the code exists than after.

Good first contributions:

- New UltraShell or QCSH commands (see [docs/SHELLS.md](docs/SHELLS.md))
- New desktop applications
- Additional test coverage
- Documentation corrections
- Extra QAC icons or a new font face

---

## Setting up

You need a Linux host — or WSL, or a container — with a GCC toolchain
capable of producing freestanding x86-64 binaries, GNU `ld`, GNU Make and
Python 3.8 or newer. An emulator (QEMU or Bochs) is needed to run the
boot tests.

```sh
git clone https://github.com/Seigh-sword/QiraOS.git
cd QiraOS
./scripts/setup-dev.sh     # checks for the tools and reports what is missing
make                       # builds build/qira-os.iso
make run                   # boots it
```

There is no cross-compiler requirement: the host GCC is used with
`-ffreestanding -nostdlib`, and the build does not link against the host
C library.

---

## The development loop

```sh
make            # build the kernel and the ISO
make test-unit  # host-side checks, a couple of seconds
make test-boot  # boots the ISO in an emulator, two to three minutes
make test       # both
```

Run `make test-unit` constantly — it is fast and catches most mistakes.
Run `make test-boot` before you push.

Useful targets:

| Target | What it does |
| --- | --- |
| `make iso` | build the bootable image |
| `make run` | build and boot it |
| `make screenshots` | regenerate `docs/screenshots/` |
| `make clean` | remove build output |
| `make info` | print the build configuration |

### Debugging

Serial output is the single most useful tool:

```sh
make iso CMDLINE_EXTRA="loglevel=4 echo=serial"
qemu-system-x86_64 -cdrom build/qira-os.iso -serial stdio -m 512
```

You can also drive the OS without a keyboard, which is how the tests
work:

```sh
make iso CMDLINE_EXTRA="autorun=selftest;diag echo=serial"
```

Remember that **underscores become spaces** inside `autorun`, so
`ls_-l_/etc` runs `ls -l /etc`.

For a source-level debugger, see the GDB section of the README.

---

## Coding standards

### Language

C11 (`-std=gnu11`) and GNU assembler syntax. There is no nasm and no
Rust in this project.

The kernel is freestanding: **no libc**. No `stdio.h`, no `stdlib.h`, no
`string.h` from the host. Use the kernel's own facilities in
`src/kernel/include/kernel/` and `src/lib/`.

### Style

- Four-space indentation, no tabs.
- Ninety-column soft limit.
- Braces on the same line for control flow, on their own line for
  function definitions.
- `snake_case` for functions and variables; `UPPER_CASE` for macros and
  constants.
- Prefix public functions with their subsystem: `vfs_open`, `sched_yield`,
  `qac_decode`.
- Declare variables close to first use.
- Always brace the body of an `if`, even a single statement.

### Comments

Explain **why**, not what. A comment that restates the code is noise; a
comment that records the reason for a non-obvious decision saves the next
person an hour. The existing code follows this — see the header comments
in `lqx.h` or `qac.h` for the intended tone.

### Things that will bite you

These are real problems that have already cost debugging time in this
codebase:

- **`offsetof` is not available.** Compute member offsets by hand.
- **Kernel task stacks are 64 KB.** A buffer over about 1 KB in a shell
  command will overflow one. Use `shell_scratch()`.
- **`KEY_ENTER == '\n'` and `KEY_BACKSPACE == '\b'`.** Using both as
  `case` labels in one `switch` is a duplicate-case compile error.
- **Never call `shell_run()` from inside a windowed terminal.** It
  reenters the event loop.
- **Interrupt handlers must not allocate.** The heap is not reentrant.
- **Editing `stage2.S` does not always trigger a bootloader rebuild.**
  `rm -f build/boot.bin build/boot/stage2.o && make bootloader`.

---

## Testing requirements

Every functional change needs a test. The suites are:

**`tests/run_unit_tests.py`** — host-side static and structural checks:
format layouts, generated data, source-tree organisation, tool round
trips. Fast; run it constantly.

**`tests/run_boot_tests.py`** — boots the ISO in an emulator, drives it
through `autorun`, and asserts on the serial log and on a captured
framebuffer.

**`selftest`** — assertions that run *inside* the kernel, covering the
heap, the physical and virtual memory managers, the scheduler, the
filesystem and the string routines. Add cases here for anything that can
only be verified at runtime.

Both suites must pass before a pull request can be merged. CI enforces
this.

---

## Submitting a pull request

1. Fork the repository and branch from `main`.
2. Make your change, with tests.
3. Run `make test` and make sure it is green.
4. Update the documentation. **This matters:** the README must never
   claim a feature that is not implemented. If your change adds a
   command, a format or an application, update the relevant document in
   `docs/` and add an entry to `CHANGELOG.md` under *Unreleased*.
5. Write a clear commit message: a short imperative summary line, a blank
   line, then an explanation of *why* the change is needed.

   ```
   net: retry DNS queries on timeout

   A single UDP query is dropped often enough on a loaded host that
   name resolution failed intermittently. Retry twice with a 500 ms
   timeout before reporting failure.
   ```

6. Open the pull request and describe what you changed, why, and how you
   tested it.

### Review

The maintainer reviews pull requests. Expect questions about design
decisions — that is the point of review, not a criticism of the patch.

---

## Reporting bugs

Open an issue with:

- what you did, what you expected, and what happened;
- the output of `make info`;
- the emulator or hardware you ran on;
- a serial log captured with `loglevel=4 echo=serial`, if you can get
  one.

A serial log makes the difference between a bug that is fixed and one
that cannot be reproduced.

### Security issues

Please email zack.yt.7085@gmail.com rather than opening a public issue.

That said, be realistic about the threat model: Qira OS is a hobby
operating system. It has no cryptographic RNG, no code signing, no
address-space randomisation, and no privilege isolation worth relying on.
Do not run it on anything you care about.

---

## Licence

By contributing you agree that your contributions are licensed under the
Apache License 2.0, the same terms that cover the rest of the project.
