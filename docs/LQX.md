# LQX — Linked Qira Executables

`.lqx` is the native executable format of Qira OS. It is what the kernel
loads when you run a program that did not come compiled into the kernel
image.

- **Signature:** `QX` (ASCII, two bytes) followed by a format byte `L`
- **Extension:** `.lqx`
- **Endianness:** little-endian throughout
- **Machine:** x86-64 only (`0x8664`)
- **Reference implementation:** [`src/kernel/sys/lqx.c`](../src/kernel/sys/lqx.c)
- **Header:** [`src/kernel/include/kernel/lqx.h`](../src/kernel/include/kernel/lqx.h)
- **Builder:** [`tools/mklqx.py`](../tools/mklqx.py)

---

## Why not ELF

ELF is the obvious choice and Qira OS deliberately does not use it. ELF
carries three decades of compatibility surface — program headers *and*
section headers describing overlapping views of the same bytes, a dozen
relocation types per architecture, dynamic tables, versioning records,
`PT_GNU_*` extensions. A loader that parses ELF *correctly* is a large
piece of code, and a loader that parses it *safely* — remember that it is
mapping attacker-supplied files into an address space — is larger still.

LQX keeps only what Qira actually uses:

| Concern | ELF | LQX |
| --- | --- | --- |
| Views of the file | program headers + section headers | one section table |
| Relocations | ~30 types per arch | none; images are pre-linked |
| Dynamic linking | `.dynamic`, `.got`, `.plt`, symbol versioning | a flat import table |
| Loader size | thousands of lines to do safely | ~400 lines, auditable in one sitting |

The trade-off is real: LQX images are not portable to other operating
systems, and there is no shared-library machinery. For a hobby kernel that
compiles its own userspace, neither matters.

### What "linked" means

An LQX image is **fully linked internally**. All intra-image references are
already resolved for a fixed load base, so the loader performs zero
relocation work. What it *does* resolve are **imports** — the kernel
services the program calls. Each import names a function; the loader looks
the name up in the kernel export table and patches the resolved address
into the program's import slot before transferring control. That is the
entire dynamic-linking story, and it fits in one loop.

---

## File layout

```
offset 0    ┌───────────────────────────────┐
            │ struct qx_header      88 bytes│
            ├───────────────────────────────┤
            │ struct lqx_section[]  36 each │   section_count entries
            ├───────────────────────────────┤
            │ struct lqx_import[]   32 each │   import_count entries
            ├───────────────────────────────┤
            │ struct lqx_symbol[]   32 each │   symbol_count entries
            ├───────────────────────────────┤
            │ section payloads              │   referenced by file offset
            └───────────────────────────────┘
```

Every table is located by an explicit offset stored in the header, so the
tables need not be adjacent and the whole image can be validated before a
single byte is mapped.

---

## Header — 88 bytes

Python `struct` format: `<2sBBHHIIQQIIIIIIII24s`

| Offset | Size | Field | Notes |
| ---: | ---: | --- | --- |
| 0 | 2 | `signature` | `"QX"` |
| 2 | 1 | `format` | `'L'` — Linked |
| 3 | 1 | `version` | `1` |
| 4 | 2 | `machine` | `0x8664` |
| 6 | 2 | `flags` | see below |
| 8 | 4 | `header_size` | `88` |
| 12 | 4 | `total_size` | total file length in bytes |
| 16 | 8 | `entry_point` | virtual address; must fall inside an executable section |
| 24 | 8 | `load_base` | virtual address the image was linked for |
| 32 | 4 | `section_count` | ≤ 16 |
| 36 | 4 | `section_offset` | file offset of the section table |
| 40 | 4 | `import_count` | ≤ 64 |
| 44 | 4 | `import_offset` | file offset of the import table |
| 48 | 4 | `symbol_count` | ≤ 128 |
| 52 | 4 | `symbol_offset` | file offset of the symbol table |
| **56** | 4 | **`checksum`** | see *Checksum* below |
| 60 | 4 | `stack_size` | bytes of stack the program wants |
| 64 | 24 | `name` | NUL-padded image name |

### Header flags

| Bit | Name | Meaning |
| ---: | --- | --- |
| `0x0001` | `LQX_FLAG_EXECUTABLE` | a runnable program |
| `0x0002` | `LQX_FLAG_LIBRARY` | exports symbols, has no entry point |
| `0x0004` | `LQX_FLAG_SERVICE` | runs as a background service |
| `0x0008` | `LQX_FLAG_DESKTOP` | registers a desktop application |
| `0x0010` | `LQX_FLAG_PRIVILEGED` | requests elevated capabilities |

### Checksum

A 32-bit wrapping sum over **every byte of the file except the four bytes
of the checksum field itself** (offsets 56–59, which are treated as zero).
A builder writes zero there, sums the image, and stores the result. The
loader repeats the computation and rejects a mismatch.

This is an integrity check against truncation and corruption. It is **not**
a signature and provides no authenticity guarantee.

---

## Section table — 36 bytes per entry

Python `struct` format: `<12sBBHQIII`

| Offset | Size | Field | Notes |
| ---: | ---: | --- | --- |
| 0 | 12 | `name` | NUL-padded, e.g. `.text` |
| 12 | 1 | `kind` | see kinds table |
| 13 | 1 | `flags` | permission bits |
| 14 | 2 | `align` | required alignment in bytes |
| 16 | 8 | `vaddr` | virtual address of the mapping |
| 24 | 4 | `file_offset` | 0 for BSS |
| 28 | 4 | `file_size` | 0 for BSS |
| 32 | 4 | `mem_size` | ≥ `file_size`; the tail is zero-filled |

### Section kinds

| Value | Name | Typical permissions |
| ---: | --- | --- |
| 1 | `LQX_SECTION_CODE` | `r-x` |
| 2 | `LQX_SECTION_DATA` | `rw-` |
| 3 | `LQX_SECTION_RODATA` | `r--` |
| 4 | `LQX_SECTION_BSS` | `rw-`, no file bytes |
| 5 | `LQX_SECTION_RESOURCE` | `r--`, icons and other assets |

### Permission bits

| Bit | Meaning |
| ---: | --- |
| `0x01` | readable |
| `0x02` | writable |
| `0x04` | executable |

---

## Import and symbol tables — 32 bytes per entry

Both tables share a layout. Python `struct` format: `<24sQ`

| Offset | Size | Field |
| ---: | ---: | --- |
| 0 | 24 | `name`, NUL-padded |
| 24 | 8 | `value` — resolved address (imports) or symbol address (symbols) |

Imports are written with `value = 0` by the builder and patched by the
loader. Symbols exist purely for diagnostics: `lqx info` prints them and
the backtrace code can attribute an address to a name.

---

## Validation rules

`lqx_validate()` rejects an image if any of the following hold. The checks
run in this order, before any mapping happens.

1. The file is shorter than 88 bytes.
2. `signature` is not `"QX"`, `format` is not `'L'`, or `version` is not `1`.
3. `machine` is not `0x8664`.
4. `header_size` is not 88, or `total_size` disagrees with the actual length.
5. Any count exceeds its maximum (16 sections, 64 imports, 128 symbols).
6. Any table offset plus its length runs past the end of the file.
7. Any section's `file_offset + file_size` runs past the end of the file.
8. Any section's `mem_size` is smaller than its `file_size`.
9. **A section is both writable and executable.** W^X is enforced at load
   time, not merely recommended.
10. `entry_point` does not fall inside a section carrying `LQX_SEC_EXEC`
    (skipped for `LQX_FLAG_LIBRARY` images, which have no entry point).
11. The checksum does not match.

Section address ranges are also checked for overlap.

---

## Kernel exports

The kernel publishes 27 services that an LQX image may import. List them
from a shell:

```
ush> lqx exports
```

They cover console output, memory allocation, the filesystem, timing,
string helpers, and process control. The export table is defined in
[`src/kernel/sys/lqx.c`](../src/kernel/sys/lqx.c); an unresolved import is
a load-time error, so a program can never reach a NULL service pointer.

---

## Building an image

`tools/mklqx.py` contains a minimal ELF64 reader. You compile a program
with the ordinary toolchain, then convert the result:

```sh
gcc -std=gnu11 -ffreestanding -fno-builtin -nostdlib -nostdinc \
    -fno-pie -fno-stack-protector -mno-red-zone -m64 -O2 \
    -c examples/hello.c -o build/hello.o

ld -nostdlib -static -Ttext 0x400000 build/hello.o -o build/hello.elf

python3 tools/mklqx.py --input build/hello.elf --output rootfs/bin/hello.lqx \
    --name hello --stack 16384
```

`make ramdisk` does this for the bundled example, producing the 404-byte
`rootfs/bin/hello.lqx` that ships in the ISO.

Inspect an image from the host:

```sh
python3 tools/mklqx.py --info rootfs/bin/hello.lqx
```

…or from inside the running OS:

```
ush> lqx info /bin/hello.lqx
ush> lqx exports
```

---

## Limitations

- No relocations: an image is linked for one fixed base address.
- No shared libraries. `LQX_FLAG_LIBRARY` reserves the concept; the loader
  does not yet resolve imports across two LQX images.
- No debug-information format. The symbol table carries names and
  addresses only — no line numbers, no types.
- The checksum is an integrity check, not a signature. Code signing is on
  the roadmap and is not implemented.
