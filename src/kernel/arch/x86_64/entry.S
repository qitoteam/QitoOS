/*
 * QitoOS - 64-bit kernel entry point
 *
 * The bootloader enters here in long mode with:
 *   - paging enabled, low 4 GiB identity mapped and aliased at -2 GiB
 *   - a flat GDT loaded (CS = 0x08, data = 0x10)
 *   - interrupts disabled
 *   - RDI = physical address of struct qito_boot_info
 *
 * This stub installs the kernel stack, clears BSS, and calls kmain().
 */

    .code64
    .section .text.entry, "ax"
    .globl _kernel_entry
    .type _kernel_entry, @function

_kernel_entry:
    cli
    cld

    /* Install the kernel stack (defined below, in BSS). */
    leaq    kernel_stack_top(%rip), %rsp
    andq    $~15, %rsp
    xorq    %rbp, %rbp

    /* Preserve the boot info pointer across the BSS wipe. */
    movq    %rdi, %r15

    /* Zero .bss: the loader only copies file-backed contents. */
    leaq    __bss_start(%rip), %rdi
    leaq    __bss_end(%rip), %rcx
    subq    %rdi, %rcx
    shrq    $3, %rcx
    xorq    %rax, %rax
    rep     stosq

    /* Enable SSE so the compiler may use xmm registers for memory ops. */
    call    enable_sse

    /* kmain(struct qito_boot_info *) */
    movq    %r15, %rdi
    call    kmain

    /* kmain must not return. */
1:  cli
    hlt
    jmp     1b
    .size _kernel_entry, . - _kernel_entry

/*
 * enable_sse - clear CR0.EM, set CR0.MP, and enable OSFXSR/OSXMMEXCPT.
 */
    .type enable_sse, @function
enable_sse:
    movq    %cr0, %rax
    andq    $~(1 << 2), %rax        /* clear EM (no FPU emulation)        */
    orq     $(1 << 1), %rax         /* set MP                             */
    movq    %rax, %cr0

    movq    %cr4, %rax
    orq     $((1 << 9) | (1 << 10)), %rax   /* OSFXSR | OSXMMEXCPT        */
    movq    %rax, %cr4

    fninit
    ret
    .size enable_sse, . - enable_sse

/* ------------------------------------------------------------------ */
    .section .bss, "aw", @nobits
    .align 4096
    .globl kernel_stack_bottom
kernel_stack_bottom:
    .space 65536                    /* 64 KiB boot stack                  */
    .globl kernel_stack_top
kernel_stack_top:

    .section .note.GNU-stack, "", @progbits
