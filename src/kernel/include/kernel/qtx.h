/*
 * QitoOS - QTX, Qito eXecutables
 *
 * Native program format. The on-disk magic stays "QX" — only the format byte
 * changes from 'L' (old LQX) to 'X'. Extension .qtx.
 *
 * Header layout is 88 bytes, little-endian. Python struct: <2sBBHHIIQQIIIIIIII24s
 */

#ifndef QITO_QTX_H
#define QITO_QTX_H

#include <kernel/types.h>

struct shell;

#define QX_SIGNATURE      "QX"
#define QTX_VERSION       1
#define QTX_MAX_SECTIONS  16
#define QTX_MAX_IMPORTS   64
#define QTX_MAX_SYMBOLS   128
#define QTX_NAME_MAX      24

#define QTX_FORMAT_EXEC   'X'
#define QTX_FORMAT_LIB    'D'

#define QTX_MACHINE_X86_64 0x8664

#define QTX_FLAG_EXECUTABLE 0x0001
#define QTX_FLAG_LIBRARY    0x0002
#define QTX_FLAG_SERVICE    0x0004
#define QTX_FLAG_DESKTOP    0x0008
#define QTX_FLAG_PRIVILEGED 0x0010

typedef enum {
    QTX_SECTION_CODE = 1,
    QTX_SECTION_DATA = 2,
    QTX_SECTION_RODATA = 3,
    QTX_SECTION_BSS = 4,
    QTX_SECTION_RESOURCE = 5,
} qtx_section_kind_t;

#define QTX_SEC_READ    0x01
#define QTX_SEC_WRITE   0x02
#define QTX_SEC_EXEC    0x04

struct qx_header {
    char     signature[2];
    uint8_t  format;
    uint8_t  version;
    uint16_t machine;
    uint16_t flags;
    uint32_t header_size;
    uint32_t total_size;
    uint64_t entry_point;
    uint64_t load_base;
    uint32_t section_count;
    uint32_t section_offset;
    uint32_t import_count;
    uint32_t import_offset;
    uint32_t symbol_count;
    uint32_t symbol_offset;
    uint32_t checksum;
    uint32_t stack_size;
    char     name[QTX_NAME_MAX];
} PACKED;

struct qtx_section {
    char     name[12];
    uint8_t  kind;
    uint8_t  flags;
    uint16_t alignment;
    uint64_t virtual_address;
    uint32_t file_offset;
    uint32_t file_size;
    uint32_t memory_size;
} PACKED;

struct qtx_import {
    char     name[QTX_NAME_MAX];
    uint64_t patch_address;
} PACKED;

struct qtx_symbol {
    char     name[QTX_NAME_MAX];
    uint64_t address;
} PACKED;

struct qtx_image {
    char     name[QTX_NAME_MAX];
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

int qtx_validate(const void *data, size_t len, const char **reason);
int qtx_probe(const char *path, struct qx_header *out);
int qtx_load(const char *path, struct qtx_image *out);
int qtx_load_memory(const void *data, size_t len, struct qtx_image *out);
void qtx_unload(struct qtx_image *image);
int qtx_run(struct qtx_image *image, int argc, char **argv);
uint64_t qtx_symbol(const struct qtx_image *image, const char *name);

struct qtx_export {
    const char *name;
    void       *address;
};

void qtx_init(void);
void *qtx_resolve_export(const char *name);
int   qtx_export_count(void);
const char *qtx_export_name(int index);

const char *qtx_section_kind_name(uint8_t kind);
void        qtx_describe(const struct qx_header *header, struct shell *sh);

#endif /* QITO_QTX_H */
