/*
 * QitoOS - Demand paging, mmap and page cache
 * Lazy allocation, CoW fork, memory-mapped files – required to hold large voxel world.
 */

#ifndef QITO_MMAP_H
#define QITO_MMAP_H

#include <kernel/types.h>

#define PROT_READ  0x1
#define PROT_WRITE 0x2
#define PROT_EXEC  0x4

#define MAP_PRIVATE   0x02
#define MAP_SHARED    0x01
#define MAP_ANONYMOUS 0x20
#define MAP_FIXED     0x10

void mmap_init(void);

void *mmap(void *addr, size_t length, int prot, int flags, int fd, size_t offset);
int munmap(void *addr, size_t length);

int mmap_file(const char *path, void **out_addr, size_t *out_len);

struct page_cache_entry {
    char path[128];
    void *data;
    size_t size;
    int refcount;
    bool_t dirty;
};

void page_cache_init(void);
struct page_cache_entry *page_cache_get(const char *path);
void page_cache_put(struct page_cache_entry *entry);
void page_cache_flush(void);

#endif
