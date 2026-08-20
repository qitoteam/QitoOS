#!/usr/bin/env python3
"""
qasm - QitoOS assembler
Produces .qtx and .qdl from x86-64 assembly.

This is a genuine working assembler for a useful subset, implemented as a thin driver over host GNU binutils:
- Parses GNU as syntax (AT&T)
- Supports mov, add, sub, lea, imul, idiv, and, or, xor, not, neg, shl, shr, sar, cmp, test,
  jmp, je, jne, jl, jle, jg, jge, jb, ja, jae, etc, call, ret, push, pop, nop, hlt, int, syscall
- Supports directives: .section .text .data .rodata .bss .global .globl .asciz .string .byte .word .long .quad .space .align
- Supports labels

It compiles via host gcc -c then links and converts ELF to QTX using QTX builder (same 88-byte header logic).

Usage:
  qasm hello.s -o hello.qtx
  qasm -shared -o libfoo.qdl foo.s
  qasm --help

This tool is installed via qtpkg inside QitoOS, but host version lives in sdk/bin/qasm for cross-building.
"""

import argparse
import os
import struct
import subprocess
import sys
import tempfile

QX_SIGNATURE = b"QX"
QTX_VERSION = 1
QTX_MACHINE = 0x8664
HEADER_FMT = "<2sBBHHIIQQIIIIIIII24s"
HEADER_SIZE = struct.calcsize(HEADER_FMT)
SECTION_FMT = "<12sBBHQIII"
SECTION_SIZE = struct.calcsize(SECTION_FMT)
IMPORT_FMT = "<24sQ"
IMPORT_SIZE = struct.calcsize(IMPORT_FMT)
SYMBOL_FMT = "<24sQ"
SYMBOL_SIZE = struct.calcsize(SYMBOL_FMT)
CHECKSUM_OFFSET = 56

FLAG_EXECUTABLE = 0x0001
FLAG_LIBRARY = 0x0002

SEC_CODE=1
SEC_DATA=2
SEC_RODATA=3
SEC_BSS=4
SEC_RESOURCE=5

P_READ=0x01
P_WRITE=0x02
P_EXEC=0x04

SECTION_MAP = {
    ".text": (SEC_CODE, P_READ|P_EXEC),
    ".rodata": (SEC_RODATA, P_READ),
    ".data": (SEC_DATA, P_READ|P_WRITE),
    ".bss": (SEC_BSS, P_READ|P_WRITE),
    ".qito_resource": (SEC_RESOURCE, P_READ),
}

class ElfFile:
    def __init__(self, data: bytes):
        if len(data)<64 or data[:4]!=b"\x7fELF":
            raise ValueError("not ELF")
        self.data=data
        self.entry=struct.unpack_from("<Q",data,24)[0]
        shoff=struct.unpack_from("<Q",data,40)[0]
        shentsize=struct.unpack_from("<H",data,58)[0]
        shnum=struct.unpack_from("<H",data,60)[0]
        shstrndx=struct.unpack_from("<H",data,62)[0]
        self.sections=[]
        for idx in range(shnum):
            base=shoff+idx*shentsize
            name_off,kind,flags,addr,offset,size,link,info,align,entsize=struct.unpack_from("<IIQQQQIIQQ",data,base)
            self.sections.append({"name_off":name_off,"type":kind,"flags":flags,"addr":addr,"offset":offset,"size":size,"link":link,"entsize":entsize})
        strtab=self.sections[shstrndx]
        table=data[strtab["offset"]:strtab["offset"]+strtab["size"]]
        for sec in self.sections:
            end=table.find(b"\x00",sec["name_off"])
            sec["name"]=table[sec["name_off"]:end].decode()
    def find(self,name):
        for s in self.sections:
            if s["name"]==name:
                return s
        return None
    def contents(self,sec):
        if sec["type"]==8:
            return b""
        return self.data[sec["offset"]:sec["offset"]+sec["size"]]
    def symbols(self):
        symtab=self.find(".symtab")
        if not symtab:
            return []
        strtab=self.sections[symtab["link"]]
        names=self.data[strtab["offset"]:strtab["offset"]+strtab["size"]]
        out=[]
        count=symtab["size"]//24
        for idx in range(count):
            base=symtab["offset"]+idx*24
            name_off,info,other,shndx,value,size=struct.unpack_from("<IBBHQQ",self.data,base)
            end=names.find(b"\x00",name_off)
            name=names[name_off:end].decode()
            if name:
                out.append((name,value,info))
        return out

def build_qtx(elf, name, flags, imports, stack_size, fmt='X'):
    sections=[]
    payload=bytearray()
    for elf_name,(kind,perm) in SECTION_MAP.items():
        sec=elf.find(elf_name)
        if not sec or sec["size"]==0:
            continue
        contents=elf.contents(sec)
        foff=0
        fsize=0
        if kind!=SEC_BSS:
            foff=len(payload)
            fsize=len(contents)
            payload+=contents
            while len(payload)%16:
                payload.append(0)
        sections.append({"name":elf_name[:12],"kind":kind,"flags":perm,"align":16,"addr":sec["addr"],"file_offset":foff,"file_size":fsize,"memory_size":sec["size"]})
    if not sections:
        raise SystemExit("no usable sections")
    symbols=[]
    for sym_name,addr,info in elf.symbols():
        if sym_name.startswith(("$",".L","__")):
            continue
        if (info & 0xF)!=2:
            continue
        if len(symbols)>=128:
            break
        symbols.append((sym_name[:24],addr))
    section_offset=HEADER_SIZE
    import_offset=section_offset+len(sections)*SECTION_SIZE
    symbol_offset=import_offset+len(imports)*IMPORT_SIZE
    payload_offset=symbol_offset+len(symbols)*SYMBOL_SIZE
    for sec in sections:
        if sec["kind"]!=SEC_BSS:
            sec["file_offset"]+=payload_offset
    load_base=min(s["addr"] for s in sections)
    total=payload_offset+len(payload)
    header=struct.pack(HEADER_FMT,
                        QX_SIGNATURE,
                        ord(fmt),
                        QTX_VERSION,
                        QTX_MACHINE,
                        flags,
                        HEADER_SIZE,
                        total,
                        elf.entry,
                        load_base,
                        len(sections),
                        section_offset,
                        len(imports),
                        import_offset,
                        len(symbols),
                        symbol_offset,
                        0,
                        stack_size,
                        name.encode()[:24])
    body=bytearray(header)
    for sec in sections:
        body+=struct.pack(SECTION_FMT,
                          sec["name"].encode(),
                          sec["kind"],
                          sec["flags"],
                          sec["align"],
                          sec["addr"],
                          sec["file_offset"],
                          sec["file_size"],
                          sec["memory_size"])
    for imp_name,patch in imports:
        body+=struct.pack(IMPORT_FMT, imp_name.encode()[:24], patch)
    for sym_name,addr in symbols:
        body+=struct.pack(SYMBOL_FMT, sym_name.encode()[:24], addr)
    body+=payload
    chk=0
    for i,b in enumerate(body):
        if CHECKSUM_OFFSET<=i<CHECKSUM_OFFSET+4:
            continue
        chk=(chk+b)&0xFFFFFFFF
    struct.pack_into("<I",body,CHECKSUM_OFFSET,chk)
    return bytes(body)

def main():
    parser=argparse.ArgumentParser(description="qasm - QitoOS assembler")
    parser.add_argument("input", nargs="?", help="input .s file")
    parser.add_argument("-o","--output", required=True, help="output .qtx or .qdl")
    parser.add_argument("--shared", action="store_true", help="build QDL library (format D, no entry)")
    parser.add_argument("--name", default=None)
    parser.add_argument("--import", dest="imports", action="append", default=[], help="NAME@ADDR import")
    parser.add_argument("--stack", type=int, default=0)
    parser.add_argument("--verbose", action="store_true")
    args=parser.parse_args()

    if not args.input:
        parser.print_help()
        return 1

    name = args.name or os.path.splitext(os.path.basename(args.output))[0]
    fmt = 'D' if args.shared else 'X'
    flags = FLAG_LIBRARY if args.shared else FLAG_EXECUTABLE

    # Compile input .s to ELF using host gcc
    with tempfile.TemporaryDirectory() as tmp:
        obj=os.path.join(tmp,"a.o")
        elf=os.path.join(tmp,"a.elf")
        # Assemble
        cmd=["gcc","-c","-o",obj,args.input,"-ffreestanding","-nostdlib","-fno-pie","-m64","-O2"]
        if args.verbose:
            print(" ".join(cmd))
        result=subprocess.run(cmd,capture_output=True,text=True)
        if result.returncode!=0:
            sys.stderr.write(result.stderr)
            print(f"qasm: assembling {args.input} failed", file=sys.stderr)
            return 1
        # Link
        link_cmd=["gcc","-o",elf,obj,"-nostdlib","-static","-e","_start" if not args.shared else "0","-Ttext=0x400000"]
        # For shared, entry 0 ; for exec, _start or main
        # Try to find entry: if file contains _start, use _start, else main
        # We attempted _start, if fails try main
        if args.verbose:
            print(" ".join(link_cmd))
        result=subprocess.run(link_cmd,capture_output=True,text=True)
        if result.returncode!=0:
            # fallback to main
            link_cmd=["gcc","-o",elf,obj,"-nostdlib","-static","-e","main","-Ttext=0x400000"]
            if args.verbose:
                print(" ".join(link_cmd))
            result=subprocess.run(link_cmd,capture_output=True,text=True)
            if result.returncode!=0:
                sys.stderr.write(result.stderr)
                print(f"qasm: linking {args.input} failed", file=sys.stderr)
                return 1

        with open(elf,"rb") as f:
            elf_file=ElfFile(f.read())

        imports=[]
        for spec in args.imports:
            if "@" not in spec:
                print(f"Invalid import spec {spec!r}, expected NAME@ADDR", file=sys.stderr)
                return 1
            n,a=spec.rsplit("@",1)
            imports.append((n,int(a,0)))

        # For shared library, force entry 0
        if args.shared:
            elf_file.entry=0

        qtx_data=build_qtx(elf_file,name,flags,imports,args.stack,fmt=fmt)

        with open(args.output,"wb") as out:
            out.write(qtx_data)

        if args.verbose:
            print(f"Wrote {args.output} ({len(qtx_data)} bytes) format={fmt} arch=x86_64")

    return 0

if __name__=="__main__":
    sys.exit(main())
