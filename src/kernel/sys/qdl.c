/*
 * QitoOS - QDL dynamic library loader
 * Loads .qdl from lib directory on demand, refcounts, unloads when last user exits.
 */

#include <kernel/qdl.h>
#include <kernel/fs.h>
#include <kernel/mm.h>
#include <kernel/string.h>
#include <kernel/log.h>
#include <kernel/shell.h>
#include <kernel/printf.h>

static struct qdl_entry loaded[QDL_MAX_LOADED];
static int loaded_count=0;

void qdl_init(void)
{
    memset(loaded,0,sizeof(loaded));
    loaded_count=0;
    KLOG_INFO("qdl","dynamic library loader ready (lib directory)");
}

static int find_free_slot(void)
{
    for (int i=0;i<QDL_MAX_LOADED;i++) if (!loaded[i].loaded) return i;
    return -1;
}

struct qdl_entry *qdl_find(const char *name)
{
    if (!name) return NULL;
    for (int i=0;i<QDL_MAX_LOADED;i++) if (loaded[i].loaded && strcmp(loaded[i].name,name)==0) return &loaded[i];
    return NULL;
}

struct qdl_entry *qdl_find_exporter(const char *name)
{
    for (int i=0;i<QDL_MAX_LOADED;i++) {
        if (!loaded[i].loaded) continue;
        for (int j=0;j<loaded[i].symbol_count;j++) {
            if (strcmp(loaded[i].symbols[j].name,name)==0) return &loaded[i];
        }
    }
    return NULL;
}

int qdl_entry_index(struct qdl_entry *e)
{
    if (!e) return -1;
    for (int i=0;i<QDL_MAX_LOADED;i++) if (&loaded[i]==e) return i;
    return -1;
}

void qdl_release_by_index(int idx)
{
    if (idx<0||idx>=QDL_MAX_LOADED) return;
    if (!loaded[idx].loaded) return;
    qdl_release(&loaded[idx]);
}

int qdl_loaded_count(void)
{
    int c=0;
    for (int i=0;i<QDL_MAX_LOADED;i++) if (loaded[i].loaded) c++;
    return c;
}
const char *qdl_loaded_name(int index)
{
    int c=0;
    for (int i=0;i<QDL_MAX_LOADED;i++) if (loaded[i].loaded) {
        if (c==index) return loaded[i].name;
        c++;
    }
    return NULL;
}

void qdl_retain(struct qdl_entry *entry)
{
    if (!entry) return;
    entry->refcount++;
    KLOG_DEBUG("qdl","retain '%s' ref=%d",entry->name,entry->refcount);
}
void qdl_release(struct qdl_entry *entry)
{
    if (!entry) return;
    if (entry->refcount>0) entry->refcount--;
    KLOG_DEBUG("qdl","release '%s' ref=%d",entry->name,entry->refcount);
    if (entry->refcount<=0) {
        KLOG_INFO("qdl","unloading '%s'",entry->name);
        qtx_unload(&entry->image);
        if (entry->symbols) kfree(entry->symbols);
        memset(entry,0,sizeof(*entry));
    }
}

void *qdl_resolve_symbol(const char *name)
{
    for (int i=0;i<QDL_MAX_LOADED;i++) {
        if (!loaded[i].loaded) continue;
        for (int j=0;j<loaded[i].symbol_count;j++) {
            if (strcmp(loaded[i].symbols[j].name,name)==0) {
                uint64_t addr = loaded[i].symbols[j].address;
                // Translate VA to actual: addr is virtual, need base offset
                // Our qtx loader stored base as kernel heap base, and entry computed as base + (entry_va - lowest)
                // Similarly symbol address should be base + (symbol_va - lowest)
                // We need lowest VA; we stored it? Not yet. For now, assume symbols store already absolute kernel address after load? Let's compute:
                // In qtx_load_memory we didn't translate symbols. For QDL we need to handle.
                // For simplicity, we will store symbols as already patched to actual address if we re-parse.
                // We'll approximate: if symbol address is within image's virtual range, translate like entry.

                // We need lowest VA – we can approximate by assuming load_base is lowest? Let's store lowest in entry.
                // For now, we will return address as: image.base + (symbol.address - lowest)
                // We need lowest; we can estimate lowest as image base's lowest VA stored in qdl_entry.
                uint64_t translated = (uint64_t)loaded[i].image.base + (addr - loaded[i].lowest_va);
                return (void*)translated;
            }
        }
    }
    return NULL;
}

int qdl_resolve_import(const char *import_name, void **out_addr)
{
    void *addr = qdl_resolve_symbol(import_name);
    if (addr) { *out_addr=addr; return 0; }
    return -1;
}

/* Load a QDL file into memory, validate, copy sections, keep symbol table */
int qdl_load(const char *path)
{
    // Check if already loaded by path or name
    for (int i=0;i<QDL_MAX_LOADED;i++) if (loaded[i].loaded && strcmp(loaded[i].path,path)==0) {
        loaded[i].refcount++;
        return 0;
    }

    struct fs_stat st;
    if (fs_stat(path,&st)!=0) return -1;
    if (st.size < sizeof(struct qx_header) || st.size > 4*1024*1024) return -1;
    void *buf = kmalloc(st.size);
    if (!buf) return -1;
    size_t got=0;
    if (fs_read_file(path,buf,st.size,&got)!=0 || got!=st.size) { kfree(buf); return -1; }

    const char *reason=NULL;
    if (qtx_validate(buf,got,&reason)!=0) { KLOG_WARN("qdl","%s invalid: %s",path,reason?reason:"?"); kfree(buf); return -1; }
    const struct qx_header *hdr = buf;
    if (hdr->format!='D') { KLOG_WARN("qdl","%s not a QDL (format %c)",path,hdr->format); kfree(buf); return -1; }

    int slot=find_free_slot();
    if (slot<0) { kfree(buf); return -1; }

    struct qtx_image img;
    if (qtx_load_memory(buf,got,&img)!=0) { kfree(buf); return -1; }

    // Extract symbols
    const struct qtx_symbol *symtab = (const struct qtx_symbol *)((const uint8_t*)buf + hdr->symbol_offset);
    struct qtx_symbol *copy = NULL;
    if (hdr->symbol_count) {
        copy = kmalloc(sizeof(struct qtx_symbol)*hdr->symbol_count);
        if (!copy) { qtx_unload(&img); kfree(buf); return -1; }
        memcpy(copy,symtab,sizeof(struct qtx_symbol)*hdr->symbol_count);
    }

    // Determine lowest VA for translation
    const struct qtx_section *sections = (const struct qtx_section *)((const uint8_t*)buf + hdr->section_offset);
    uint64_t lowest=UINT64_MAX;
    for (uint32_t i=0;i<hdr->section_count;i++) if (sections[i].virtual_address<lowest) lowest=sections[i].virtual_address;

    struct qdl_entry *e=&loaded[slot];
    strlcpy(e->name,hdr->name,sizeof(e->name));
    strlcpy(e->path,path,sizeof(e->path));
    e->image=img;
    e->symbols=copy;
    e->symbol_count=hdr->symbol_count;
    e->refcount=1;
    e->loaded=true;
    e->lowest_va=lowest;

    KLOG_INFO("qdl","loaded '%s' from %s (%d exports, ref=%d)",e->name,path,e->symbol_count,e->refcount);
    kfree(buf);
    return 0;
}

int qdl_load_auto(const char *path){ return qdl_load(path); }

int qdl_unload(const char *name)
{
    struct qdl_entry *e=qdl_find(name);
    if (!e) return -1;
    if (e->refcount>1) {
        KLOG_WARN("qdl","'%s' still in use (ref=%d)",name,e->refcount);
        return -1;
    }
    qdl_release(e);
    return 0;
}

int qdl_load_needed(const char *import_name)
{
    // Scan /lib for .qdl files and try to load one that exports import_name
    struct fs_node *dir = fs_lookup("/lib");
    if (!dir) return -1;
    struct fs_dirent dent;
    for (int i=0; fs_readdir(dir,i,&dent)==0; i++) {
        size_t len=strlen(dent.name);
        if (len<5) continue;
        if (strcmp(dent.name+len-4,".qdl")!=0) continue;
        char path[QDL_PATH_MAX];
        snprintf(path,sizeof(path),"/lib/%s",dent.name);
        // Skip if already loaded
        bool_t already=false;
        for (int k=0;k<QDL_MAX_LOADED;k++) if (loaded[k].loaded && strcmp(loaded[k].path,path)==0) { already=true; break; }
        if (already) {
            // Check if it exports needed symbol
            struct qdl_entry *e=qdl_find_exporter(import_name);
            if (e) return 0;
            continue;
        }
        if (qdl_load(path)==0) {
            // Check if now provides symbol
            if (qdl_resolve_symbol(import_name)) return 0;
            // If not, keep loaded (maybe needed later) and continue
        }
    }
    return -1;
}

void qdl_describe(struct shell *sh)
{
    if (!sh) return;
    shell_printf(sh,"Loaded QDLs: %d\n", qdl_loaded_count());
    for (int i=0;i<QDL_MAX_LOADED;i++) if (loaded[i].loaded) {
        shell_printf(sh,"  %-20s %s  ref=%d  %d exports\n", loaded[i].name, loaded[i].path, loaded[i].refcount, loaded[i].symbol_count);
    }
}

int qdl_validate_symbol_table(const void *data, size_t len, const struct qx_header *hdr)
{
    (void)data; (void)len; (void)hdr;
    return 0;
}
