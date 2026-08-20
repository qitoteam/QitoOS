# QTX — Qito eXecutable

`.qtx` is the native executable format of QitoOS, replacing the old `.lqx` (Linked Qira Executables).

## Motivation

ELF carries 30 years of compatibility baggage that a from-scratch kernel does not need, and parsing it safely is most of a day's work. QTX keeps only what Qito actually uses, which makes the loader small enough to audit line by line — the right trade for code that maps attacker-supplied files.

## On-disk layout

```
struct qx_header             88 bytes, begins with "QX"
struct qtx_section[sections] 36 bytes each
struct qtx_import[imports]   32 bytes each, unresolved kernel services
struct qtx_symbol[symbols]   32 bytes each, diagnostics / exports
section payloads             raw bytes
```

All multi-byte fields are little-endian. Everything is offset-addressed from start of file, so image can be validated fully before a single byte is mapped.

Python struct: `<2sBBHHIIQQIIIIIIII24s`

| Off | Size | Field | Off | Size | Field |
|-----|------|-------|-----|------|-------|
| 0 | 2 | "QX" | 32 | 4 | section_count (≤16) |
| 2 | 1 | format 'X' | 36 | 4 | section_offset |
| 3 | 1 | version 1 | 40 | 4 | import_count (≤64) |
| 4 | 2 | machine 0x8664 | 44 | 4 | import_offset |
| 6 | 2 | flags | 48 | 4 | symbol_count (≤128) |
| 8 | 4 | header_size = 88 | 52 | 4 | symbol_offset |
| 12 | 4 | total_size | 56 | 4 | checksum (excludes itself) |
| 16 | 8 | entry_point | 60 | 4 | stack_size |
| 24 | 8 | load_base | 64 | 24 | name |

Section entry 36 B `<12sBBHQIII`; import/symbol 32 B `<24sQ`.

Kinds: 1 code, 2 data, 3 rodata, 4 bss, 5 resource.

Perms: r=1, w=2, x=4.

## Validation

Loader must reject:

- w+x sections (W^X violation)
- out-of-bounds offsets
- entry outside an exec section
- bad checksum (32-bit wrapping sum, checksum field treated as 0)
- section_count 0 or >16, import_count >64, symbol_count >128
- import name not NUL-terminated within 24 bytes
- entry 0 for executable, non-zero for library

Validate fully before mapping anything.

## Production

`.qtx` files may only be produced by `qcc` or `qasm` (see docs/QTPKG.md).

Old Python generator `tools/mklqx.py` is explicitly retired and deleted.

`qasm` is the assembler, `qcc` is the C compiler – both installed via `qtpkg`, not bundled.

```bash
qasm hello.s -o hello.qtx
qcc hello.c -o hello.qtx
qtx info hello.qtx
qtx run hello.qtx arg1 arg2
```

## Execution

QTX images run as genuine user processes with fault isolation (Ring 3).

- GDT selectors: null 0x00, kcode 0x08, kdata 0x10, udata 0x18, ucode 0x20, TSS 0x28
- IST 1 = #DF, 2 = NMI, 3 = #PF
- Task creates its own address space (user half), maps code as R-X, data as RW-, stack as RW- at high user VA (0x7FFFFFE00000)
- TSS rsp0 points to kernel stack for interrupts from user mode
- Page fault in user mode kills task, not kernel panic (see `idt.c` handle_exception)

## Dynamic linking

Imports are resolved first against kernel export table (`qtx_export`), then against loaded QDLs (`/lib/*.qdl`), on-demand loading with refcount.

## Example

Minimal QTX program in assembly:

```asm
.section .text
.global _start
_start:
    mov $1, %rdi
    lea msg(%rip), %rsi
    call console_puts
    ret
.section .rodata
msg: .asciz "Hello from QTX!\n"
```

See `sdk/` for C headers and `examples/hello.c` for C example.

## Tools

- `qtx info <file>` – describe header
- `qtx verify <file>` – validate checksum and offsets
- `qtx run <file>` – load and run as user task
- `qtx exports` – list kernel services available to QTX
