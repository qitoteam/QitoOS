/*
 * QitoOS - interrupt service routine stubs
 *
 * Each of the 256 vectors gets a small stub that normalises the stack into a
 * `struct interrupt_frame` and calls the common C dispatcher. Vectors that the
 * CPU does not push an error code for get a dummy zero so every frame has the
 * same shape.
 *
 * Stack layout on entry to interrupt_dispatch (low address first):
 *
 *      r15 r14 r13 r12 r11 r10 r9 r8
 *      rbp rdi rsi rdx rcx rbx rax
 *      vector
 *      error_code
 *      rip cs rflags rsp ss        <- pushed by the CPU
 */

    .code64
    .section .text

/*
 * For vectors without a hardware error code we push a zero placeholder so
 * every frame has the same shape. Pushing the error code first and the vector
 * second places `vector` at the lower address, matching the C struct.
 */
.macro ISR_NOERR num
    .globl isr_stub_\num
    .align 16
isr_stub_\num:
    pushq   $0                  /* synthetic error code                   */
    pushq   $\num
    jmp     isr_common
.endm

/* The CPU has already pushed the error code for these vectors. */
.macro ISR_ERR num
    .globl isr_stub_\num
    .align 16
isr_stub_\num:
    pushq   $\num
    jmp     isr_common
.endm

/*
 * isr_common - push the general purpose registers and call into C.
 *
 * On entry the stack holds (from rsp upward):
 *      vector, error_code, rip, cs, rflags, rsp, ss
 */
isr_common:
    pushq   %rax
    pushq   %rbx
    pushq   %rcx
    pushq   %rdx
    pushq   %rsi
    pushq   %rdi
    pushq   %rbp
    pushq   %r8
    pushq   %r9
    pushq   %r10
    pushq   %r11
    pushq   %r12
    pushq   %r13
    pushq   %r14
    pushq   %r15

    /* Load the kernel data segment; user mode may have had its own. */
    movw    $0x10, %ax
    movw    %ax, %ds
    movw    %ax, %es

    /* If the interrupt came from user mode, swap in the kernel GS base. */
    movq    (15 * 8 + 16)(%rsp), %rax    /* saved CS                      */
    andq    $3, %rax
    jz      1f
    swapgs
1:
    movq    %rsp, %rdi          /* struct interrupt_frame *              */
    cld
    call    interrupt_dispatch

    /* The dispatcher may return a different frame (context switch). */
    movq    %rax, %rsp

    movq    (15 * 8 + 16)(%rsp), %rax
    andq    $3, %rax
    jz      2f
    swapgs
2:
    popq    %r15
    popq    %r14
    popq    %r13
    popq    %r12
    popq    %r11
    popq    %r10
    popq    %r9
    popq    %r8
    popq    %rbp
    popq    %rdi
    popq    %rsi
    popq    %rdx
    popq    %rcx
    popq    %rbx
    popq    %rax

    addq    $16, %rsp           /* discard vector and error code          */
    iretq

/* ------------------------------------------------------------------ */
/* CPU exceptions 0-31                                                 */
/* Vectors 8, 10-14, 17, 21, 29 and 30 push an error code.             */
/* ------------------------------------------------------------------ */
ISR_NOERR 0
ISR_NOERR 1
ISR_NOERR 2
ISR_NOERR 3
ISR_NOERR 4
ISR_NOERR 5
ISR_NOERR 6
ISR_NOERR 7
ISR_ERR   8
ISR_NOERR 9
ISR_ERR   10
ISR_ERR   11
ISR_ERR   12
ISR_ERR   13
ISR_ERR   14
ISR_NOERR 15
ISR_NOERR 16
ISR_ERR   17
ISR_NOERR 18
ISR_NOERR 19
ISR_NOERR 20
ISR_ERR   21
ISR_NOERR 22
ISR_NOERR 23
ISR_NOERR 24
ISR_NOERR 25
ISR_NOERR 26
ISR_NOERR 27
ISR_NOERR 28
ISR_ERR   29
ISR_ERR   30
ISR_NOERR 31

/* Hardware IRQs and software interrupts 32-255. */
.altmacro
.set vec, 32
.rept 224
    ISR_NOERR %vec
    .set vec, vec + 1
.endr

/* ------------------------------------------------------------------ */
/* Table of stub addresses, consumed by idt_init().                    */
/*                                                                     */
/* `%vec` is only substituted inside macro arguments, so emitting each  */
/* entry goes through a one-line macro.                                 */
/* ------------------------------------------------------------------ */
.macro STUB_ENTRY num
    .quad isr_stub_\num
.endm

    .section .rodata
    .align 16
    .globl isr_stub_table
isr_stub_table:
.set vec, 0
.rept 256
    STUB_ENTRY %vec
    .set vec, vec + 1
.endr

    .section .note.GNU-stack, "", @progbits
