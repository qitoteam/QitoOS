#!/usr/bin/env python3
"""
qcc - QitoOS C compiler
Produces .qtx and .qdl from C source.

This is a real compiler for a C subset, implemented as a thin driver over host GCC,
with QTX emission. It documents exactly what it does and doesn't support.

Supports:
- int, char, void, pointers, arrays, structs (no bitfields, no packed __attribute__ except section)
- if, else, while, for, return, function calls, switch (limited)
- + - * / % & | ^ << >> == != < > <= >= && || ! ~ unary, sizeof (limited)
- #include, #define (simple), #ifdef
- Functions, global variables, local variables

Does NOT support:
- float, double, long double
- long long beyond 64-bit (int is 32-bit, long is 64-bit on host)
- goto (allowed but discouraged)
- setjmp/longjmp
- variadic args (printf is special kernel-provided)
- C++ features
- _Generic, _Atomic, threads
- inline asm (use qasm instead)
- Complex struct passing by value (use pointers)

It works by:
1. Compiling C to ELF object with host GCC -ffreestanding -nostdlib -fno-pie -m64
2. Linking to ELF executable with -static -e main/_start -Ttext=0x400000
3. Converting ELF to QTX via same builder as qasm (QX header 88 bytes, format X or D)

Usage:
  qcc hello.c -o hello.qtx
  qcc -shared -o libfoo.qdl foo.c
  qcc -I sdk/include -o prog.qtx prog.c -L sdk/lib -lq
  qcc --help

This tool is installed via qtpkg inside QitoOS (host version in sdk/bin/qcc).

Be pragmatic: full C compiler is enormous, so qcc is a thin driver but it DOES produce real working QTX/QDL.
Do not overclaim – see docs/QTX.md and sdk/include/qito/qcc.h
"""

import argparse
import os
import struct
import subprocess
import sys
import tempfile

# Reuse QTX builder from qasm
from qasm import build_qtx, ElfFile, FLAG_EXECUTABLE, FLAG_LIBRARY

def main():
    parser=argparse.ArgumentParser(description="qcc - QitoOS C compiler (subset, thin driver over GCC)")
    parser.add_argument("inputs", nargs="+", help="C source files")
    parser.add_argument("-o","--output", required=True, help="output .qtx or .qdl")
    parser.add_argument("-I","--include", action="append", default=[], help="include paths")
    parser.add_argument("-L","--libpath", action="append", default=[], help="library paths (ignored, for compatibility)")
    parser.add_argument("-l","--lib", action="append", default=[], help="libraries (ignored)")
    parser.add_argument("--shared", action="store_true", help="build QDL library")
    parser.add_argument("--name", default=None)
    parser.add_argument("--import", dest="imports", action="append", default=[], help="NAME@ADDR import e.g. console_puts@0x402000")
    parser.add_argument("--stack", type=int, default=0)
    parser.add_argument("--verbose", action="store_true")
    parser.add_argument("-D","--define", action="append", default=[], help="defines")
    parser.add_argument("-O","--optimize", default="2")
    args=parser.parse_args()

    name = args.name or os.path.splitext(os.path.basename(args.output))[0]
    flags = FLAG_LIBRARY if args.shared else FLAG_EXECUTABLE
    fmt = 'D' if args.shared else 'X'

    with tempfile.TemporaryDirectory() as tmp:
        objs=[]
        for src in args.inputs:
            obj=os.path.join(tmp, os.path.basename(src)+".o")
            cmd=["gcc","-c","-o",obj,src,
                 "-ffreestanding","-nostdlib","-fno-pie","-fno-pic",
                 "-fno-stack-protector",
                 "-m64","-O"+str(args.optimize),
                 "-std=gnu11","-Wall","-Wextra","-Wno-unused-parameter"]
            for inc in args.include:
                cmd+=["-I",inc]
            for d in args.define:
                cmd+=["-D",d]
            # Disable some warnings that are noisy for freestanding
            cmd+=["-Wno-builtin-declaration-mismatch"]
            if args.verbose:
                print(" ".join(cmd))
            result=subprocess.run(cmd,capture_output=True,text=True)
            if result.returncode!=0:
                sys.stderr.write(result.stderr)
                print(f"qcc: compile {src} failed", file=sys.stderr)
                return 1
            objs.append(obj)

        elf=os.path.join(tmp,"a.elf")
        # Link all objects into one ELF
        link_cmd=["gcc","-o",elf]+objs+["-nostdlib","-static"]
        if args.shared:
            link_cmd+=["-e","0","-Ttext=0x400000"]
        else:
            # Try _start then main
            link_cmd+=["-e","_start","-Ttext=0x400000"]
        if args.verbose:
            print(" ".join(link_cmd))
        result=subprocess.run(link_cmd,capture_output=True,text=True)
        if result.returncode!=0 and not args.shared:
            # fallback to main
            link_cmd=["gcc","-o",elf]+objs+["-nostdlib","-static","-e","main","-Ttext=0x400000"]
            if args.verbose:
                print(" ".join(link_cmd))
            result=subprocess.run(link_cmd,capture_output=True,text=True)
        if result.returncode!=0:
            sys.stderr.write(result.stderr)
            print("qcc: linking failed", file=sys.stderr)
            return 1

        with open(elf,"rb") as f:
            elf_file=ElfFile(f.read())

        imports=[]
        for spec in args.imports:
            if "@" not in spec:
                print(f"Invalid import {spec}", file=sys.stderr)
                return 1
            n,a=spec.rsplit("@",1)
            imports.append((n,int(a,0)))

        if args.shared:
            elf_file.entry=0

        qtx_data=build_qtx(elf_file,name,flags,imports,args.stack,fmt=fmt)
        with open(args.output,"wb") as out:
            out.write(qtx_data)
        if args.verbose:
            print(f"Wrote {args.output} ({len(qtx_data)} bytes) fmt={fmt}")

    return 0

if __name__=="__main__":
    sys.exit(main())
