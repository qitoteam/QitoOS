/*
 * Qira OS - LQX, Linked Qira Executables
 *
 * The native program format. ELF is the obvious alternative, but ELF carries
 * thirty years of compatibility baggage that a from-scratch kernel does not
 * need, and parsing it safely is most of a day's work. LQX keeps only what
 * Qira actually uses, which makes the loader small enough to audit line by
 * line — the right trade for code that maps attacker-supplied files.
 *
 * File layout
 * -----------
 *
 *   struct qx_header             88 bytes, begins with the "QX" signature
 *   struct lqx_section[sections] 36 bytes each
 *   struct lqx_import[imports]   32 bytes each, unresolved kernel services
 *   struct lqx_symbol[symbols]   32 bytes each, for diagnostics and linking
 *   section payloads             raw bytes, referenced by offset
 *
 * All multi-byte fields are little-endian. Everything is offset-addressed
 * from the start of the file, so an image can be validated fully before a
 * single byte is mapped.
 *
 * Why "linked"
 * ------------
 *
 * An LQX image is fully linked internally: relocations are already applied
 * for a fixed load base, so the loader does no relocation work. What it does
 * resolve is *imports* — the kernel services a program calls. Each import
 * names a function; the loader looks it up in the kernel's export table and
 * patches the address into the program's import table before entry. That is
 * how a program calls into the system without a syscall trampoline for every
 * entry point, and it means an image that requires a service the running
 * kernel does not provide is rejected at load time rather than crashing.
 */
#ifndef QIRA_LQX_H
#define QIRA_LQX_H

#include <kernel/types.h>

struct shell;

#define QX_SIGNATURE      "QX"
#define LQX_VERSION       1
#define LQX_MAX_SECTIONS  16
#define LQX_MAX_IMPORTS   64
#define LQX_MAX_SYMBOLS   128
#define LQX_NAME_MAX      24

/* Machine types. */
#define LQX_MACHINE_X86_64 0x8664

/* Header flags. */
#define LQX_FLAG_EXECUTABLE 0x0001   /* a runnable program                */
#define LQX_FLAG_LIBRARY    0x0002   /* provides symbols, no entry point  */
#define LQX_FLAG_SERVICE    0x0004   /* runs as a background service      */
#define LQX_FLAG_DESKTOP    0x0008   /* registers a desktop application   */
#define LQX_FLAG_PRIVILEGED 0x0010   /* asks for elevated capabilities    */

/* Section kinds. */
typedef enum {
    LQX_SECTION_CODE = 1,     /* executable, read-only                    */
    LQX_SECTION_DATA = 2,     /* initialised, writable                    */
    LQX_SECTION_RODATA = 3,   /* initialised, read-only                   */
    LQX_SECTION_BSS = 4,      /* zero-filled, occupies no file space      */
    LQX_SECTION_RESOURCE = 5, /* icons, strings and other assets          */
} lqx_section_kind_t;

/* Section flags. */
#define LQX_SEC_READ    0x01
#define LQX_SEC_WRITE   0x02
#define LQX_SEC_EXEC    0x04

/*
 * The QX header. The first two bytes are the "QX" signature, which is what
 * `file`-style detection and the shell's executability check look for.
 */
struct qx_header {
    char     signature[2];      /* "QX"                                    */
    uint8_t  format;            /* 'L' for a linked image                  */
    uint8_t  version;
    uint16_t machine;           /* LQX_MACHINE_*                           */
    uint16_t flags;

    uint32_t header_size;       /* == sizeof(struct qx_header)             */
    uint32_t total_size;        /* the whole file, for validation          */

    uint64_t entry_point;       /* virtual address, 0 for a library        */
    uint64_t load_base;         /* the base the image was linked for       */

    uint32_t section_count;
    uint32_t section_offset;
    uint32_t import_count;
    uint32_t import_offset;
    uint32_t symbol_count;
    uint32_t symbol_offset;

    uint32_t checksum;          /* 32-bit wrapping sum, checksum field 0   */
    uint32_t stack_size;        /* requested stack, 0 for the default      */

    char     name[LQX_NAME_MAX];
} PACKED;

struct lqx_section {
    char     name[12];
    uint8_t  kind;              /* lqx_section_kind_t                      */
    uint8_t  flags;             /* LQX_SEC_*                               */
    uint16_t alignment;
    uint64_t virtual_address;
    uint32_t file_offset;       /* 0 for BSS                               */
    uint32_t file_size;         /* 0 for BSS                               */
    uint32_t memory_size;       /* >= file_size; the remainder is zeroed   */
} PACKED;

struct lqx_import {
    char     name[LQX_NAME_MAX];
    uint64_t patch_address;     /* where the loader writes the resolution  */
} PACKED;

struct lqx_symbol {
    char     name[LQX_NAME_MAX];
    uint64_t address;
} PACKED;

/* A loaded image. */
struct lqx_image {
    char     name[LQX_NAME_MAX];
    void    *base;              /* the allocated image in kernel memory    */
    size_t   size;
    uint64_t entry;             /* absolute, ready to call                 */
    uint16_t flags;
    int      section_count;
    int      import_count;
    int      symbol_count;
    bool_t   loaded;
};

/*
 * Validate an in-memory image without loading it. Returns 0 when the file is
 * well formed and every offset lies inside the buffer.
 */
int lqx_validate(const void *data, size_t len, const char **reason);

/* Read the header of a file on disk. */
int lqx_probe(const char *path, struct qx_header *out);

/*
 * Load an image: allocate memory, copy each section, zero the BSS, resolve
 * imports against the kernel export table and apply page permissions.
 */
int lqx_load(const char *path, struct lqx_image *out);
int lqx_load_memory(const void *data, size_t len, struct lqx_image *out);

void lqx_unload(struct lqx_image *image);

/* Run a loaded executable as a task. Returns the pid, or negative. */
int lqx_run(struct lqx_image *image, int argc, char **argv);

/* Look up a symbol the image exports. */
uint64_t lqx_symbol(const struct lqx_image *image, const char *name);

/* --- the kernel export table ------------------------------------------ */

struct lqx_export {
    const char *name;
    void       *address;
};

/* Register the services programs may import. */
void lqx_init(void);

void *lqx_resolve_export(const char *name);
int   lqx_export_count(void);
const char *lqx_export_name(int index);

/* Human-readable descriptions, used by the shell and the file manager. */
const char *lqx_section_kind_name(uint8_t kind);
void        lqx_describe(const struct qx_header *header, struct shell *sh);

#endif /* QIRA_LQX_H */
