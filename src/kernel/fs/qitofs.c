/*
 * QitoOS - QitoFS ramdisk loader
 *
 * Unpacks the boot ramdisk image (produced by tools/mkqitofs.py) into the VFS.
 * The format is a header, a flat table of entries, and a data region; see the
 * tool for the authoritative layout.
 */

#include <kernel/fs.h>
#include <kernel/mm.h>
#include <kernel/log.h>
#include <kernel/string.h>
#include <kernel/syscall.h>

#define QITOFS_MAGIC   "QITOFS01"
#define QITOFS_VERSION 1

#define QITOFS_TYPE_FILE 1
#define QITOFS_TYPE_DIR  2

struct qitofs_header {
    char     magic[8];
    uint32_t version;
    uint32_t entry_count;
    uint64_t total_size;
    uint64_t data_offset;
    uint32_t flags;
    uint32_t checksum;
    char     label[32];
} PACKED;

struct qitofs_entry {
    char     path[192];
    uint32_t type;
    uint32_t permissions;
    uint64_t size;
    uint64_t offset;
    uint64_t modified;
    uint32_t uid;
    uint32_t gid;
} PACKED;

int fs_mount_ramdisk(const void *image, size_t size)
{
    if (!image || size < sizeof(struct qitofs_header)) {
        KLOG_WARN("qitofs", "no ramdisk image supplied");
        return -QE_INVAL;
    }

    const struct qitofs_header *header = (const struct qitofs_header *)image;

    if (memcmp(header->magic, QITOFS_MAGIC, 8) != 0) {
        KLOG_ERR("qitofs", "bad magic, not a QitoFS image");
        return -QE_INVAL;
    }
    if (header->version != QITOFS_VERSION) {
        KLOG_ERR("qitofs", "unsupported image version %u", header->version);
        return -QE_INVAL;
    }
    if (header->total_size > size) {
        KLOG_ERR("qitofs", "image claims %llu bytes but only %llu were loaded",
                 (unsigned long long)header->total_size,
                 (unsigned long long)size);
        return -QE_INVAL;
    }

    const struct qitofs_entry *entries =
        (const struct qitofs_entry *)((const uint8_t *)image +
                                      sizeof(struct qitofs_header));
    const uint8_t *data = (const uint8_t *)image + header->data_offset;

    /* Verify the payload checksum before trusting any of it. */
    uint32_t checksum = 0;
    for (uint32_t i = 0; i < header->entry_count; i++) {
        const struct qitofs_entry *entry = &entries[i];
        if (entry->type != QITOFS_TYPE_FILE) {
            continue;
        }
        for (uint64_t b = 0; b < entry->size; b++) {
            checksum += data[entry->offset + b];
        }
    }
    if (checksum != header->checksum) {
        KLOG_ERR("qitofs", "checksum mismatch (computed %08x, expected %08x)",
                 checksum, header->checksum);
        return -QE_IO;
    }

    uint32_t created_dirs  = 0;
    uint32_t created_files = 0;
    uint64_t bytes         = 0;

    for (uint32_t i = 0; i < header->entry_count; i++) {
        const struct qitofs_entry *entry = &entries[i];

        char path[FS_PATH_MAX];
        strlcpy(path, entry->path, sizeof(path));

        if (entry->type == QITOFS_TYPE_DIR) {
            if (!fs_lookup(path)) {
                if (fs_mkdir(path, entry->permissions) == 0) {
                    created_dirs++;
                }
            }
            continue;
        }

        if (entry->offset + entry->size > header->total_size - header->data_offset) {
            KLOG_WARN("qitofs", "entry '%s' extends past the image, skipped", path);
            continue;
        }

        /* Make sure the parent directory exists. */
        char parent[FS_PATH_MAX];
        strlcpy(parent, path, sizeof(parent));
        char *slash = strrchr(parent, '/');
        if (slash && slash != parent) {
            *slash = '\0';
            if (!fs_lookup(parent)) {
                fs_mkdir(parent, 0755);
            }
        }

        struct fs_node *node = fs_lookup(path);
        if (!node) {
            node = fs_create(path, FS_FILE, entry->permissions);
        }
        if (!node) {
            KLOG_WARN("qitofs", "could not create '%s'", path);
            continue;
        }

        if (entry->size) {
            node->data = kmalloc((size_t)entry->size);
            if (!node->data) {
                KLOG_ERR("qitofs", "out of memory unpacking '%s'", path);
                continue;
            }
            memcpy(node->data, data + entry->offset, (size_t)entry->size);
            node->size     = entry->size;
            node->capacity = entry->size;
        }
        node->permissions = entry->permissions;
        node->modified    = entry->modified;
        node->uid         = entry->uid;
        node->gid         = entry->gid;

        created_files++;
        bytes += entry->size;
    }

    char label[33];
    memcpy(label, header->label, 32);
    label[32] = '\0';

    KLOG_INFO("qitofs", "mounted '%s': %u files, %u directories, %llu bytes",
              label, created_files, created_dirs, (unsigned long long)bytes);
    return 0;
}
