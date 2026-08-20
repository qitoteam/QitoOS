/*
 * QitoOS - virtual filesystem core
 *
 * Implements the node tree, path resolution, and the file object layer that
 * sits between the system calls and the individual filesystem backends.
 *
 * Regular files live in memory (QitoFS): their contents are a heap buffer that
 * grows as needed. Device and synthetic nodes delegate to an ops table.
 */

#include <kernel/fs.h>
#include <kernel/mm.h>
#include <kernel/log.h>
#include <kernel/string.h>
#include <kernel/time.h>
#include <kernel/sched.h>
#include <kernel/syscall.h>
#include <kernel/spinlock.h>

static struct fs_node *root_node;
static uint64_t        next_inode = 1;
static spinlock_t      fs_lock;
static uint64_t        open_file_count;

const char *fs_type_name(fs_node_type_t type)
{
    switch (type) {
    case FS_FILE: return "file";
    case FS_DIR:  return "directory";
    case FS_DEV:  return "device";
    case FS_LINK: return "link";
    default:      return "unknown";
    }
}

void fs_format_permissions(uint32_t perms, fs_node_type_t type, char *out)
{
    out[0] = (type == FS_DIR) ? 'd' : (type == FS_DEV) ? 'c'
                                    : (type == FS_LINK) ? 'l' : '-';
    out[1] = (perms & FS_PERM_OWNER_R) ? 'r' : '-';
    out[2] = (perms & FS_PERM_OWNER_W) ? 'w' : '-';
    out[3] = (perms & FS_PERM_OWNER_X) ? 'x' : '-';
    out[4] = (perms & FS_PERM_GROUP_R) ? 'r' : '-';
    out[5] = (perms & FS_PERM_GROUP_W) ? 'w' : '-';
    out[6] = (perms & FS_PERM_GROUP_X) ? 'x' : '-';
    out[7] = (perms & FS_PERM_OTHER_R) ? 'r' : '-';
    out[8] = (perms & FS_PERM_OTHER_W) ? 'w' : '-';
    out[9] = (perms & FS_PERM_OTHER_X) ? 'x' : '-';
    out[10] = '\0';
}

static struct fs_node *node_alloc(const char *name, fs_node_type_t type,
                                  uint32_t perms)
{
    struct fs_node *node = kzalloc(sizeof(struct fs_node));
    if (!node) {
        return NULL;
    }

    strlcpy(node->name, name, sizeof(node->name));
    node->type        = type;
    node->permissions = perms;
    node->inode       = next_inode++;
    node->created     = rtc_unix_time();
    node->modified    = node->created;
    node->accessed    = node->created;
    node->uid         = 0;
    node->gid         = 0;
    return node;
}

static void node_link(struct fs_node *parent, struct fs_node *child)
{
    child->parent = parent;

    /* Append so directory listings keep creation order. */
    if (!parent->children) {
        parent->children = child;
        return;
    }
    struct fs_node *last = parent->children;
    while (last->sibling) {
        last = last->sibling;
    }
    last->sibling = child;
}

static void node_unlink(struct fs_node *node)
{
    struct fs_node *parent = node->parent;
    if (!parent) {
        return;
    }
    if (parent->children == node) {
        parent->children = node->sibling;
        return;
    }
    for (struct fs_node *iter = parent->children; iter; iter = iter->sibling) {
        if (iter->sibling == node) {
            iter->sibling = node->sibling;
            return;
        }
    }
}

static void node_free(struct fs_node *node)
{
    /* Recursively free the subtree. */
    struct fs_node *child = node->children;
    while (child) {
        struct fs_node *next = child->sibling;
        node_free(child);
        child = next;
    }
    if (node->data) {
        kfree(node->data);
    }
    kfree(node);
}

static struct fs_node *find_child(struct fs_node *dir, const char *name)
{
    if (!dir || dir->type != FS_DIR) {
        return NULL;
    }
    for (struct fs_node *child = dir->children; child; child = child->sibling) {
        if (strcmp(child->name, name) == 0) {
            return child;
        }
    }
    return NULL;
}

void fs_init(void)
{
    spinlock_init(&fs_lock, "vfs");

    root_node = node_alloc("/", FS_DIR, 0755);
    if (!root_node) {
        panic("vfs: cannot allocate the root node");
    }

    /* The standard directory skeleton. */
    static const char *skeleton[] = {
        "/bin",      /* system executables                   */
        "/dev",      /* device nodes                         */
        "/etc",      /* configuration                        */
        "/home",     /* user home directories                */
        "/home/user",
        "/proc",     /* live kernel state                    */
        "/sys",      /* system information                   */
        "/tmp",      /* scratch space                        */
        "/var",
        "/var/log",  /* persisted logs                       */
        "/usr",
        "/usr/share",
        "/usr/share/doc",
    };

    for (size_t i = 0; i < ARRAY_SIZE(skeleton); i++) {
        fs_mkdir(skeleton[i], 0755);
    }

    /* /tmp is world-writable. */
    struct fs_node *tmp = fs_lookup("/tmp");
    if (tmp) {
        tmp->permissions = 0777;
    }

    KLOG_INFO("vfs", "virtual filesystem mounted at /");
}

struct fs_node *fs_root(void)
{
    return root_node;
}

void fs_resolve_path(const char *cwd, const char *path, char *out, size_t size)
{
    char work[FS_PATH_MAX];

    /* Build an absolute path first. */
    if (path[0] == '/') {
        strlcpy(work, path, sizeof(work));
    } else {
        strlcpy(work, cwd ? cwd : "/", sizeof(work));
        if (work[strlen(work) - 1] != '/') {
            strlcat(work, "/", sizeof(work));
        }
        strlcat(work, path, sizeof(work));
    }

    /* Collapse ".", ".." and duplicate separators. */
    const char *segments[64];
    int         depth = 0;
    char       *save  = NULL;

    for (char *token = strtok_r(work, "/", &save); token;
         token       = strtok_r(NULL, "/", &save)) {
        if (strcmp(token, ".") == 0 || token[0] == '\0') {
            continue;
        }
        if (strcmp(token, "..") == 0) {
            if (depth > 0) {
                depth--;
            }
            continue;
        }
        if (depth < (int)ARRAY_SIZE(segments)) {
            segments[depth++] = token;
        }
    }

    if (depth == 0) {
        strlcpy(out, "/", size);
        return;
    }

    out[0] = '\0';
    for (int i = 0; i < depth; i++) {
        strlcat(out, "/", size);
        strlcat(out, segments[i], size);
    }
}

struct fs_node *fs_lookup_at(struct fs_node *base, const char *path)
{
    if (!path || !*path) {
        return base;
    }

    struct fs_node *node = (path[0] == '/') ? root_node : base;
    char            work[FS_PATH_MAX];
    char           *save = NULL;

    strlcpy(work, path, sizeof(work));

    for (char *token = strtok_r(work, "/", &save); token;
         token       = strtok_r(NULL, "/", &save)) {
        if (strcmp(token, ".") == 0) {
            continue;
        }
        if (strcmp(token, "..") == 0) {
            node = node->parent ? node->parent : root_node;
            continue;
        }
        node = find_child(node, token);
        if (!node) {
            return NULL;
        }
    }
    return node;
}

struct fs_node *fs_lookup(const char *path)
{
    return fs_lookup_at(root_node, path);
}

/* Split "/a/b/c" into the parent node for "/a/b" and the final name "c". */
static struct fs_node *resolve_parent(const char *path, char *name_out,
                                      size_t name_size)
{
    char work[FS_PATH_MAX];
    strlcpy(work, path, sizeof(work));

    char *slash = strrchr(work, '/');
    if (!slash) {
        strlcpy(name_out, work, name_size);
        return root_node;
    }

    strlcpy(name_out, slash + 1, name_size);
    if (slash == work) {
        return root_node;
    }
    *slash = '\0';
    return fs_lookup(work);
}

struct fs_node *fs_create(const char *path, fs_node_type_t type, uint32_t perms)
{
    char            name[FS_NAME_MAX];
    struct fs_node *parent = resolve_parent(path, name, sizeof(name));

    if (!parent || parent->type != FS_DIR || name[0] == '\0') {
        return NULL;
    }
    if (find_child(parent, name)) {
        return NULL;   /* already exists */
    }

    struct fs_node *node = node_alloc(name, type, perms);
    if (!node) {
        return NULL;
    }

    struct task *task = sched_current();
    if (task) {
        node->uid = task->uid;
        node->gid = task->gid;
    }

    bool_t irq = spinlock_acquire(&fs_lock);
    node_link(parent, node);
    parent->modified = rtc_unix_time();
    spinlock_release(&fs_lock, irq);

    return node;
}

int fs_mkdir(const char *path, uint32_t perms)
{
    if (fs_lookup(path)) {
        return -QE_EXIST;
    }
    return fs_create(path, FS_DIR, perms) ? 0 : -QE_NOENT;
}

int fs_unlink(const char *path)
{
    struct fs_node *node = fs_lookup(path);

    if (!node) {
        return -QE_NOENT;
    }
    if (node == root_node) {
        return -QE_PERM;
    }
    if (node->type == FS_DIR && node->children) {
        return -QE_NOTEMPTY;
    }
    if (node->open_count > 0) {
        return -QE_PERM;
    }

    bool_t irq = spinlock_acquire(&fs_lock);
    node_unlink(node);
    spinlock_release(&fs_lock, irq);

    node_free(node);
    return 0;
}

int fs_rename(const char *from, const char *to)
{
    struct fs_node *node = fs_lookup(from);
    if (!node || node == root_node) {
        return -QE_NOENT;
    }
    if (fs_lookup(to)) {
        return -QE_EXIST;
    }

    char            name[FS_NAME_MAX];
    struct fs_node *parent = resolve_parent(to, name, sizeof(name));
    if (!parent || parent->type != FS_DIR) {
        return -QE_NOENT;
    }

    bool_t irq = spinlock_acquire(&fs_lock);
    node_unlink(node);
    node->sibling = NULL;
    strlcpy(node->name, name, sizeof(node->name));
    node_link(parent, node);
    node->modified = rtc_unix_time();
    spinlock_release(&fs_lock, irq);

    return 0;
}

struct fs_node *fs_register_node(const char *dir, const char *name,
                                 fs_node_type_t type, const struct fs_ops *ops,
                                 void *backing)
{
    struct fs_node *parent = fs_lookup(dir);
    if (!parent || parent->type != FS_DIR) {
        KLOG_ERR("vfs", "cannot register %s/%s: no such directory", dir, name);
        return NULL;
    }
    if (find_child(parent, name)) {
        return NULL;
    }

    struct fs_node *node = node_alloc(name, type, 0644);
    if (!node) {
        return NULL;
    }
    node->ops     = ops;
    node->backing = backing;

    bool_t irq = spinlock_acquire(&fs_lock);
    node_link(parent, node);
    spinlock_release(&fs_lock, irq);

    return node;
}

bool_t fs_can_access(struct fs_node *node, uint32_t want_flags)
{
    struct task *task = sched_current();

    /* Root, and the kernel before the scheduler starts, bypass the checks. */
    if (!task || task->uid == 0) {
        return true;
    }

    uint32_t perms = node->permissions;
    uint32_t shift = 0;   /* other */

    if (node->uid == task->uid) {
        shift = 6;        /* owner */
    } else if (node->gid == task->gid) {
        shift = 3;        /* group */
    }

    bool_t can_read  = (perms >> shift) & 4;
    bool_t can_write = (perms >> shift) & 2;

    if ((want_flags & O_WRONLY) && !can_write) {
        return false;
    }
    if ((want_flags & O_RDONLY) && !can_read) {
        return false;
    }
    return true;
}

/* Grow an in-memory file so `needed` bytes fit. */
static int ensure_capacity(struct fs_node *node, uint64_t needed)
{
    if (needed <= node->capacity) {
        return 0;
    }

    uint64_t capacity = node->capacity ? node->capacity : 256;
    while (capacity < needed) {
        capacity *= 2;
    }

    uint8_t *data = kmalloc((size_t)capacity);
    if (!data) {
        return -QE_NOMEM;
    }
    if (node->data) {
        memcpy(data, node->data, (size_t)node->size);
        kfree(node->data);
    }
    memset(data + node->size, 0, (size_t)(capacity - node->size));

    node->data     = data;
    node->capacity = capacity;
    return 0;
}

struct file *fs_open(const char *path, uint32_t flags)
{
    struct fs_node *node = fs_lookup(path);

    if (!node) {
        if (!(flags & O_CREATE)) {
            return NULL;
        }
        node = fs_create(path, FS_FILE, 0644);
        if (!node) {
            return NULL;
        }
    } else if ((flags & O_EXCL) && (flags & O_CREATE)) {
        return NULL;
    }

    if ((flags & O_DIR) && node->type != FS_DIR) {
        return NULL;
    }
    if (node->type == FS_DIR && (flags & O_WRONLY)) {
        return NULL;
    }
    if (!fs_can_access(node, flags)) {
        return NULL;
    }

    if (node->ops && node->ops->open) {
        if (node->ops->open(node, flags) != 0) {
            return NULL;
        }
    }

    if ((flags & O_TRUNC) && node->type == FS_FILE) {
        node->size = 0;
    }

    struct file *file = kzalloc(sizeof(struct file));
    if (!file) {
        return NULL;
    }

    file->node   = node;
    file->flags  = flags;
    file->refs   = 1;
    file->offset = (flags & O_APPEND) ? node->size : 0;

    node->open_count++;
    node->accessed = rtc_unix_time();
    open_file_count++;

    return file;
}

void fs_close(struct file *file)
{
    if (!file) {
        return;
    }
    if (--file->refs > 0) {
        return;
    }

    if (file->node) {
        if (file->node->ops && file->node->ops->close) {
            file->node->ops->close(file->node);
        }
        if (file->node->open_count) {
            file->node->open_count--;
        }
    }
    if (open_file_count) {
        open_file_count--;
    }
    kfree(file);
}

ssize_t fs_read(struct file *file, void *buf, size_t len)
{
    if (!file || !file->node) {
        return -QE_BADF;
    }
    if ((file->flags & O_RDWR) == O_WRONLY) {
        return -QE_BADF;
    }

    struct fs_node *node = file->node;

    if (node->ops && node->ops->refresh) {
        node->ops->refresh(node);
    }
    if (node->ops && node->ops->read) {
        ssize_t got = node->ops->read(node, file->offset, buf, len);
        if (got > 0) {
            file->offset += (uint64_t)got;
        }
        return got;
    }
    if (node->type == FS_DIR) {
        return -QE_ISDIR;
    }

    if (file->offset >= node->size) {
        return 0;
    }
    size_t count = MIN(len, (size_t)(node->size - file->offset));
    memcpy(buf, node->data + file->offset, count);
    file->offset += count;
    node->accessed = rtc_unix_time();
    return (ssize_t)count;
}

ssize_t fs_write(struct file *file, const void *buf, size_t len)
{
    if (!file || !file->node) {
        return -QE_BADF;
    }
    if (!(file->flags & (O_WRONLY | O_RDWR))) {
        return -QE_BADF;
    }

    struct fs_node *node = file->node;

    if (node->ops && node->ops->write) {
        ssize_t written = node->ops->write(node, file->offset, buf, len);
        if (written > 0) {
            file->offset += (uint64_t)written;
        }
        return written;
    }
    if (node->type == FS_DIR) {
        return -QE_ISDIR;
    }

    if (file->flags & O_APPEND) {
        file->offset = node->size;
    }

    int error = ensure_capacity(node, file->offset + len);
    if (error != 0) {
        return error;
    }

    memcpy(node->data + file->offset, buf, len);
    file->offset += len;
    if (file->offset > node->size) {
        node->size = file->offset;
    }
    node->modified = rtc_unix_time();
    return (ssize_t)len;
}

int64_t fs_seek(struct file *file, int64_t offset, int origin)
{
    if (!file || !file->node) {
        return -QE_BADF;
    }

    int64_t base;
    switch (origin) {
    case SEEK_SET: base = 0; break;
    case SEEK_CUR: base = (int64_t)file->offset; break;
    case SEEK_END: base = (int64_t)file->node->size; break;
    default:       return -QE_INVAL;
    }

    int64_t target = base + offset;
    if (target < 0) {
        return -QE_INVAL;
    }
    file->offset = (uint64_t)target;
    return target;
}

int fs_stat_node(struct fs_node *node, struct fs_stat *out)
{
    if (!node || !out) {
        return -QE_INVAL;
    }
    if (node->ops && node->ops->refresh) {
        node->ops->refresh(node);
    }

    out->size        = node->size;
    out->type        = (uint32_t)node->type;
    out->permissions = node->permissions;
    out->uid         = node->uid;
    out->gid         = node->gid;
    out->created     = node->created;
    out->modified    = node->modified;
    out->inode       = node->inode;
    return 0;
}

int fs_stat(const char *path, struct fs_stat *out)
{
    struct fs_node *node = fs_lookup(path);
    if (!node) {
        return -QE_NOENT;
    }
    return fs_stat_node(node, out);
}

int fs_truncate(const char *path, uint64_t size)
{
    struct fs_node *node = fs_lookup(path);
    if (!node) {
        return -QE_NOENT;
    }
    if (node->type != FS_FILE) {
        return -QE_ISDIR;
    }
    if (node->ops && node->ops->truncate) {
        return node->ops->truncate(node, size);
    }
    if (size > node->size) {
        int error = ensure_capacity(node, size);
        if (error) {
            return error;
        }
        memset(node->data + node->size, 0, (size_t)(size - node->size));
    }
    node->size     = size;
    node->modified = rtc_unix_time();
    return 0;
}

int fs_readdir(struct fs_node *dir, int index, struct fs_dirent *out)
{
    if (!dir || dir->type != FS_DIR || index < 0) {
        return -QE_NOTDIR;
    }

    int i = 0;
    for (struct fs_node *child = dir->children; child; child = child->sibling) {
        if (i == index) {
            strlcpy(out->name, child->name, sizeof(out->name));
            out->type = (uint32_t)child->type;
            if (child->ops && child->ops->refresh) {
                child->ops->refresh(child);
            }
            out->size = child->size;
            return 0;
        }
        i++;
    }
    return -QE_NOENT;
}

int fs_readdir_path(const char *path, int index, struct fs_dirent *out)
{
    return fs_readdir(fs_lookup(path), index, out);
}

int fs_read_file(const char *path, void *buf, size_t max, size_t *out_len)
{
    struct file *file = fs_open(path, O_RDONLY);
    if (!file) {
        return -QE_NOENT;
    }

    ssize_t got = fs_read(file, buf, max);
    fs_close(file);

    if (got < 0) {
        return (int)got;
    }
    if (out_len) {
        *out_len = (size_t)got;
    }
    return 0;
}

int fs_write_file(const char *path, const void *buf, size_t len)
{
    struct file *file = fs_open(path, O_WRONLY | O_CREATE | O_TRUNC);
    if (!file) {
        return -QE_NOENT;
    }

    ssize_t written = fs_write(file, buf, len);
    fs_close(file);
    return (written < 0) ? (int)written : 0;
}

static void count_tree(struct fs_node *node, struct fs_statistics *stats)
{
    stats->nodes++;
    switch (node->type) {
    case FS_DIR:
        stats->directories++;
        break;
    case FS_DEV:
        stats->devices++;
        break;
    default:
        stats->files++;
        stats->total_bytes += node->size;
        break;
    }
    for (struct fs_node *child = node->children; child; child = child->sibling) {
        count_tree(child, stats);
    }
}

void fs_get_statistics(struct fs_statistics *out)
{
    memset(out, 0, sizeof(*out));
    if (root_node) {
        count_tree(root_node, out);
    }
    out->open_files = open_file_count;
}
