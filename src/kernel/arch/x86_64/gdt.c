/*
 * QitoOS - Global Descriptor Table and Task State Segment
 *
 * Long mode barely uses segmentation, but a GDT is still required for the
 * privilege-level switch between ring 0 and ring 3, and a TSS is needed so the
 * CPU knows which stack to load on an interrupt taken from user mode.
 *
 * Selector order matters: SYSRET requires the user data descriptor to sit
 * immediately before the user code descriptor.
 */

#include <kernel/cpu.h>
#include <kernel/log.h>
#include <kernel/string.h>

struct gdt_entry {
    uint16_t limit_low;
    uint16_t base_low;
    uint8_t  base_mid;
    uint8_t  access;
    uint8_t  granularity;
    uint8_t  base_high;
} PACKED;

/* A TSS descriptor occupies two consecutive GDT slots in long mode. */
struct tss_descriptor {
    uint16_t limit_low;
    uint16_t base_low;
    uint8_t  base_mid;
    uint8_t  access;
    uint8_t  granularity;
    uint8_t  base_high;
    uint32_t base_upper;
    uint32_t reserved;
} PACKED;

struct gdt_pointer {
    uint16_t limit;
    uint64_t base;
} PACKED;

#define GDT_ENTRIES 7   /* null, kcode, kdata, udata, ucode, tss (x2) */

static struct gdt_entry   gdt[GDT_ENTRIES] ALIGNED(16);
static struct tss         kernel_tss ALIGNED(16);
static struct gdt_pointer gdt_ptr;

/* Dedicated stacks for faults that cannot trust the interrupted stack. */
static uint8_t ist_stack_df[8192] ALIGNED(16);   /* #DF double fault      */
static uint8_t ist_stack_nmi[8192] ALIGNED(16);  /* NMI                   */
static uint8_t ist_stack_pf[8192] ALIGNED(16);   /* #PF page fault        */

/* Access byte bits. */
#define ACC_PRESENT   0x80
#define ACC_RING0     0x00
#define ACC_RING3     0x60
#define ACC_SEGMENT   0x10
#define ACC_EXEC      0x08
#define ACC_RW        0x02
#define ACC_ACCESSED  0x01
#define ACC_TSS_AVAIL 0x09

/* Granularity byte bits. */
#define GRAN_LONG     0x20   /* 64-bit code segment                        */
#define GRAN_DB       0x40   /* 32-bit default operand size                */
#define GRAN_4K       0x80

static void gdt_set(int index, uint32_t base, uint32_t limit, uint8_t access,
                    uint8_t granularity)
{
    gdt[index].limit_low   = (uint16_t)(limit & 0xFFFF);
    gdt[index].base_low    = (uint16_t)(base & 0xFFFF);
    gdt[index].base_mid    = (uint8_t)((base >> 16) & 0xFF);
    gdt[index].access      = access;
    gdt[index].granularity = (uint8_t)(((limit >> 16) & 0x0F) | granularity);
    gdt[index].base_high   = (uint8_t)((base >> 24) & 0xFF);
}

static void gdt_set_tss(int index, uint64_t base, uint32_t limit)
{
    struct tss_descriptor *d = (struct tss_descriptor *)&gdt[index];

    d->limit_low   = (uint16_t)(limit & 0xFFFF);
    d->base_low    = (uint16_t)(base & 0xFFFF);
    d->base_mid    = (uint8_t)((base >> 16) & 0xFF);
    d->access      = ACC_PRESENT | ACC_TSS_AVAIL;
    d->granularity = (uint8_t)((limit >> 16) & 0x0F);
    d->base_high   = (uint8_t)((base >> 24) & 0xFF);
    d->base_upper  = (uint32_t)(base >> 32);
    d->reserved    = 0;
}

void tss_set_kernel_stack(uint64_t rsp0)
{
    kernel_tss.rsp0 = rsp0;
}

void gdt_init(void)
{
    memset(gdt, 0, sizeof(gdt));
    memset(&kernel_tss, 0, sizeof(kernel_tss));

    /* 0x00 null descriptor is already zeroed. */

    /* 0x08 kernel code: present, ring 0, executable, 64-bit. */
    gdt_set(1, 0, 0xFFFFF,
            ACC_PRESENT | ACC_RING0 | ACC_SEGMENT | ACC_EXEC | ACC_RW,
            GRAN_LONG | GRAN_4K);

    /* 0x10 kernel data. */
    gdt_set(2, 0, 0xFFFFF, ACC_PRESENT | ACC_RING0 | ACC_SEGMENT | ACC_RW,
            GRAN_DB | GRAN_4K);

    /* 0x18 user data (must come first for SYSRET). */
    gdt_set(3, 0, 0xFFFFF, ACC_PRESENT | ACC_RING3 | ACC_SEGMENT | ACC_RW,
            GRAN_DB | GRAN_4K);

    /* 0x20 user code, 64-bit. */
    gdt_set(4, 0, 0xFFFFF,
            ACC_PRESENT | ACC_RING3 | ACC_SEGMENT | ACC_EXEC | ACC_RW,
            GRAN_LONG | GRAN_4K);

    /* 0x28 TSS (two slots). */
    kernel_tss.iomap_base = sizeof(struct tss);
    kernel_tss.ist[0]     = (uint64_t)(ist_stack_df + sizeof(ist_stack_df));
    kernel_tss.ist[1]     = (uint64_t)(ist_stack_nmi + sizeof(ist_stack_nmi));
    kernel_tss.ist[2]     = (uint64_t)(ist_stack_pf + sizeof(ist_stack_pf));
    gdt_set_tss(5, (uint64_t)&kernel_tss, sizeof(struct tss) - 1);

    gdt_ptr.limit = sizeof(gdt) - 1;
    gdt_ptr.base  = (uint64_t)gdt;

    __asm__ volatile("lgdt %0" : : "m"(gdt_ptr) : "memory");

    /*
     * Reload the segment registers. CS can only be changed with a far
     * transfer, so a far return is used to jump to the next instruction with
     * the new selector.
     */
    __asm__ volatile(
        "pushq %[code]\n"
        "leaq 1f(%%rip), %%rax\n"
        "pushq %%rax\n"
        "lretq\n"
        "1:\n"
        "movw %[data], %%ax\n"
        "movw %%ax, %%ds\n"
        "movw %%ax, %%es\n"
        "movw %%ax, %%fs\n"
        "movw %%ax, %%gs\n"
        "movw %%ax, %%ss\n"
        :
        : [code] "i"((uint64_t)SEL_KERNEL_CODE), [data] "i"((uint16_t)SEL_KERNEL_DATA)
        : "rax", "memory");

    __asm__ volatile("ltr %%ax" : : "a"((uint16_t)SEL_TSS));

    KLOG_INFO("gdt", "descriptor tables installed (%d entries, TSS at %p)",
              GDT_ENTRIES, (void *)&kernel_tss);
}
