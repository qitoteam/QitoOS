/*
 * QitoOS - CPU state, descriptor tables and interrupt frames
 */
#ifndef QITO_CPU_H
#define QITO_CPU_H

#include <kernel/types.h>

/* Segment selectors laid out by gdt_init(). */
#define SEL_NULL        0x00
#define SEL_KERNEL_CODE 0x08
#define SEL_KERNEL_DATA 0x10
#define SEL_USER_DATA   0x18    /* ring 3, must precede user code for SYSRET */
#define SEL_USER_CODE   0x20
#define SEL_TSS         0x28

#define RING0 0
#define RING3 3

/*
 * Register state pushed by the interrupt stubs. The layout must exactly match
 * the push order in arch/x86_64/isr.S.
 */
struct interrupt_frame {
    /* Pushed by the stub, in reverse order. */
    uint64_t r15, r14, r13, r12, r11, r10, r9, r8;
    uint64_t rbp, rdi, rsi, rdx, rcx, rbx, rax;

    uint64_t vector;
    uint64_t error_code;

    /* Pushed by the CPU. */
    uint64_t rip;
    uint64_t cs;
    uint64_t rflags;
    uint64_t rsp;
    uint64_t ss;
} PACKED;

/* Task state segment; only the stack pointers and IST entries are used. */
struct tss {
    uint32_t reserved0;
    uint64_t rsp0;
    uint64_t rsp1;
    uint64_t rsp2;
    uint64_t reserved1;
    uint64_t ist[7];
    uint64_t reserved2;
    uint16_t reserved3;
    uint16_t iomap_base;
} PACKED;

struct cpu_info {
    char     vendor[13];
    char     brand[49];
    uint32_t family;
    uint32_t model;
    uint32_t stepping;
    uint32_t max_leaf;
    uint32_t max_ext_leaf;
    uint64_t features_edx;   /* CPUID.1:EDX */
    uint64_t features_ecx;   /* CPUID.1:ECX */
    uint64_t features_ext;   /* CPUID.80000001:EDX */
    bool_t   has_apic;
    bool_t   has_sse;
    bool_t   has_sse2;
    bool_t   has_nx;
    bool_t   has_long_mode;
    bool_t   has_tsc;
    bool_t   has_msr;
    uint32_t phys_addr_bits;
    uint32_t virt_addr_bits;
};

void gdt_init(void);
void tss_set_kernel_stack(uint64_t rsp0);

void idt_init(void);
void cpu_detect(void);
const struct cpu_info *cpu_get_info(void);

/* Dump register state; used by the fault handlers and the debugger. */
void cpu_dump_frame(const struct interrupt_frame *frame);

#endif /* QITO_CPU_H */
