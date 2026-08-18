/*
 * Qira OS - boot protocol
 *
 * Structure handed from the Qira bootloader (src/boot) to the kernel.
 * A pointer to a `struct qira_boot_info` is passed in RDI when the kernel
 * entry point is invoked in 64-bit long mode.
 *
 * This header is shared verbatim between the bootloader build and the kernel
 * build, so it must remain free-standing and dependency-light.
 */
#ifndef QIRA_BOOTINFO_H
#define QIRA_BOOTINFO_H

/*
 * This header is included both by hosted tools (which have a C library) and
 * by the freestanding kernel build (which is compiled with -nostdinc), so the
 * fixed-width types are defined locally when <stdint.h> is unavailable.
 */
#if defined(QIRA_KERNEL)
#include <kernel/types.h>
#else
#include <stdint.h>
#endif

#define QIRA_BOOT_MAGIC   0x51424F49u /* 'QBOI' */
#define QIRA_BOOT_VERSION 1u

/* Fixed physical addresses agreed between stage 2 and the kernel. */
#define QIRA_BOOTINFO_PHYS 0x00020000ull
#define QIRA_MMAP_PHYS     0x00021000ull
#define QIRA_KERNEL_PHYS   0x00100000ull
#define QIRA_RAMDISK_PHYS  0x02000000ull

/* Memory map entry types (identical to the BIOS E820 encoding). */
#define QIRA_MEM_USABLE     1u
#define QIRA_MEM_RESERVED   2u
#define QIRA_MEM_ACPI_RECL  3u
#define QIRA_MEM_ACPI_NVS   4u
#define QIRA_MEM_BAD        5u

struct qira_mmap_entry {
    uint64_t base;
    uint64_t length;
    uint32_t type;
    uint32_t attr;
} __attribute__((packed));

struct qira_boot_info {
    uint32_t magic;       /* QIRA_BOOT_MAGIC                              */
    uint32_t version;     /* QIRA_BOOT_VERSION                            */

    /* Linear framebuffer, as programmed through VBE by stage 2. */
    uint64_t fb_addr;
    uint32_t fb_pitch;    /* bytes per scanline                           */
    uint16_t fb_width;
    uint16_t fb_height;
    uint8_t  fb_bpp;
    uint8_t  fb_red_shift;
    uint8_t  fb_red_size;
    uint8_t  fb_green_shift;
    uint8_t  fb_green_size;
    uint8_t  fb_blue_shift;
    uint8_t  fb_blue_size;
    uint8_t  fb_valid;    /* 0 => graphics unavailable, VGA text fallback */

    /* Physical memory map collected via INT 15h/E820. */
    uint32_t mmap_count;
    uint32_t mmap_entry_size;
    uint64_t mmap_addr;

    /* Kernel image placement. */
    uint64_t kernel_phys;
    uint64_t kernel_size;

    /* Initial RAM disk (the QiraFS root image). */
    uint64_t ramdisk_phys;
    uint64_t ramdisk_size;

    uint8_t  boot_drive;
    uint8_t  reserved0[7];

    /* Firmware discovery hints gathered in real mode. */
    uint64_t acpi_rsdp;   /* 0 when not found                             */
    uint64_t ebda_phys;
    uint32_t bios_lowmem_kb;
    uint32_t reserved1;

    char     cmdline[256];
} __attribute__((packed));

#endif /* QIRA_BOOTINFO_H */
