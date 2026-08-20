/*
 * QitoOS - memory management
 */
#ifndef QITO_MM_H
#define QITO_MM_H

#include <kernel/types.h>
#include "../../../boot/bootinfo.h"

#define PAGE_SIZE       4096ull
#define PAGE_SHIFT      12
#define LARGE_PAGE_SIZE (2ull * 1024 * 1024)

/* The bootloader identity maps the low 4 GiB and aliases it at -2 GiB. */
#define KERNEL_VIRT_BASE  0xFFFFFFFF80000000ull
/* Direct physical map window used for arbitrary physical access. */
#define PHYS_MAP_BASE     0x0000000000000000ull

#define PAGE_ALIGN_UP(x)   ALIGN_UP((uint64_t)(x), PAGE_SIZE)
#define PAGE_ALIGN_DOWN(x) ALIGN_DOWN((uint64_t)(x), PAGE_SIZE)

/* Page table entry flags. */
#define PTE_PRESENT   (1ull << 0)
#define PTE_WRITE     (1ull << 1)
#define PTE_USER      (1ull << 2)
#define PTE_PWT       (1ull << 3)
#define PTE_PCD       (1ull << 4)
#define PTE_ACCESSED  (1ull << 5)
#define PTE_DIRTY     (1ull << 6)
#define PTE_HUGE      (1ull << 7)
#define PTE_GLOBAL    (1ull << 8)
#define PTE_NX        (1ull << 63)
#define PTE_ADDR_MASK 0x000FFFFFFFFFF000ull

/* --- physical memory allocator ------------------------------------- */
void        pmm_init(const struct qito_boot_info *boot);
phys_addr_t pmm_alloc_page(void);
phys_addr_t pmm_alloc_pages(size_t count);
void        pmm_free_page(phys_addr_t addr);
void        pmm_free_pages(phys_addr_t addr, size_t count);
void        pmm_reserve_region(phys_addr_t base, uint64_t length);

uint64_t pmm_total_bytes(void);
uint64_t pmm_free_bytes(void);
uint64_t pmm_used_bytes(void);
uint64_t pmm_reserved_bytes(void);

/* Translate between the direct map and physical addresses. */
INLINE void *phys_to_virt(phys_addr_t p)
{
    return (void *)(uintptr_t)p;
}

INLINE phys_addr_t virt_to_phys_direct(void *v)
{
    return (phys_addr_t)(uintptr_t)v;
}

/* --- virtual memory ------------------------------------------------- */
struct address_space {
    phys_addr_t pml4_phys;
    uint64_t   *pml4;
    uint64_t    ref_count;
    uint64_t    mapped_pages;
};

void vmm_init(const struct qito_boot_info *boot);
struct address_space *vmm_kernel_space(void);
struct address_space *vmm_create_space(void);
void  vmm_destroy_space(struct address_space *space);
void  vmm_switch(struct address_space *space);

int   vmm_map(struct address_space *space, virt_addr_t virt, phys_addr_t phys,
              uint64_t flags);
int   vmm_map_range(struct address_space *space, virt_addr_t virt,
                    phys_addr_t phys, size_t size, uint64_t flags);
int   vmm_unmap(struct address_space *space, virt_addr_t virt);
phys_addr_t vmm_resolve(struct address_space *space, virt_addr_t virt);

/* Allocate zeroed, page-aligned anonymous memory into an address space. */
int   vmm_alloc_at(struct address_space *space, virt_addr_t virt, size_t size,
                   uint64_t flags);

int   vmm_handle_page_fault(uint64_t addr, uint64_t error_code);

/* --- kernel heap ---------------------------------------------------- */
void  heap_init(void);
void *kmalloc(size_t size);
void *kzalloc(size_t size);
void *kcalloc(size_t count, size_t size);
void *krealloc(void *ptr, size_t size);
void  kfree(void *ptr);
char *kstrdup(const char *s);

struct heap_stats {
    uint64_t total_bytes;
    uint64_t used_bytes;
    uint64_t free_bytes;
    uint64_t allocations;
    uint64_t frees;
    uint64_t block_count;
    uint64_t largest_free;
};
void heap_get_stats(struct heap_stats *out);
bool_t heap_validate(void);

#endif /* QITO_MM_H */
