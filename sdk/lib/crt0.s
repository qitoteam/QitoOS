    .section .text
    .globl _start
_start:
    call main
    hlt
    jmp _start
