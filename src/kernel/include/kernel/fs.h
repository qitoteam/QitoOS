/*
 * Qira OS - virtual filesystem
 *
 * A node-based VFS with pluggable backends. QiraFS (an in-memory filesystem
 * seeded from the boot ramdisk) provides the root; devfs exposes device
 * nodes; procfs exposes live kernel state as readable text files.
 */
#ifndef QIRA_FS_H
#define QIRA_FS_H

#include <kernel/types.h>

#define FS_NAME_MAX 64
#define FS_PATH_MAX 512
#define FS_MAX_MOUNTS 8

typedef enum {
    FS_FILE = 1,
    FS_DIR  = 2,
    FS_DEV  = 3,
    FS_LINK = 4,
} fs_node_type_t;

/* Permission bits, mirroring the POSIX layout. */
#define FS_PERM_OTHER_X 0001
#define FS_PERM_OTHER_W 0002
#define FS_PERM_OTHER_R 0004
#define FS_PERM_GROUP_X 0010
#define FS_PERM_GROUP_W 0020
#define FS_PERM_GROUP_R 0040
#define FS_PERM_OWNER_X 0100
#define FS_PERM_OWNER_W 0200
#define FS_PERM_OWNER_R 0400

/* Open flags */
#define O_RDONLY 0x0001
#define O_WRONLY 0x0002
#define O_RDWR   0x0003
#define O_CREATE 0x0100
#define O_APPEND 0x0200
#define O_TRUNC  0x0400
#define O_EXCL   0x0800
#define O_DIR    0x1000

/* Seek origins */
#define SEEK_SET 0
#define SEEK_CUR 1
#define SEEK_END 2

struct fs_node;
struct file;

/* Backend operations. Any may be NULL if unsupported. */
struct fs_ops {
    ssize_t (*read)(struct fs_node *node, uint64_t offset, void *buf, size_t len);
    ssize_t (*write)(struct fs_node *node, uint64_t offset, const void *buf,
                     size_t len);
    int     (*truncate)(struct fs_node *node, uint64_t size);
    int     (*open)(struct fs_node *node, uint32_t flags);
    int     (*close)(struct fs_node *node);
    /* Refresh a synthetic node's contents before a read (procfs). */
    int     (*refresh)(struct fs_node *node);
};

struct fs_node {
    char            name[FS_NAME_MAX];
    fs_node_type_t  type;
    uint32_t        permissions;
    uint32_t        uid;
    uint32_t        gid;

    uint64_t        size;
    uint64_t        capacity;
    uint8_t        *data;          /* in-memory contents for QiraFS      */

    uint64_t        created;       /* unix timestamps                    */
    uint64_t        modified;
    uint64_t        accessed;

    const struct fs_ops *ops;
    void           *backing;       /* driver private data                */

    struct fs_node *parent;
    struct fs_node *children;      /* first child, for directories       */
    struct fs_node *sibling;       /* next entry in the parent           */

    uint32_t        open_count;
    uint64_t        inode;
};

struct file {
    struct fs_node *node;
    uint64_t        offset;
    uint32_t        flags;
    uint32_t        refs;
};

struct fs_stat {
    uint64_t size;
    uint32_t type;
    uint32_t permissions;
    uint32_t uid;
    uint32_t gid;
    uint64_t created;
    uint64_t modified;
    uint64_t inode;
};

struct fs_dirent {
    char     name[FS_NAME_MAX];
    uint32_t type;
    uint64_t size;
};

void fs_init(void);
/* Populate the root filesystem from a QiraFS ramdisk image. */
int  fs_mount_ramdisk(const void *image, size_t size);

struct fs_node *fs_root(void);
struct fs_node *fs_lookup(const char *path);
struct fs_node *fs_lookup_at(struct fs_node *base, const char *path);

struct fs_node *fs_create(const char *path, fs_node_type_t type, uint32_t perms);
int  fs_unlink(const char *path);
int  fs_rename(const char *from, const char *to);
int  fs_mkdir(const char *path, uint32_t perms);

/* Register a device or synthetic node under an existing directory. */
struct fs_node *fs_register_node(const char *dir, const char *name,
                                 fs_node_type_t type, const struct fs_ops *ops,
                                 void *backing);

struct file *fs_open(const char *path, uint32_t flags);
void         fs_close(struct file *file);
ssize_t      fs_read(struct file *file, void *buf, size_t len);
ssize_t      fs_write(struct file *file, const void *buf, size_t len);
int64_t      fs_seek(struct file *file, int64_t offset, int origin);
int          fs_stat(const char *path, struct fs_stat *out);
int          fs_stat_node(struct fs_node *node, struct fs_stat *out);
int          fs_truncate(const char *path, uint64_t size);

/* Directory iteration. Returns 0 on success, negative when exhausted. */
int  fs_readdir(struct fs_node *dir, int index, struct fs_dirent *out);
int  fs_readdir_path(const char *path, int index, struct fs_dirent *out);

/* Convenience helpers used all over the kernel and shells. */
int  fs_read_file(const char *path, void *buf, size_t max, size_t *out_len);
int  fs_write_file(const char *path, const void *buf, size_t len);

/* Normalise a possibly relative path against `cwd` into `out`. */
void fs_resolve_path(const char *cwd, const char *path, char *out, size_t size);

const char *fs_type_name(fs_node_type_t type);
void fs_format_permissions(uint32_t perms, fs_node_type_t type, char *out);

struct fs_statistics {
    uint64_t nodes;
    uint64_t files;
    uint64_t directories;
    uint64_t devices;
    uint64_t total_bytes;
    uint64_t open_files;
};
void fs_get_statistics(struct fs_statistics *out);

/* Permission check against the current task's credentials. */
bool_t fs_can_access(struct fs_node *node, uint32_t want_flags);

#endif /* QIRA_FS_H */
