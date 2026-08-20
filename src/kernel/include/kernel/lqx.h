/* Legacy LQX wrapper – forwards to QTX, for compatibility */
#ifndef QITO_LQX_H
#define QITO_LQX_H
#include <kernel/qtx.h>

/* Old constants -> new */
#define LQX_VERSION QTX_VERSION
#define LQX_MAX_SECTIONS QTX_MAX_SECTIONS
#define LQX_MAX_IMPORTS QTX_MAX_IMPORTS
#define LQX_MAX_SYMBOLS QTX_MAX_SYMBOLS
#define LQX_NAME_MAX QTX_NAME_MAX
#define LQX_MACHINE_X86_64 QTX_MACHINE_X86_64
#define LQX_FLAG_EXECUTABLE QTX_FLAG_EXECUTABLE
#define LQX_FLAG_LIBRARY QTX_FLAG_LIBRARY
#define LQX_FLAG_SERVICE QTX_FLAG_SERVICE
#define LQX_FLAG_DESKTOP QTX_FLAG_DESKTOP
#define LQX_FLAG_PRIVILEGED QTX_FLAG_PRIVILEGED
#define LQX_SECTION_CODE QTX_SECTION_CODE
#define LQX_SECTION_DATA QTX_SECTION_DATA
#define LQX_SECTION_RODATA QTX_SECTION_RODATA
#define LQX_SECTION_BSS QTX_SECTION_BSS
#define LQX_SECTION_RESOURCE QTX_SECTION_RESOURCE
#define LQX_SEC_READ QTX_SEC_READ
#define LQX_SEC_WRITE QTX_SEC_WRITE
#define LQX_SEC_EXEC QTX_SEC_EXEC

/* Legacy structs – same layout as QTX, defined separately to avoid macro conflicts */
struct lqx_section {
    char     name[12];
    uint8_t  kind;
    uint8_t  flags;
    uint16_t alignment;
    uint64_t virtual_address;
    uint32_t file_offset;
    uint32_t file_size;
    uint32_t memory_size;
} PACKED;

struct lqx_import {
    char     name[24];
    uint64_t patch_address;
} PACKED;

struct lqx_symbol {
    char     name[24];
    uint64_t address;
} PACKED;

struct lqx_image {
    char     name[24];
    void    *base;
    size_t   size;
    uint64_t entry;
    uint16_t flags;
    int      section_count;
    int      import_count;
    int      symbol_count;
    bool_t   loaded;
    bool_t   is_user;
    struct address_space *space;
    uint64_t user_stack_top;
    int      dep_qdl_count;
    int      dep_qdl_ids[8];
};

struct lqx_export {
    const char *name;
    void       *address;
};

/* Function wrappers */
static inline int lqx_validate(const void *d, size_t l, const char **r){ return qtx_validate(d,l,r); }
static inline int lqx_probe(const char *p, struct qx_header *o){ return qtx_probe(p,o); }
static inline int lqx_load(const char *p, struct lqx_image *o){ return qtx_load(p,(struct qtx_image*)o); }
static inline int lqx_load_memory(const void *d,size_t l, struct lqx_image *o){ return qtx_load_memory(d,l,(struct qtx_image*)o); }
static inline void lqx_unload(struct lqx_image *i){ qtx_unload((struct qtx_image*)i); }
static inline int lqx_run(struct lqx_image *i,int a,char **v){ return qtx_run((struct qtx_image*)i,a,v); }
static inline uint64_t lqx_symbol(const struct lqx_image *i,const char *n){ return qtx_symbol((const struct qtx_image*)i,n); }
static inline void lqx_init(void){ qtx_init(); }
static inline void *lqx_resolve_export(const char *n){ return qtx_resolve_export(n); }
static inline int lqx_export_count(void){ return qtx_export_count(); }
static inline const char *lqx_export_name(int idx){ return qtx_export_name(idx); }
static inline const char *lqx_section_kind_name(uint8_t k){ return qtx_section_kind_name(k); }
static inline void lqx_describe(const struct qx_header *h, struct shell *s){ qtx_describe(h,s); }

#endif /* QITO_LQX_H */
