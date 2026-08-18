/*
 * Qira OS - virtual memory manager
 *
 * Manages 4-level x86_64 page tables. The kernel half (entries 256-511 of the
 * PML4) is shared by every address space, so a new user process only needs its
 * own lower half.
 */

#include <kernel/mm.h>
#include <kernel/log.h>
#include <kernel/string.h>
#include <kernel/io.h>
#include <kernel/spinlock.h>

#define PML4_INDEX(v) (((v) >> 39) & 0x1FF)
#define PDPT_INDEX(v) (((v) >> 30) & 0x1FF)
#define PD_INDEX(v)   (((v) >> 21) & 0x1FF)
#define PT_INDEX(v)   (((v) >> 12) & 0x1FF)

static struct address_space kernel_space;
static spinlock_t           vmm_lock;

/* Currently active address space, tracked to avoid redundant CR3 writes. */
static struct address_space *active_space;

/*
 * Walk to the page table entry for `virt`, optionally allocating any missing
 * intermediate tables.
 */
static uint64_t *walk(struct address_space *space, virt_addr_t virt, bool_t create,
                      uint64_t parent_flags)
{
    uint64_t *table = space->pml4;
    uint64_t  indices[4] = {
        PML4_INDEX(virt),
        PDPT_INDEX(virt),
        PD_INDEX(virt),
        PT_INDEX(virt),
    };

    for (int level = 0; level < 3; level++) {
        uint64_t entry = table[indices[level]];

        if (!(entry & PTE_PRESENT)) {
            if (!create) {
                return NULL;
            }
            phys_addr_t page = pmm_alloc_page();
            if (!page) {
                return NULL;
            }
            memset(phys_to_virt(page), 0, PAGE_SIZE);
            entry = page | PTE_PRESENT | PTE_WRITE | (parent_flags & PTE_USER);
            table[indices[level]] = entry;
        } else if (entry & PTE_HUGE) {
            /*
             * A large page already covers this address. Splitting it is not
             * supported; callers must not map inside the bootloader's 2 MiB
             * identity mappings.
             */
            return NULL;
        } else if ((parent_flags & PTE_USER) && !(entry & PTE_USER)) {
            /* Widen permissions so user mappings remain reachable. */
            table[indices[level]] = entry | PTE_USER;
        }

        table = (uint64_t *)phys_to_virt(entry & PTE_ADDR_MASK);
    }

    return &table[indices[3]];
}

void vmm_init(const struct qira_boot_info *boot)
{
    UNUSED(boot);
    spinlock_init(&vmm_lock, "vmm");

    /*
     * Adopt the page tables the bootloader created rather than rebuilding
     * them: they already identity map the low 4 GiB with 2 MiB pages and
     * alias the kernel at -2 GiB.
     */
    kernel_space.pml4_phys    = read_cr3() & PTE_ADDR_MASK;
    kernel_space.pml4         = (uint64_t *)phys_to_virt(kernel_space.pml4_phys);
    kernel_space.ref_count    = 1;
    kernel_space.mapped_pages = 0;
    active_space              = &kernel_space;

    /*
     * Pre-allocate every kernel-half PML4 entry so that address spaces created
     * later can share them by simply copying the entries.
     */
    for (int i = 256; i < 512; i++) {
        if (!(kernel_space.pml4[i] & PTE_PRESENT)) {
            phys_addr_t page = pmm_alloc_page();
            if (!page) {
                panic("vmm: cannot allocate kernel PML4 entry %d", i);
            }
            memset(phys_to_virt(page), 0, PAGE_SIZE);
            kernel_space.pml4[i] = page | PTE_PRESENT | PTE_WRITE;
        }
    }

    KLOG_INFO("vmm", "kernel address space at CR3=%llx",
              (unsigned long long)kernel_space.pml4_phys);
}

struct address_space *vmm_kernel_space(void)
{
    return &kernel_space;
}

struct address_space *vmm_create_space(void)
{
    struct address_space *space = kzalloc(sizeof(struct address_space));
    if (!space) {
        return NULL;
    }

    phys_addr_t pml4 = pmm_alloc_page();
    if (!pml4) {
        kfree(space);
        return NULL;
    }

    space->pml4_phys = pml4;
    space->pml4      = (uint64_t *)phys_to_virt(pml4);
    space->ref_count = 1;

    memset(space->pml4, 0, PAGE_SIZE);

    /* Share the kernel half so syscalls and interrupts always work. */
    for (int i = 256; i < 512; i++) {
        space->pml4[i] = kernel_space.pml4[i];
    }

    return space;
}

/* Recursively release the tables (and their frames) below `table`. */
static void free_table(uint64_t *table, int level)
{
    if (level == 0) {
        return;
    }
    for (int i = 0; i < 512; i++) {
        uint64_t entry = table[i];
        if (!(entry & PTE_PRESENT) || (entry & PTE_HUGE)) {
            continue;
        }
        phys_addr_t child = entry & PTE_ADDR_MASK;
        if (level > 1) {
            free_table((uint64_t *)phys_to_virt(child), level - 1);
        }
        pmm_free_page(child);
    }
}

void vmm_destroy_space(struct address_space *space)
{
    if (!space || space == &kernel_space) {
        return;
    }
    if (--space->ref_count > 0) {
        return;
    }

    /* Only the user half is owned by this address space. */
    for (int i = 0; i < 256; i++) {
        uint64_t entry = space->pml4[i];
        if (!(entry & PTE_PRESENT)) {
            continue;
        }
        phys_addr_t pdpt = entry & PTE_ADDR_MASK;
        free_table((uint64_t *)phys_to_virt(pdpt), 3);
        pmm_free_page(pdpt);
    }

    pmm_free_page(space->pml4_phys);
    kfree(space);
}

void vmm_switch(struct address_space *space)
{
    if (!space || space == active_space) {
        return;
    }
    active_space = space;
    write_cr3(space->pml4_phys);
}

int vmm_map(struct address_space *space, virt_addr_t virt, phys_addr_t phys,
            uint64_t flags)
{
    if (!space) {
        space = &kernel_space;
    }

    bool_t    irq = spinlock_acquire(&vmm_lock);
    uint64_t *pte = walk(space, PAGE_ALIGN_DOWN(virt), true, flags);

    if (!pte) {
        spinlock_release(&vmm_lock, irq);
        return -1;
    }

    *pte = (phys & PTE_ADDR_MASK) | (flags & ~PTE_ADDR_MASK) | PTE_PRESENT;
    space->mapped_pages++;

    spinlock_release(&vmm_lock, irq);

    if (space == active_space || space == &kernel_space) {
        invlpg((void *)(uintptr_t)virt);
    }
    return 0;
}

int vmm_map_range(struct address_space *space, virt_addr_t virt, phys_addr_t phys,
                  size_t size, uint64_t flags)
{
    size = PAGE_ALIGN_UP(size);
    for (size_t offset = 0; offset < size; offset += PAGE_SIZE) {
        if (vmm_map(space, virt + offset, phys + offset, flags) != 0) {
            return -1;
        }
    }
    return 0;
}

int vmm_unmap(struct address_space *space, virt_addr_t virt)
{
    if (!space) {
        space = &kernel_space;
    }

    bool_t    irq = spinlock_acquire(&vmm_lock);
    uint64_t *pte = walk(space, PAGE_ALIGN_DOWN(virt), false, 0);

    if (!pte || !(*pte & PTE_PRESENT)) {
        spinlock_release(&vmm_lock, irq);
        return -1;
    }

    *pte = 0;
    if (space->mapped_pages) {
        space->mapped_pages--;
    }
    spinlock_release(&vmm_lock, irq);

    if (space == active_space || space == &kernel_space) {
        invlpg((void *)(uintptr_t)virt);
    }
    return 0;
}

phys_addr_t vmm_resolve(struct address_space *space, virt_addr_t virt)
{
    if (!space) {
        space = &kernel_space;
    }

    uint64_t *table = space->pml4;
    uint64_t  idx[4] = {PML4_INDEX(virt), PDPT_INDEX(virt), PD_INDEX(virt),
                        PT_INDEX(virt)};

    for (int level = 0; level < 3; level++) {
        uint64_t entry = table[idx[level]];
        if (!(entry & PTE_PRESENT)) {
            return 0;
        }
        if (entry & PTE_HUGE) {
            /* 1 GiB page at the PDPT level, 2 MiB at the PD level. */
            uint64_t page_shift = (level == 1) ? 30 : 21;
            uint64_t mask       = (1ull << page_shift) - 1;
            return (entry & PTE_ADDR_MASK & ~mask) | (virt & mask);
        }
        table = (uint64_t *)phys_to_virt(entry & PTE_ADDR_MASK);
    }

    uint64_t entry = table[idx[3]];
    if (!(entry & PTE_PRESENT)) {
        return 0;
    }
    return (entry & PTE_ADDR_MASK) | (virt & (PAGE_SIZE - 1));
}

int vmm_alloc_at(struct address_space *space, virt_addr_t virt, size_t size,
                 uint64_t flags)
{
    virt_addr_t start = PAGE_ALIGN_DOWN(virt);
    virt_addr_t end   = PAGE_ALIGN_UP(virt + size);

    for (virt_addr_t page = start; page < end; page += PAGE_SIZE) {
        if (vmm_resolve(space, page)) {
            continue;   /* already mapped */
        }
        phys_addr_t frame = pmm_alloc_page();
        if (!frame) {
            return -1;
        }
        if (vmm_map(space, page, frame, flags | PTE_PRESENT) != 0) {
            pmm_free_page(frame);
            return -1;
        }
    }
    return 0;
}

/*
 * Page fault handler hook.
 *
 * Qira does not implement demand paging or swap yet, so this only reports
 * whether the fault could be resolved. Returning non-zero makes the fault
 * fatal (or fatal to the faulting task).
 */
int vmm_handle_page_fault(uint64_t addr, uint64_t error_code)
{
    UNUSED(addr);
    UNUSED(error_code);
    return -1;
}
