/*
 * Qira OS - LQX loader
 *
 * See kernel/lqx.h for the format. The guiding rule here is that validation
 * happens entirely up front, against the file buffer, before anything is
 * allocated or copied. Once lqx_validate() has passed, the load path can
 * trust every offset it reads.
 */

#include <kernel/lqx.h>
#include <kernel/fs.h>
#include <kernel/mm.h>
#include <kernel/string.h>
#include <kernel/printf.h>
#include <kernel/log.h>
#include <kernel/sched.h>
#include <kernel/shell.h>
#include <kernel/console.h>
#include <kernel/time.h>
#include <kernel/qac.h>

/* --- the export table -------------------------------------------------- */

/*
 * Services an LQX program may import. Keeping this an explicit list rather
 * than exposing every kernel symbol means a program can only reach what the
 * system deliberately offers.
 */
static const struct lqx_export exports[] = {
    /* Console output */
    {"console_write",   (void *)console_write},
    {"console_puts",    (void *)console_puts},
    {"console_clear",   (void *)console_clear},

    /* Memory */
    {"kmalloc",         (void *)kmalloc},
    {"kzalloc",         (void *)kzalloc},
    {"kfree",           (void *)kfree},

    /* Strings */
    {"strlen",          (void *)strlen},
    {"strcmp",          (void *)strcmp},
    {"strlcpy",         (void *)strlcpy},
    {"strlcat",         (void *)strlcat},
    {"memcpy",          (void *)memcpy},
    {"memset",          (void *)memset},
    {"snprintf",        (void *)snprintf},

    /* Filesystem */
    {"fs_read_file",    (void *)fs_read_file},
    {"fs_write_file",   (void *)fs_write_file},
    {"fs_lookup",       (void *)fs_lookup},
    {"fs_unlink",       (void *)fs_unlink},
    {"fs_mkdir",        (void *)fs_mkdir},

    /* Time */
    {"time_uptime_ms",  (void *)time_uptime_ms},
    {"time_sleep_ms",   (void *)time_sleep_ms},
    {"rtc_unix_time",   (void *)rtc_unix_time},

    /* Tasks */
    {"sched_yield",     (void *)sched_yield},
    {"sched_sleep_ms",  (void *)sched_sleep_ms},
    {"sched_current_pid", (void *)sched_current_pid},

    /* Icons */
    {"qac_load",        (void *)qac_load},
    {"qac_draw",        (void *)qac_draw},
    {"qac_free",        (void *)qac_free},
};

void *lqx_resolve_export(const char *name)
{
    for (size_t i = 0; i < ARRAY_SIZE(exports); i++) {
        if (strcmp(exports[i].name, name) == 0) {
            return exports[i].address;
        }
    }
    return NULL;
}

int lqx_export_count(void)
{
    return (int)ARRAY_SIZE(exports);
}

const char *lqx_export_name(int index)
{
    if (index < 0 || index >= (int)ARRAY_SIZE(exports)) {
        return NULL;
    }
    return exports[index].name;
}

void lqx_init(void)
{
    KLOG_INFO("lqx", "loader ready, %d kernel services exported",
              (int)ARRAY_SIZE(exports));
}

/* --- validation -------------------------------------------------------- */

/* Does [offset, offset+length) lie wholly inside a buffer of `len` bytes? */
static bool_t in_bounds(uint64_t offset, uint64_t length, size_t len)
{
    if (offset > len) {
        return false;
    }
    if (length > len) {
        return false;
    }
    return offset + length <= len;
}

static uint32_t compute_checksum(const uint8_t *data, size_t len)
{
    uint32_t sum = 0;
    /* The checksum field itself is excluded from the sum. */
    size_t   checksum_at = (size_t)((const uint8_t *)&((const struct qx_header *)0)->checksum - (const uint8_t *)0);

    for (size_t i = 0; i < len; i++) {
        /* The checksum field itself is treated as zero. */
        if (i >= checksum_at && i < checksum_at + sizeof(uint32_t)) {
            continue;
        }
        sum += data[i];
    }
    return sum;
}

int lqx_validate(const void *data, size_t len, const char **reason)
{
    #define REJECT(text)                 \
        do {                             \
            if (reason) *reason = text;  \
            return -1;                   \
        } while (0)

    if (!data || len < sizeof(struct qx_header)) {
        REJECT("file is smaller than the QX header");
    }

    const struct qx_header *header = (const struct qx_header *)data;

    if (header->signature[0] != 'Q' || header->signature[1] != 'X') {
        REJECT("missing the QX signature");
    }
    if (header->format != 'L') {
        REJECT("not a linked image");
    }
    if (header->version != LQX_VERSION) {
        REJECT("unsupported LQX version");
    }
    if (header->machine != LQX_MACHINE_X86_64) {
        REJECT("built for a different machine");
    }
    if (header->header_size != sizeof(struct qx_header)) {
        REJECT("header size does not match this kernel");
    }
    if (header->total_size != len) {
        REJECT("recorded size does not match the file");
    }

    if (header->section_count == 0 ||
        header->section_count > LQX_MAX_SECTIONS) {
        REJECT("section count is out of range");
    }
    if (header->import_count > LQX_MAX_IMPORTS) {
        REJECT("too many imports");
    }
    if (header->symbol_count > LQX_MAX_SYMBOLS) {
        REJECT("too many symbols");
    }

    /* Every table must lie inside the file. */
    if (!in_bounds(header->section_offset,
                   (uint64_t)header->section_count * sizeof(struct lqx_section),
                   len)) {
        REJECT("the section table runs past the end of the file");
    }
    if (header->import_count &&
        !in_bounds(header->import_offset,
                   (uint64_t)header->import_count * sizeof(struct lqx_import),
                   len)) {
        REJECT("the import table runs past the end of the file");
    }
    if (header->symbol_count &&
        !in_bounds(header->symbol_offset,
                   (uint64_t)header->symbol_count * sizeof(struct lqx_symbol),
                   len)) {
        REJECT("the symbol table runs past the end of the file");
    }

    /* Each section's payload must lie inside the file too. */
    const struct lqx_section *sections =
        (const struct lqx_section *)((const uint8_t *)data +
                                     header->section_offset);

    bool_t has_code = false;

    for (uint32_t i = 0; i < header->section_count; i++) {
        const struct lqx_section *section = &sections[i];

        if (section->memory_size < section->file_size) {
            REJECT("a section claims less memory than file data");
        }
        if (section->memory_size > 64 * 1024 * 1024) {
            REJECT("a section is implausibly large");
        }
        if (section->kind != LQX_SECTION_BSS) {
            if (!in_bounds(section->file_offset, section->file_size, len)) {
                REJECT("a section's data runs past the end of the file");
            }
        }
        if (section->kind == LQX_SECTION_CODE) {
            has_code = true;
            if (!(section->flags & LQX_SEC_EXEC)) {
                REJECT("a code section is not marked executable");
            }
        }
        /* Writable and executable at once is refused outright. */
        if ((section->flags & LQX_SEC_WRITE) && (section->flags & LQX_SEC_EXEC)) {
            REJECT("a section is both writable and executable");
        }
    }

    if ((header->flags & LQX_FLAG_EXECUTABLE) && !has_code) {
        REJECT("an executable image has no code section");
    }
    if ((header->flags & LQX_FLAG_EXECUTABLE) && header->entry_point == 0) {
        REJECT("an executable image has no entry point");
    }

    /* The entry point must land inside an executable section. */
    if (header->entry_point) {
        bool_t entry_ok = false;
        for (uint32_t i = 0; i < header->section_count; i++) {
            const struct lqx_section *section = &sections[i];
            if (!(section->flags & LQX_SEC_EXEC)) {
                continue;
            }
            if (header->entry_point >= section->virtual_address &&
                header->entry_point <
                    section->virtual_address + section->memory_size) {
                entry_ok = true;
                break;
            }
        }
        if (!entry_ok) {
            REJECT("the entry point is not inside an executable section");
        }
    }

    /* Import names must be NUL-terminated within their field. */
    if (header->import_count) {
        const struct lqx_import *imports =
            (const struct lqx_import *)((const uint8_t *)data +
                                        header->import_offset);
        for (uint32_t i = 0; i < header->import_count; i++) {
            bool_t terminated = false;
            for (size_t c = 0; c < LQX_NAME_MAX; c++) {
                if (imports[i].name[c] == '\0') {
                    terminated = true;
                    break;
                }
            }
            if (!terminated) {
                REJECT("an import name is not terminated");
            }
        }
    }

    uint32_t expected = compute_checksum((const uint8_t *)data, len);
    if (expected != header->checksum) {
        REJECT("checksum mismatch: the file is corrupt");
    }

    #undef REJECT

    if (reason) {
        *reason = NULL;
    }
    return 0;
}

int lqx_probe(const char *path, struct qx_header *out)
{
    char   buffer[sizeof(struct qx_header)];
    size_t got = 0;

    if (fs_read_file(path, buffer, sizeof(buffer), &got) != 0) {
        return -1;
    }
    if (got < sizeof(struct qx_header)) {
        return -1;
    }

    const struct qx_header *header = (const struct qx_header *)buffer;
    if (header->signature[0] != 'Q' || header->signature[1] != 'X') {
        return -1;
    }

    if (out) {
        memcpy(out, buffer, sizeof(*out));
    }
    return 0;
}

/* --- loading ----------------------------------------------------------- */

int lqx_load_memory(const void *data, size_t len, struct lqx_image *out)
{
    const char *reason = NULL;

    if (!out) {
        return -1;
    }
    if (lqx_validate(data, len, &reason) != 0) {
        KLOG_ERR("lqx", "rejected image: %s", reason ? reason : "invalid");
        return -1;
    }

    const struct qx_header *header = (const struct qx_header *)data;
    const struct lqx_section *sections =
        (const struct lqx_section *)((const uint8_t *)data +
                                     header->section_offset);

    /* Work out the span the image occupies once laid out. */
    uint64_t lowest  = UINT64_MAX;
    uint64_t highest = 0;

    for (uint32_t i = 0; i < header->section_count; i++) {
        uint64_t start = sections[i].virtual_address;
        uint64_t end   = start + sections[i].memory_size;
        if (start < lowest) {
            lowest = start;
        }
        if (end > highest) {
            highest = end;
        }
    }

    if (highest <= lowest) {
        KLOG_ERR("lqx", "image has no addressable content");
        return -1;
    }

    size_t span = (size_t)(highest - lowest);
    if (span > 64 * 1024 * 1024) {
        KLOG_ERR("lqx", "image span of %llu bytes is too large",
                 (unsigned long long)span);
        return -1;
    }

    void *base = kzalloc(span);
    if (!base) {
        KLOG_ERR("lqx", "no memory for a %llu byte image",
                 (unsigned long long)span);
        return -1;
    }

    /* Copy each section into place; BSS is already zero from kzalloc. */
    for (uint32_t i = 0; i < header->section_count; i++) {
        const struct lqx_section *section = &sections[i];
        uint8_t *destination =
            (uint8_t *)base + (section->virtual_address - lowest);

        if (section->kind != LQX_SECTION_BSS && section->file_size) {
            memcpy(destination, (const uint8_t *)data + section->file_offset,
                   section->file_size);
        }
    }

    /*
     * Resolve imports. The image was linked expecting the loader to write a
     * function address at each patch site; an unresolved import is fatal,
     * because letting the program run and jump through a null pointer would
     * be strictly worse than refusing to start it.
     */
    if (header->import_count) {
        const struct lqx_import *imports =
            (const struct lqx_import *)((const uint8_t *)data +
                                        header->import_offset);

        for (uint32_t i = 0; i < header->import_count; i++) {
            void *target = lqx_resolve_export(imports[i].name);
            if (!target) {
                KLOG_ERR("lqx", "unresolved import '%s'", imports[i].name);
                kfree(base);
                return -1;
            }

            uint64_t patch = imports[i].patch_address;
            if (patch < lowest || patch + sizeof(void *) > highest) {
                KLOG_ERR("lqx", "import '%s' patches outside the image",
                         imports[i].name);
                kfree(base);
                return -1;
            }

            *(void **)((uint8_t *)base + (patch - lowest)) = target;
        }
    }

    memset(out, 0, sizeof(*out));
    strlcpy(out->name, header->name, sizeof(out->name));
    out->base          = base;
    out->size          = span;
    out->entry         = header->entry_point
                             ? (uint64_t)base + (header->entry_point - lowest)
                             : 0;
    out->flags         = header->flags;
    out->section_count = (int)header->section_count;
    out->import_count  = (int)header->import_count;
    out->symbol_count  = (int)header->symbol_count;
    out->loaded        = true;

    KLOG_INFO("lqx", "loaded '%s': %llu bytes, %u sections, %u imports",
              out->name, (unsigned long long)span, header->section_count,
              header->import_count);
    return 0;
}

int lqx_load(const char *path, struct lqx_image *out)
{
    struct fs_stat stat;

    if (fs_stat(path, &stat) != 0) {
        KLOG_ERR("lqx", "%s: no such file", path);
        return -1;
    }
    if (stat.size < sizeof(struct qx_header) || stat.size > 16 * 1024 * 1024) {
        KLOG_ERR("lqx", "%s: implausible size", path);
        return -1;
    }

    void *buffer = kmalloc(stat.size);
    if (!buffer) {
        return -1;
    }

    size_t got = 0;
    if (fs_read_file(path, buffer, stat.size, &got) != 0 || got != stat.size) {
        kfree(buffer);
        return -1;
    }

    int result = lqx_load_memory(buffer, got, out);
    kfree(buffer);
    return result;
}

void lqx_unload(struct lqx_image *image)
{
    if (!image || !image->loaded) {
        return;
    }
    kfree(image->base);
    image->base   = NULL;
    image->loaded = false;
}

/* --- execution --------------------------------------------------------- */

struct lqx_run_context {
    uint64_t entry;
    int      argc;
    char   **argv;
};

static void lqx_trampoline(void *arg)
{
    struct lqx_run_context *context = (struct lqx_run_context *)arg;

    int (*entry)(int, char **) = (int (*)(int, char **))context->entry;
    int status = entry(context->argc, context->argv);

    KLOG_INFO("lqx", "program exited with status %d", status);

    for (int i = 0; i < context->argc; i++) {
        kfree(context->argv[i]);
    }
    kfree(context->argv);
    kfree(context);

    sched_exit(status);
}

int lqx_run(struct lqx_image *image, int argc, char **argv)
{
    if (!image || !image->loaded || !image->entry) {
        return -1;
    }
    if (!(image->flags & LQX_FLAG_EXECUTABLE)) {
        KLOG_ERR("lqx", "'%s' is not an executable image", image->name);
        return -1;
    }

    struct lqx_run_context *context = kzalloc(sizeof(*context));
    if (!context) {
        return -1;
    }

    /* Copy the arguments: the caller's storage may not outlive the task. */
    context->argv = kzalloc(sizeof(char *) * (size_t)(argc + 1));
    if (!context->argv) {
        kfree(context);
        return -1;
    }

    for (int i = 0; i < argc; i++) {
        size_t len = strlen(argv[i]) + 1;
        context->argv[i] = kmalloc(len);
        if (context->argv[i]) {
            memcpy(context->argv[i], argv[i], len);
        }
    }
    context->argc  = argc;
    context->entry = image->entry;

    int pid = sched_create_kernel_task(image->name, lqx_trampoline, context,
                                       PRIO_NORMAL);
    if (pid < 0) {
        for (int i = 0; i < argc; i++) {
            kfree(context->argv[i]);
        }
        kfree(context->argv);
        kfree(context);
        return -1;
    }

    KLOG_INFO("lqx", "started '%s' as pid %d", image->name, pid);
    return pid;
}

uint64_t lqx_symbol(const struct lqx_image *image, const char *name)
{
    UNUSED(image);
    UNUSED(name);
    /* Symbol lookup in a loaded image is not implemented yet. */
    return 0;
}

/* --- description ------------------------------------------------------- */

const char *lqx_section_kind_name(uint8_t kind)
{
    switch (kind) {
    case LQX_SECTION_CODE:     return "code";
    case LQX_SECTION_DATA:     return "data";
    case LQX_SECTION_RODATA:   return "rodata";
    case LQX_SECTION_BSS:      return "bss";
    case LQX_SECTION_RESOURCE: return "resource";
    default:                   return "unknown";
    }
}

void lqx_describe(const struct qx_header *header, struct shell *sh)
{
    if (!header || !sh) {
        return;
    }

    shell_printf(sh, "  %-16s %s\n", "Name", header->name);
    shell_printf(sh, "  %-16s QX/L version %u\n", "Format", header->version);
    shell_printf(sh, "  %-16s %s\n", "Machine",
                 header->machine == LQX_MACHINE_X86_64 ? "x86_64" : "unknown");
    shell_printf(sh, "  %-16s %u bytes\n", "Size", header->total_size);
    shell_printf(sh, "  %-16s 0x%llx\n", "Entry point",
                 (unsigned long long)header->entry_point);
    shell_printf(sh, "  %-16s 0x%llx\n", "Link base",
                 (unsigned long long)header->load_base);
    shell_printf(sh, "  %-16s %u\n", "Sections", header->section_count);
    shell_printf(sh, "  %-16s %u\n", "Imports", header->import_count);
    shell_printf(sh, "  %-16s %u\n", "Symbols", header->symbol_count);

    shell_printf(sh, "  %-16s", "Attributes");
    if (header->flags & LQX_FLAG_EXECUTABLE) shell_printf(sh, " executable");
    if (header->flags & LQX_FLAG_LIBRARY)    shell_printf(sh, " library");
    if (header->flags & LQX_FLAG_SERVICE)    shell_printf(sh, " service");
    if (header->flags & LQX_FLAG_DESKTOP)    shell_printf(sh, " desktop");
    if (header->flags & LQX_FLAG_PRIVILEGED) shell_printf(sh, " privileged");
    shell_printf(sh, "\n");
}
