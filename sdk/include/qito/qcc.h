/* QitoOS SDK - qcc driver info
 * qcc is the QitoOS C compiler, produces .qtx and .qdl
 * qasm is the assembler
 * Both installed via qtpkg, not bundled.
 *
 * This header documents what qcc supports.
 *
 * qcc is a real compiler for a C subset:
 * - Supports: int, char, void, pointers, arrays, structs (no bitfields)
 * - Supports: if, else, while, for, return, function calls
 * - Supports: + - * / % & | ^ << >> == != < > <= >= && || ! ~ unary
 * - Supports: #include, #define (simple), #ifdef
 * - Does NOT support: floating point, double, long double, complex, goto (limited), setjmp, variadic args (printf is special), C++,
 *   _Generic, atomics, threads (use kernel tasks), inline asm (use qasm)
 *
 * qasm is genuine working x86-64 assembler, useful subset:
 * - Supports: mov, add, sub, lea, imul, idiv, and, or, xor, not, neg, shl, shr, sar, cmp, test,
 *             jmp, je, jne, jl, jle, jg, jge, jb, ja, etc, call, ret, push, pop, nop, hlt, int,
 *             syscall (int 0x80)
 * - Supports: .section, .global, .text, .data, .rodata, .bss, .asciz, .byte, .word, .long, .quad
 * - Supports: labels, local labels
 * - Does NOT support: AVX/AVX512, complex macro system, .macro (simple only)
 *
 * Example:
 *   qcc -o hello.qtx hello.c
 *   qasm -o hello.qtx hello.s
 *   qcc -shared -o libfoo.qdl foo.c
 */

#ifndef QITO_SDK_QCC_H
#define QITO_SDK_QCC_H

/* No API, just documentation in this header */

#endif
