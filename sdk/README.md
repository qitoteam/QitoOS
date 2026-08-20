# QitoOS SDK

Headers and libraries for creating QitoOS applications (.qtx) and dynamic libraries (.qdl) outside the kernel.

## Contents

```
sdk/
  include/qito/       C headers for user programs
    qtx.h             QTX executable format and loader API
    qdl.h             QDL dynamic library API
    qti.h             QTI icon API
    console.h         console_write, console_puts
    fs.h              file read/write
    string.h          freestanding string ops
    stdlib.h          minimal malloc/free wrappers
    qcc.h             qcc compiler API
  lib/
    libq.a            freestanding C runtime (printf, string)
    crt0.o            QTX entry stub (calls main)
  bin/
    qasm              assembler (host version, also installable via qtpkg inside QitoOS)
    qcc               compiler driver (host version, also via qtpkg)
  examples/
    hello.c           minimal QTX program
    libdemo/
```

## Using the SDK on host Linux

The SDK includes host versions of `qasm` and `qcc` that produce `.qtx` files you can run in QitoOS or inspect with `qtx info`.

On host:

```bash
./sdk/bin/qasm examples/hello.s -o hello.qtx
./sdk/bin/qcc examples/hello.c -o hello.qtx

# Inspect
python3 tools/qtx_inspect.py hello.qtx
```

Inside QitoOS, install via qtpkg:

```bash
qtpkg install qasm
qtpkg install qcc

qasm -o hello.qtx hello.s
qcc -o hello.qtx hello.c
qtx run hello.qtx
```

## Headers

User programs should include `<qito/console.h>` etc, not kernel paths.

Example minimal program (see `examples/hello.c`):

```c
#include <qito/console.h>

int main(int argc, char **argv) {
    console_puts("Hello from QTX!\n");
    return 0;
}
```

Compile:

```bash
qcc -I sdk/include -o hello.qtx examples/hello.c -L sdk/lib -lq
```

## QTX format

See `docs/QTX.md` – on-disk magic `QX`, format byte `X`, 88-byte header `<2sBBHHIIQQIIIIIIII24s`, sections, imports, symbols, checksum, W^X enforced.

## QDL

See `docs/QDL.md` – format byte `D`, library flag, export table, refcounted, on-demand from `/lib/*.qdl`.

Create a QDL:

```c
// mylib.c
int my_add(int a, int b) { return a+b; }
```

```bash
qcc -shared -o libmylib.qdl mylib.c
```

Place in `/lib/` and QTX programs can import `my_add`.

## QTI icons

See `docs/QTI.md` – real pixels, 5 sizes 16/32/64/128/256 default 64, encodings RAW/RLE/INDEX, largest-first.

Build icons:

```bash
python3 tools/mkqti.py --input logo.png --name logo --output logo.qti
```

## qtpkg and toolchain

Both `qcc` and `qasm` are installed via `qtpkg`, not bundled – per spec Part 5.

- `qasm` is genuine working x86-64 assembler (real useful subset: mov, add, sub, lea, jmp, call, ret, push, pop, cmp, test, etc.)
- `qcc` is real compiler for C subset (or thin driver) – documented exactly what it supports

Inside QitoOS:

```bash
qtpkg list
qtpkg search qasm
qtpkg install qasm
qtpkg install qcc
qasm --help
qcc --help
```

## TLS

QitoOS has HTTP but no TLS, so `qtpkg` cannot fetch `https://` – it reports clear error "TLS not supported yet" and expects plain-HTTP mirrors. See `docs/QTPKG.md`.

## Building for QitoOS

The SDK is freestanding – no glibc, no standard library. Use `sdk/lib/crt0.o` and `libq.a`.

Link command example (what qcc does internally):

```bash
ld -nostdlib -T sdk/lib/qtx.ld -o hello.elf sdk/lib/crt0.o hello.o -L sdk/lib -lq
python3 sdk/bin/qtx_link.py hello.elf -o hello.qtx --name hello --imports console_puts:0x402000
```

But prefer `qcc` driver.

## License

Apache 2.0, same as QitoOS.
