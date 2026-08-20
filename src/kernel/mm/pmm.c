/*
 * QitoOS - physical memory manager
 *
 * A bitmap allocator over all usable physical page frames reported by the
 * bootloader's E820 map. One bit per 4 KiB frame: set means allocated.
 *
 * The bitmap itself is placed in the first usable region large enough to hold
 * it, after the kernel image and the ramdisk have been reserved.
 */

#include <kernel/mm.h>
#include <kernel/log.h>
#include <kernel/string.h>
#include <kernel/spinlock.h>

static uint8_t    *frame_bitmap;
static uint64_t    frame_count;      /* frames tracked by the bitmap      */
static uint64_t    bitmap_bytes;
static uint64_t    total_memory;
static uint64_t    usable_memory;
static uint64_t    reserved_memory;
static uint64_t    used_frames;
static uint64_t    last_alloc_hint;
static spinlock_t  pmm_lock;

/* Symbols provided by the linker script. */
extern char __kernel_phys_start[];
extern char __kernel_phys_end[];

static inline void bitmap_set(uint64_t frame)
{
    frame_bitmap[frame / 8] |= (uint8_t)(1u << (frame % 8));
}

static inline void bitmap_clear(uint64_t frame)
{
    frame_bitmap[frame / 8] &= (uint8_t)~(1u << (frame % 8));
}

static inline bool_t bitmap_test(uint64_t frame)
{
    return (frame_bitmap[frame / 8] & (1u << (frame % 8))) != 0;
}

static const char *mem_type_name(uint32_t type)
{
    switch (type) {
    case QITO_MEM_USABLE:    return "usable";
    case QITO_MEM_RESERVED:  return "reserved";
    case QITO_MEM_ACPI_RECL: return "ACPI reclaimable";
    case QITO_MEM_ACPI_NVS:  return "ACPI NVS";
    case QITO_MEM_BAD:       return "bad";
    default:                 return "unknown";
    }
}

void pmm_reserve_region(phys_addr_t base, uint64_t length)
{
    if (length == 0) {
        return;
    }
    uint64_t first = base / PAGE_SIZE;
    uint64_t last  = (base + length + PAGE_SIZE - 1) / PAGE_SIZE;

    for (uint64_t frame = first; frame < last && frame < frame_count; frame++) {
        if (!bitmap_test(frame)) {
            bitmap_set(frame);
            used_frames++;
        }
    }
}

void pmm_init(const struct qito_boot_info *boot)
{
    const struct qito_mmap_entry *map =
        (const struct qito_mmap_entry *)(uintptr_t)boot->mmap_addr;
    uint32_t count = boot->mmap_count;

    spinlock_init(&pmm_lock, "pmm");

    uint64_t highest = 0;
    total_memory     = 0;
    usable_memory    = 0;
    reserved_memory  = 0;

    KLOG_INFO("pmm", "BIOS memory map (%u entries):", count);
    for (uint32_t i = 0; i < count; i++) {
        uint64_t end = map[i].base + map[i].length;

        KLOG_INFO("pmm", "  [%016llx-%016llx] %s",
                  (unsigned long long)map[i].base, (unsigned long long)end,
                  mem_type_name(map[i].type));

        total_memory += map[i].length;
        if (map[i].type == QITO_MEM_USABLE) {
            usable_memory += map[i].length;
            if (end > highest) {
                highest = end;
            }
        } else {
            reserved_memory += map[i].length;
        }
    }

    /*
     * The bootloader only maps the low 4 GiB, so restrict the allocator to
     * memory it can actually address without building more page tables.
     */
    if (highest > 0x100000000ull) {
        highest = 0x100000000ull;
    }

    frame_count  = highest / PAGE_SIZE;
    bitmap_bytes = (frame_count + 7) / 8;

    /* Find somewhere to put the bitmap: a usable region above 1 MiB. */
    uint64_t kernel_start = (uint64_t)(uintptr_t)__kernel_phys_start;
    uint64_t kernel_end   = PAGE_ALIGN_UP((uint64_t)(uintptr_t)__kernel_phys_end);
    uint64_t bitmap_phys  = 0;

    for (uint32_t i = 0; i < count; i++) {
        if (map[i].type != QITO_MEM_USABLE) {
            continue;
        }
        uint64_t base = PAGE_ALIGN_UP(map[i].base);
        uint64_t end  = map[i].base + map[i].length;

        if (base < 0x100000ull) {
            base = 0x100000ull;
        }
        /* Never overlap the kernel image or the ramdisk. */
        if (base < kernel_end && end > kernel_start) {
            base = kernel_end;
        }
        if (boot->ramdisk_size &&
            base < boot->ramdisk_phys + boot->ramdisk_size &&
            end > boot->ramdisk_phys) {
            base = PAGE_ALIGN_UP(boot->ramdisk_phys + boot->ramdisk_size);
        }
        if (base + bitmap_bytes <= end && base + bitmap_bytes <= 0x100000000ull) {
            bitmap_phys = base;
            break;
        }
    }

    if (bitmap_phys == 0) {
        panic("pmm: no room for a %llu byte frame bitmap",
              (unsigned long long)bitmap_bytes);
    }

    frame_bitmap = (uint8_t *)(uintptr_t)bitmap_phys;

    /* Start with everything allocated, then free the usable regions. */
    memset(frame_bitmap, 0xFF, bitmap_bytes);
    used_frames = frame_count;

    for (uint32_t i = 0; i < count; i++) {
        if (map[i].type != QITO_MEM_USABLE) {
            continue;
        }
        uint64_t first = PAGE_ALIGN_UP(map[i].base) / PAGE_SIZE;
        uint64_t last  = (map[i].base + map[i].length) / PAGE_SIZE;

        for (uint64_t frame = first; frame < last && frame < frame_count; frame++) {
            if (bitmap_test(frame)) {
                bitmap_clear(frame);
                used_frames--;
            }
        }
    }

    /* Reserve the pieces that must never be handed out. */
    pmm_reserve_region(0, 0x100000);                       /* real mode + BIOS */
    pmm_reserve_region(kernel_start, kernel_end - kernel_start);
    pmm_reserve_region(bitmap_phys, bitmap_bytes);
    if (boot->ramdisk_size) {
        pmm_reserve_region(boot->ramdisk_phys, boot->ramdisk_size);
    }
    /* Page tables built by the bootloader. */
    pmm_reserve_region(0x20000, 6 * PAGE_SIZE);
    /* Framebuffer, if it lives inside the tracked range. */
    if (boot->fb_valid && boot->fb_addr < highest) {
        pmm_reserve_region(boot->fb_addr, (uint64_t)boot->fb_pitch * boot->fb_height);
    }

    last_alloc_hint = 0x100000 / PAGE_SIZE;

    KLOG_INFO("pmm", "%llu MiB total, %llu MiB usable, %llu frames tracked",
              (unsigned long long)(total_memory / (1024 * 1024)),
              (unsigned long long)(usable_memory / (1024 * 1024)),
              (unsigned long long)frame_count);
    KLOG_INFO("pmm", "bitmap at %p (%llu KiB), %llu frames free",
              (void *)frame_bitmap, (unsigned long long)(bitmap_bytes / 1024),
              (unsigned long long)(frame_count - used_frames));
}

phys_addr_t pmm_alloc_page(void)
{
    return pmm_alloc_pages(1);
}

phys_addr_t pmm_alloc_pages(size_t count)
{
    if (count == 0) {
        return 0;
    }

    bool_t irq = spinlock_acquire(&pmm_lock);

    /* First-fit search starting from the last successful allocation. */
    for (int pass = 0; pass < 2; pass++) {
        uint64_t start = (pass == 0) ? last_alloc_hint : 0x100000 / PAGE_SIZE;
        uint64_t limit = (pass == 0) ? frame_count : last_alloc_hint;

        for (uint64_t frame = start; frame + count <= limit; frame++) {
            if (bitmap_test(frame)) {
                continue;
            }

            uint64_t run = 0;
            while (run < count && !bitmap_test(frame + run)) {
                run++;
            }
            if (run == count) {
                for (uint64_t i = 0; i < count; i++) {
                    bitmap_set(frame + i);
                }
                used_frames += count;
                last_alloc_hint = frame + count;

                phys_addr_t addr = frame * PAGE_SIZE;
                spinlock_release(&pmm_lock, irq);
                memset(phys_to_virt(addr), 0, count * PAGE_SIZE);
                return addr;
            }
            frame += run;   /* skip the run we just rejected */
        }
    }

    spinlock_release(&pmm_lock, irq);
    KLOG_ERR("pmm", "out of physical memory (requested %llu pages)",
             (unsigned long long)count);
    return 0;
}

void pmm_free_page(phys_addr_t addr)
{
    pmm_free_pages(addr, 1);
}

void pmm_free_pages(phys_addr_t addr, size_t count)
{
    if (addr == 0 || count == 0) {
        return;
    }

    uint64_t frame = addr / PAGE_SIZE;
    bool_t   irq   = spinlock_acquire(&pmm_lock);

    for (size_t i = 0; i < count; i++) {
        if (frame + i >= frame_count) {
            break;
        }
        if (bitmap_test(frame + i)) {
            bitmap_clear(frame + i);
            used_frames--;
        }
    }
    if (frame < last_alloc_hint) {
        last_alloc_hint = frame;
    }

    spinlock_release(&pmm_lock, irq);
}

uint64_t pmm_total_bytes(void)
{
    return total_memory;
}

uint64_t pmm_free_bytes(void)
{
    return (frame_count - used_frames) * PAGE_SIZE;
}

uint64_t pmm_used_bytes(void)
{
    return used_frames * PAGE_SIZE;
}

uint64_t pmm_reserved_bytes(void)
{
    return reserved_memory;
}
