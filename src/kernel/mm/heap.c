/*
 * QitoOS - kernel heap
 *
 * A first-fit free-list allocator over a contiguous virtual region backed by
 * physical frames. Blocks carry a small header with a magic value so that
 * double frees and buffer overruns are detected rather than silently
 * corrupting the heap.
 *
 * Adjacent free blocks are coalesced on free, and the heap grows on demand up
 * to HEAP_MAX_SIZE.
 */

#include <kernel/mm.h>
#include <kernel/log.h>
#include <kernel/string.h>
#include <kernel/spinlock.h>

#define HEAP_BASE       0xFFFFC00000000000ull
#define HEAP_INIT_SIZE  (4 * 1024 * 1024)
#define HEAP_MAX_SIZE   (256 * 1024 * 1024)
#define HEAP_GROW_STEP  (1 * 1024 * 1024)

#define BLOCK_MAGIC_USED 0x5153454455534544ull  /* "QSEDUSED" */
#define BLOCK_MAGIC_FREE 0x5153454446524545ull  /* "QSEDFREE" */

#define MIN_SPLIT_SIZE   64
#define HEAP_ALIGN       16

struct block {
    uint64_t      magic;
    size_t        size;      /* payload bytes, excluding this header */
    struct block *next;
    struct block *prev;
    bool_t        free;
    uint8_t       padding[7];
};

static struct block *heap_head;
static uint64_t      heap_start;
static uint64_t      heap_end;      /* one past the last mapped byte */
static uint64_t      heap_committed;
static spinlock_t    heap_lock;
static struct heap_stats stats;

static bool_t heap_grow(size_t needed);

void heap_init(void)
{
    spinlock_init(&heap_lock, "heap");

    heap_start     = HEAP_BASE;
    heap_end       = HEAP_BASE;
    heap_committed = 0;
    heap_head      = NULL;
    memset(&stats, 0, sizeof(stats));

    if (!heap_grow(HEAP_INIT_SIZE)) {
        panic("heap: unable to map the initial %d MiB",
              HEAP_INIT_SIZE / (1024 * 1024));
    }

    KLOG_INFO("heap", "kernel heap at %llx, %llu KiB committed",
              (unsigned long long)heap_start,
              (unsigned long long)(heap_committed / 1024));
}

/*
 * Map more physical memory at the end of the heap and append it as a free
 * block. Called with the heap lock held (or during init).
 */
static bool_t heap_grow(size_t needed)
{
    size_t amount = ALIGN_UP(needed + sizeof(struct block), HEAP_GROW_STEP);

    if (heap_committed + amount > HEAP_MAX_SIZE) {
        amount = HEAP_MAX_SIZE - heap_committed;
    }
    if (amount < needed + sizeof(struct block)) {
        return false;
    }

    for (uint64_t offset = 0; offset < amount; offset += PAGE_SIZE) {
        phys_addr_t frame = pmm_alloc_page();
        if (!frame) {
            /* Partial growth is still useful if we got anything at all. */
            if (offset == 0) {
                return false;
            }
            amount = offset;
            break;
        }
        if (vmm_map(vmm_kernel_space(), heap_end + offset, frame,
                    PTE_PRESENT | PTE_WRITE | PTE_NX) != 0) {
            pmm_free_page(frame);
            if (offset == 0) {
                return false;
            }
            amount = offset;
            break;
        }
    }

    struct block *block = (struct block *)(uintptr_t)heap_end;
    block->magic = BLOCK_MAGIC_FREE;
    block->size  = amount - sizeof(struct block);
    block->free  = true;
    block->next  = NULL;
    block->prev  = NULL;

    if (!heap_head) {
        heap_head = block;
    } else {
        struct block *tail = heap_head;
        while (tail->next) {
            tail = tail->next;
        }
        tail->next  = block;
        block->prev = tail;

        /* Merge with the previous block if it is free and adjacent. */
        if (tail->free &&
            (uintptr_t)tail + sizeof(struct block) + tail->size == (uintptr_t)block) {
            tail->size += sizeof(struct block) + block->size;
            tail->next = NULL;
        }
    }

    heap_end       += amount;
    heap_committed += amount;
    stats.total_bytes = heap_committed;
    return true;
}

static void split_block(struct block *block, size_t size)
{
    if (block->size < size + sizeof(struct block) + MIN_SPLIT_SIZE) {
        return;
    }

    struct block *rest =
        (struct block *)((uint8_t *)block + sizeof(struct block) + size);

    rest->magic = BLOCK_MAGIC_FREE;
    rest->size  = block->size - size - sizeof(struct block);
    rest->free  = true;
    rest->next  = block->next;
    rest->prev  = block;

    if (block->next) {
        block->next->prev = rest;
    }
    block->next = rest;
    block->size = size;
}

static void coalesce(struct block *block)
{
    /* Merge forward. */
    while (block->next && block->next->free &&
           (uintptr_t)block + sizeof(struct block) + block->size ==
               (uintptr_t)block->next) {
        struct block *next = block->next;
        block->size += sizeof(struct block) + next->size;
        block->next = next->next;
        if (next->next) {
            next->next->prev = block;
        }
        next->magic = 0;
    }

    /* Merge backward. */
    if (block->prev && block->prev->free &&
        (uintptr_t)block->prev + sizeof(struct block) + block->prev->size ==
            (uintptr_t)block) {
        struct block *prev = block->prev;
        prev->size += sizeof(struct block) + block->size;
        prev->next = block->next;
        if (block->next) {
            block->next->prev = prev;
        }
        block->magic = 0;
    }
}

void *kmalloc(size_t size)
{
    if (size == 0) {
        return NULL;
    }
    size = ALIGN_UP(size, HEAP_ALIGN);

    bool_t irq = spinlock_acquire(&heap_lock);

    for (int attempt = 0; attempt < 2; attempt++) {
        for (struct block *block = heap_head; block; block = block->next) {
            if (!block->free || block->size < size) {
                continue;
            }
            split_block(block, size);
            block->free  = false;
            block->magic = BLOCK_MAGIC_USED;

            stats.used_bytes += block->size;
            stats.allocations++;

            spinlock_release(&heap_lock, irq);
            return (uint8_t *)block + sizeof(struct block);
        }

        if (attempt == 0 && !heap_grow(size)) {
            break;
        }
    }

    spinlock_release(&heap_lock, irq);
    KLOG_ERR("heap", "allocation of %llu bytes failed", (unsigned long long)size);
    return NULL;
}

void *kzalloc(size_t size)
{
    void *ptr = kmalloc(size);
    if (ptr) {
        memset(ptr, 0, size);
    }
    return ptr;
}

void *kcalloc(size_t count, size_t size)
{
    /* Reject multiplications that would overflow. */
    if (count && size > SIZE_MAX / count) {
        return NULL;
    }
    return kzalloc(count * size);
}

void kfree(void *ptr)
{
    if (!ptr) {
        return;
    }

    struct block *block = (struct block *)((uint8_t *)ptr - sizeof(struct block));

    if (block->magic == BLOCK_MAGIC_FREE) {
        KLOG_ERR("heap", "double free of %p", ptr);
        return;
    }
    if (block->magic != BLOCK_MAGIC_USED) {
        KLOG_ERR("heap", "kfree(%p): bad block magic %llx (heap corruption?)", ptr,
                 (unsigned long long)block->magic);
        return;
    }

    bool_t irq = spinlock_acquire(&heap_lock);

    block->free  = true;
    block->magic = BLOCK_MAGIC_FREE;
    if (stats.used_bytes >= block->size) {
        stats.used_bytes -= block->size;
    }
    stats.frees++;
    coalesce(block);

    spinlock_release(&heap_lock, irq);
}

void *krealloc(void *ptr, size_t size)
{
    if (!ptr) {
        return kmalloc(size);
    }
    if (size == 0) {
        kfree(ptr);
        return NULL;
    }

    struct block *block = (struct block *)((uint8_t *)ptr - sizeof(struct block));
    if (block->magic != BLOCK_MAGIC_USED) {
        KLOG_ERR("heap", "krealloc(%p): bad block magic", ptr);
        return NULL;
    }
    if (block->size >= size) {
        return ptr;
    }

    void *fresh = kmalloc(size);
    if (!fresh) {
        return NULL;
    }
    memcpy(fresh, ptr, block->size);
    kfree(ptr);
    return fresh;
}

char *kstrdup(const char *s)
{
    if (!s) {
        return NULL;
    }
    size_t len  = strlen(s) + 1;
    char  *copy = kmalloc(len);
    if (copy) {
        memcpy(copy, s, len);
    }
    return copy;
}

void heap_get_stats(struct heap_stats *out)
{
    if (!out) {
        return;
    }

    bool_t irq = spinlock_acquire(&heap_lock);

    *out = stats;
    out->total_bytes  = heap_committed;
    out->block_count  = 0;
    out->free_bytes   = 0;
    out->largest_free = 0;

    for (struct block *block = heap_head; block; block = block->next) {
        out->block_count++;
        if (block->free) {
            out->free_bytes += block->size;
            if (block->size > out->largest_free) {
                out->largest_free = block->size;
            }
        }
    }

    spinlock_release(&heap_lock, irq);
}

bool_t heap_validate(void)
{
    bool_t irq   = spinlock_acquire(&heap_lock);
    bool_t valid = true;

    for (struct block *block = heap_head; block; block = block->next) {
        if (block->magic != BLOCK_MAGIC_USED && block->magic != BLOCK_MAGIC_FREE) {
            KLOG_ERR("heap", "corrupt block header at %p", (void *)block);
            valid = false;
            break;
        }
        if (block->next && block->next->prev != block) {
            KLOG_ERR("heap", "broken free list link at %p", (void *)block);
            valid = false;
            break;
        }
        if ((uintptr_t)block < heap_start || (uintptr_t)block >= heap_end) {
            KLOG_ERR("heap", "block %p outside the heap", (void *)block);
            valid = false;
            break;
        }
    }

    spinlock_release(&heap_lock, irq);
    return valid;
}
