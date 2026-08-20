/*
 * QitoOS - QDL, Qito Dynamic Libraries
 *
 * Sibling format to QTX, extension .qdl, same QX header with format byte 'D'.
 * A QDL has no entry point, sets library flag, and its symbol table is an
 * export table other images link against.
 *
 * Dynamic linking: when loading .qtx, resolve each import first against
 * kernel export table, then against any loaded .qdl. Load QDLs on demand
 * from lib directory, refcount, unload when last user exits.
 */

#ifndef QITO_QDL_H
#define QITO_QDL_H

#include <kernel/qtx.h>

#define QDL_MAX_LOADED 32
#define QDL_PATH_MAX   64

struct qdl_entry {
    char               name[QTX_NAME_MAX];
    char               path[QDL_PATH_MAX];
    struct qtx_image   image;
    struct qtx_symbol *symbols;      /* exported symbols */
    int                symbol_count;
    int                refcount;
    bool_t             loaded;
    uint64_t           lowest_va;
};

void qdl_init(void);
int  qdl_load(const char *path);
int  qdl_load_auto(const char *path); /* internal, loads without refcount bump */
int  qdl_unload(const char *name);
struct qdl_entry *qdl_find(const char *name);
void *qdl_resolve_symbol(const char *name);
int  qdl_resolve_import(const char *import_name, void **out_addr);
int  qdl_load_needed(const char *import_name); /* on-demand from /lib */
void qdl_retain(struct qdl_entry *entry);
void qdl_release(struct qdl_entry *entry);
int  qdl_loaded_count(void);
const char *qdl_loaded_name(int index);
int  qdl_validate_symbol_table(const void *data, size_t len, const struct qx_header *hdr);

void qdl_describe(struct shell *sh);

#endif /* QITO_QDL_H */
