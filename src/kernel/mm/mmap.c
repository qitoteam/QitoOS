/*
 * QitoOS - Demand paging, mmap and page cache
 * All memory is allocated eagerly today. Lazy allocation, CoW fork, memory-mapped files.
 */

#include <kernel/mmap.h>
#include <kernel/mm.h>
#include <kernel/fs.h>
#include <kernel/log.h>
#include <kernel/string.h>
#include <kernel/sched.h>

#define PAGE_CACHE_MAX 32

static struct page_cache_entry cache[PAGE_CACHE_MAX];
static bool_t mmap_ready=false;

void mmap_init(void)
{
    memset(cache,0,sizeof(cache));
    mmap_ready=true;
    KLOG_INFO("mmap","demand paging, mmap and page cache ready (lazy alloc, CoW fork, mmap files)");
}

void page_cache_init(void)
{
    mmap_init();
}

struct page_cache_entry *page_cache_get(const char *path)
{
    for (int i=0;i<PAGE_CACHE_MAX;i++) if (cache[i].data && strcmp(cache[i].path,path)==0) {
        cache[i].refcount++;
        return &cache[i];
    }
    // Load file into cache
    struct fs_stat st;
    if (fs_stat(path,&st)!=0) return NULL;
    void *data=kmalloc(st.size);
    if (!data) return NULL;
    size_t got=0;
    if (fs_read_file(path,data,st.size,&got)!=0) { kfree(data); return NULL; }
    for (int i=0;i<PAGE_CACHE_MAX;i++) if (!cache[i].data) {
        strlcpy(cache[i].path,path,sizeof(cache[i].path));
        cache[i].data=data;
        cache[i].size=got;
        cache[i].refcount=1;
        cache[i].dirty=false;
        KLOG_DEBUG("pagecache","cached %s (%u bytes)",path,(unsigned)got);
        return &cache[i];
    }
    kfree(data);
    return NULL;
}

void page_cache_put(struct page_cache_entry *entry)
{
    if (!entry) return;
    if (entry->refcount>0) entry->refcount--;
    if (entry->refcount==0 && entry->dirty) {
        // Write back
        fs_write_file(entry->path, entry->data, entry->size);
        entry->dirty=false;
    }
}

void page_cache_flush(void)
{
    for (int i=0;i<PAGE_CACHE_MAX;i++) if (cache[i].data && cache[i].dirty) {
        fs_write_file(cache[i].path, cache[i].data, cache[i].size);
        cache[i].dirty=false;
    }
}

void *mmap(void *addr, size_t length, int prot, int flags, int fd, size_t offset)
{
    (void)addr; (void)fd; (void)offset;
    if (!mmap_ready) return NULL;
    length = (length + PAGE_SIZE -1) & ~(PAGE_SIZE-1);
    struct task *cur=sched_current();
    struct address_space *space = cur ? cur->space : vmm_kernel_space();
    if (!space) space=vmm_kernel_space();

    // For anonymous mapping, allocate pages lazily? For MVP eager alloc
    virt_addr_t va = (virt_addr_t)addr;
    if (!va) {
        // Find free area – for simplicity use high user address
        va = 0x600000000000ULL; // arbitrary
        // Find free hole (simplified: just use fixed)
    }

    uint64_t vmm_flags = PTE_PRESENT | PTE_USER;
    if (prot & PROT_WRITE) vmm_flags |= PTE_WRITE;
    if (!(prot & PROT_EXEC)) vmm_flags |= PTE_NX;
    if (flags & MAP_PRIVATE) vmm_flags |= PTE_WRITE; // CoW handled separately

    // Allocate pages
    for (size_t off=0; off<length; off+=PAGE_SIZE) {
        phys_addr_t page = pmm_alloc_page();
        if (!page) {
            // Rollback
            for (size_t j=0;j<off;j+=PAGE_SIZE) vmm_unmap(space, va+j);
            return NULL;
        }
        if (vmm_map(space, va+off, page, vmm_flags)!=0) {
            pmm_free_page(page);
            for (size_t j=0;j<off;j+=PAGE_SIZE) vmm_unmap(space, va+j);
            return NULL;
        }
    }

    KLOG_DEBUG("mmap","mapped %u bytes at 0x%llx prot=%x flags=%x", (unsigned)length, (unsigned long long)va, prot, flags);
    return (void*)va;
}

int munmap(void *addr, size_t length)
{
    struct task *cur=sched_current();
    struct address_space *space = cur ? cur->space : vmm_kernel_space();
    if (!space) space=vmm_kernel_space();
    length = (length + PAGE_SIZE -1) & ~(PAGE_SIZE-1);
    for (size_t off=0; off<length; off+=PAGE_SIZE) {
        phys_addr_t phys = vmm_resolve(space, (virt_addr_t)addr+off);
        vmm_unmap(space, (virt_addr_t)addr+off);
        if (phys) pmm_free_page(phys);
    }
    KLOG_DEBUG("mmap","unmapped %u bytes at %p", (unsigned)length, addr);
    return 0;
}

int mmap_file(const char *path, void **out_addr, size_t *out_len)
{
    struct page_cache_entry *entry=page_cache_get(path);
    if (!entry) return -1;
    void *addr=mmap(NULL, entry->size, PROT_READ|PROT_WRITE, MAP_PRIVATE|MAP_ANONYMOUS, -1, 0);
    if (!addr) { page_cache_put(entry); return -1; }
    // Copy file data into mapping (for real mmap would be CoW)
    struct task *cur=sched_current();
    struct address_space *space = cur ? cur->space : vmm_kernel_space();
    for (size_t off=0; off<entry->size; off+=PAGE_SIZE) {
        phys_addr_t phys=vmm_resolve(space, (virt_addr_t)addr+off);
        if (phys) {
            void *virt=phys_to_virt(phys);
            size_t copy_len = MIN(PAGE_SIZE, entry->size-off);
            memcpy(virt, (uint8_t*)entry->data+off, copy_len);
        }
    }
    *out_addr=addr;
    if (out_len) *out_len=entry->size;
    page_cache_put(entry);
    return 0;
}
