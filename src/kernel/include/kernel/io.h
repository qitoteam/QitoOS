/*
 * QitoOS - port I/O and low level CPU helpers
 */
#ifndef QITO_IO_H
#define QITO_IO_H

#include <kernel/types.h>

INLINE void outb(uint16_t port, uint8_t value)
{
    __asm__ volatile("outb %0, %1" : : "a"(value), "Nd"(port));
}

INLINE uint8_t inb(uint16_t port)
{
    uint8_t value;
    __asm__ volatile("inb %1, %0" : "=a"(value) : "Nd"(port));
    return value;
}

INLINE void outw(uint16_t port, uint16_t value)
{
    __asm__ volatile("outw %0, %1" : : "a"(value), "Nd"(port));
}

INLINE uint16_t inw(uint16_t port)
{
    uint16_t value;
    __asm__ volatile("inw %1, %0" : "=a"(value) : "Nd"(port));
    return value;
}

INLINE void outl(uint16_t port, uint32_t value)
{
    __asm__ volatile("outl %0, %1" : : "a"(value), "Nd"(port));
}

INLINE uint32_t inl(uint16_t port)
{
    uint32_t value;
    __asm__ volatile("inl %1, %0" : "=a"(value) : "Nd"(port));
    return value;
}

/* A short delay by writing to an unused diagnostic port. */
INLINE void io_wait(void)
{
    outb(0x80, 0);
}

INLINE void cpu_halt(void)
{
    __asm__ volatile("hlt");
}

INLINE void cpu_cli(void)
{
    __asm__ volatile("cli" ::: "memory");
}

INLINE void cpu_sti(void)
{
    __asm__ volatile("sti" ::: "memory");
}

INLINE void cpu_pause(void)
{
    __asm__ volatile("pause" ::: "memory");
}

INLINE uint64_t read_flags(void)
{
    uint64_t flags;
    __asm__ volatile("pushfq; popq %0" : "=r"(flags) :: "memory");
    return flags;
}

/* Disable interrupts and report whether they had been enabled. */
INLINE bool_t irq_save(void)
{
    uint64_t flags = read_flags();
    cpu_cli();
    return (flags & (1u << 9)) != 0;
}

INLINE void irq_restore(bool_t enabled)
{
    if (enabled) {
        cpu_sti();
    }
}

INLINE uint64_t read_cr0(void)
{
    uint64_t v;
    __asm__ volatile("movq %%cr0, %0" : "=r"(v));
    return v;
}

INLINE void write_cr0(uint64_t v)
{
    __asm__ volatile("movq %0, %%cr0" : : "r"(v) : "memory");
}

INLINE uint64_t read_cr2(void)
{
    uint64_t v;
    __asm__ volatile("movq %%cr2, %0" : "=r"(v));
    return v;
}

INLINE uint64_t read_cr3(void)
{
    uint64_t v;
    __asm__ volatile("movq %%cr3, %0" : "=r"(v));
    return v;
}

INLINE void write_cr3(uint64_t v)
{
    __asm__ volatile("movq %0, %%cr3" : : "r"(v) : "memory");
}

INLINE uint64_t read_cr4(void)
{
    uint64_t v;
    __asm__ volatile("movq %%cr4, %0" : "=r"(v));
    return v;
}

INLINE void write_cr4(uint64_t v)
{
    __asm__ volatile("movq %0, %%cr4" : : "r"(v) : "memory");
}

INLINE uint64_t rdmsr(uint32_t msr)
{
    uint32_t lo, hi;
    __asm__ volatile("rdmsr" : "=a"(lo), "=d"(hi) : "c"(msr));
    return ((uint64_t)hi << 32) | lo;
}

INLINE void wrmsr(uint32_t msr, uint64_t value)
{
    __asm__ volatile("wrmsr"
                     :
                     : "c"(msr), "a"((uint32_t)value), "d"((uint32_t)(value >> 32)));
}

INLINE uint64_t rdtsc(void)
{
    uint32_t lo, hi;
    __asm__ volatile("rdtsc" : "=a"(lo), "=d"(hi));
    return ((uint64_t)hi << 32) | lo;
}

INLINE void cpuid_raw(uint32_t leaf, uint32_t subleaf, uint32_t *a, uint32_t *b,
                      uint32_t *c, uint32_t *d)
{
    __asm__ volatile("cpuid"
                     : "=a"(*a), "=b"(*b), "=c"(*c), "=d"(*d)
                     : "a"(leaf), "c"(subleaf));
}

INLINE void invlpg(void *addr)
{
    __asm__ volatile("invlpg (%0)" : : "b"(addr) : "memory");
}

/* Memory-mapped I/O accessors. */
INLINE uint8_t mmio_read8(volatile void *addr)
{
    return *(volatile uint8_t *)addr;
}

INLINE void mmio_write8(volatile void *addr, uint8_t v)
{
    *(volatile uint8_t *)addr = v;
}

INLINE uint16_t mmio_read16(volatile void *addr)
{
    return *(volatile uint16_t *)addr;
}

INLINE void mmio_write16(volatile void *addr, uint16_t v)
{
    *(volatile uint16_t *)addr = v;
}

INLINE uint32_t mmio_read32(volatile void *addr)
{
    return *(volatile uint32_t *)addr;
}

INLINE void mmio_write32(volatile void *addr, uint32_t v)
{
    *(volatile uint32_t *)addr = v;
}

INLINE uint64_t mmio_read64(volatile void *addr)
{
    return *(volatile uint64_t *)addr;
}

INLINE void mmio_write64(volatile void *addr, uint64_t v)
{
    *(volatile uint64_t *)addr = v;
}

#define MSR_EFER          0xC0000080u
#define MSR_STAR          0xC0000081u
#define MSR_LSTAR         0xC0000082u
#define MSR_CSTAR         0xC0000083u
#define MSR_SFMASK        0xC0000084u
#define MSR_FS_BASE       0xC0000100u
#define MSR_GS_BASE       0xC0000101u
#define MSR_KERNEL_GSBASE 0xC0000102u

#endif /* QITO_IO_H */
