/*
 * Qira OS - system call dispatch
 *
 * Userspace enters the kernel through `int 0x80` with the call number in RAX
 * and up to six arguments in RDI, RSI, RDX, R10, R8, R9. The result is
 * returned in RAX; negative values are error codes from <kernel/syscall.h>.
 */

#define QIRA_KERNEL 1

#include <kernel/syscall.h>
#include <kernel/cpu.h>
#include <kernel/sched.h>
#include <kernel/fs.h>
#include <kernel/mm.h>
#include <kernel/log.h>
#include <kernel/string.h>
#include <kernel/time.h>
#include <kernel/console.h>
#include <kernel/version.h>
#include <kernel/sysinfo.h>

static uint64_t call_counts[SYS_MAX];

const char *qira_strerror(int error)
{
    if (error < 0) {
        error = -error;
    }
    switch (error) {
    case QE_OK:          return "success";
    case QE_PERM:        return "operation not permitted";
    case QE_NOENT:       return "no such file or directory";
    case QE_IO:          return "input/output error";
    case QE_BADF:        return "bad file descriptor";
    case QE_AGAIN:       return "try again";
    case QE_NOMEM:       return "out of memory";
    case QE_ACCESS:      return "permission denied";
    case QE_EXIST:       return "file exists";
    case QE_NOTDIR:      return "not a directory";
    case QE_ISDIR:       return "is a directory";
    case QE_INVAL:       return "invalid argument";
    case QE_NFILE:       return "too many open files in system";
    case QE_MFILE:       return "too many open files";
    case QE_FBIG:        return "file too large";
    case QE_NOSPC:       return "no space left on device";
    case QE_SPIPE:       return "illegal seek";
    case QE_ROFS:        return "read-only filesystem";
    case QE_NAMETOOLONG: return "file name too long";
    case QE_NOSYS:       return "function not implemented";
    case QE_NOTEMPTY:    return "directory not empty";
    default:             return "unknown error";
    }
}

/*
 * Validate a pointer supplied by userspace.
 *
 * Kernel tasks are trusted. For user tasks the address must lie in the lower
 * half of the address space and be mapped.
 */
static bool_t user_pointer_ok(const void *ptr, size_t len)
{
    struct task *task = sched_current();

    if (!ptr) {
        return false;
    }
    if (!task || task->is_kernel) {
        return true;
    }

    uint64_t start = (uint64_t)(uintptr_t)ptr;
    uint64_t end   = start + len;

    if (end < start || start >= 0x0000800000000000ull ||
        end > 0x0000800000000000ull) {
        return false;
    }
    for (uint64_t page = PAGE_ALIGN_DOWN(start); page < end; page += PAGE_SIZE) {
        if (!vmm_resolve(task->space, page)) {
            return false;
        }
    }
    return true;
}

/* --- file descriptor helpers ---------------------------------------- */

static int fd_alloc(struct task *task, struct file *file)
{
    for (int fd = 0; fd < MAX_FDS; fd++) {
        if (!task->fds[fd]) {
            task->fds[fd] = file;
            return fd;
        }
    }
    return -QE_MFILE;
}

static struct file *fd_get(struct task *task, int fd)
{
    if (fd < 0 || fd >= MAX_FDS) {
        return NULL;
    }
    return task->fds[fd];
}

/* --- individual system calls ---------------------------------------- */

static int64_t sys_open(const char *path, uint32_t flags)
{
    struct task *task = sched_current();
    if (!task || !user_pointer_ok(path, 1)) {
        return -QE_INVAL;
    }

    char resolved[FS_PATH_MAX];
    fs_resolve_path(task->cwd, path, resolved, sizeof(resolved));

    struct file *file = fs_open(resolved, flags);
    if (!file) {
        return -QE_NOENT;
    }

    int fd = fd_alloc(task, file);
    if (fd < 0) {
        fs_close(file);
        return fd;
    }
    return fd;
}

static int64_t sys_close(int fd)
{
    struct task *task = sched_current();
    struct file *file = fd_get(task, fd);

    if (!file) {
        return -QE_BADF;
    }
    fs_close(file);
    task->fds[fd] = NULL;
    return 0;
}

static int64_t sys_read(int fd, void *buf, size_t len)
{
    struct task *task = sched_current();

    if (!user_pointer_ok(buf, len)) {
        return -QE_INVAL;
    }

    /* fd 0 is the console. */
    if (fd == 0) {
        return console_read(buf, len);
    }

    struct file *file = fd_get(task, fd);
    if (!file) {
        return -QE_BADF;
    }
    return fs_read(file, buf, len);
}

static int64_t sys_write(int fd, const void *buf, size_t len)
{
    struct task *task = sched_current();

    if (!user_pointer_ok(buf, len)) {
        return -QE_INVAL;
    }

    /* fd 1 and 2 are the console. */
    if (fd == 1 || fd == 2) {
        console_write((const char *)buf, len);
        return (int64_t)len;
    }

    struct file *file = fd_get(task, fd);
    if (!file) {
        return -QE_BADF;
    }
    return fs_write(file, buf, len);
}

static int64_t sys_seek(int fd, int64_t offset, int origin)
{
    struct file *file = fd_get(sched_current(), fd);
    if (!file) {
        return -QE_BADF;
    }
    return fs_seek(file, offset, origin);
}

static int64_t sys_stat(const char *path, struct fs_stat *out)
{
    struct task *task = sched_current();

    if (!user_pointer_ok(path, 1) || !user_pointer_ok(out, sizeof(*out))) {
        return -QE_INVAL;
    }

    char resolved[FS_PATH_MAX];
    fs_resolve_path(task->cwd, path, resolved, sizeof(resolved));
    return fs_stat(resolved, out);
}

static int64_t sys_readdir(const char *path, int index, struct fs_dirent *out)
{
    struct task *task = sched_current();

    if (!user_pointer_ok(path, 1) || !user_pointer_ok(out, sizeof(*out))) {
        return -QE_INVAL;
    }

    char resolved[FS_PATH_MAX];
    fs_resolve_path(task->cwd, path, resolved, sizeof(resolved));
    return fs_readdir_path(resolved, index, out);
}

static int64_t sys_chdir(const char *path)
{
    struct task *task = sched_current();

    if (!task || !user_pointer_ok(path, 1)) {
        return -QE_INVAL;
    }

    char resolved[FS_PATH_MAX];
    fs_resolve_path(task->cwd, path, resolved, sizeof(resolved));

    struct fs_node *node = fs_lookup(resolved);
    if (!node) {
        return -QE_NOENT;
    }
    if (node->type != FS_DIR) {
        return -QE_NOTDIR;
    }
    strlcpy(task->cwd, resolved, sizeof(task->cwd));
    return 0;
}

static int64_t sys_getcwd(char *buf, size_t len)
{
    struct task *task = sched_current();

    if (!task || !user_pointer_ok(buf, len)) {
        return -QE_INVAL;
    }
    strlcpy(buf, task->cwd, len);
    return (int64_t)strlen(buf);
}

static int64_t sys_sysinfo(struct qira_sysinfo *out)
{
    if (!user_pointer_ok(out, sizeof(*out))) {
        return -QE_INVAL;
    }

    memset(out, 0, sizeof(*out));
    strlcpy(out->version, QIRA_VERSION_STRING, sizeof(out->version));
    strlcpy(out->codename, QIRA_CODENAME, sizeof(out->codename));
    strlcpy(out->arch, "x86_64", sizeof(out->arch));

    const struct cpu_info *cpu = cpu_get_info();
    strlcpy(out->cpu_model, cpu->brand, sizeof(out->cpu_model));
    strlcpy(out->cpu_vendor, cpu->vendor, sizeof(out->cpu_vendor));

    out->uptime_ms    = time_uptime_ms();
    out->total_memory = pmm_total_bytes();
    out->free_memory  = pmm_free_bytes();
    out->used_memory  = pmm_used_bytes();
    out->task_count   = (uint32_t)sched_task_count();
    out->cpu_khz      = time_cpu_khz();
    out->unix_time    = rtc_unix_time();
    return 0;
}

static int64_t sys_dmesg(char *buf, size_t len, size_t offset)
{
    if (!user_pointer_ok(buf, len)) {
        return -QE_INVAL;
    }
    return (int64_t)log_read(buf, len, offset);
}

/* --- dispatcher ------------------------------------------------------ */

void syscall_init(void)
{
    memset(call_counts, 0, sizeof(call_counts));
    KLOG_INFO("syscall", "%d system calls registered", SYS_MAX);
}

uint64_t syscall_count(uint32_t number)
{
    return (number < SYS_MAX) ? call_counts[number] : 0;
}

void syscall_dispatch(struct interrupt_frame *frame)
{
    uint64_t number = frame->rax;
    uint64_t a0     = frame->rdi;
    uint64_t a1     = frame->rsi;
    uint64_t a2     = frame->rdx;
    uint64_t a3     = frame->r10;

    int64_t result;

    if (number < SYS_MAX) {
        call_counts[number]++;
    }

    switch (number) {
    case SYS_EXIT:
        sched_exit((int)a0);
        result = 0;
        break;

    case SYS_YIELD:
        sched_yield();
        result = 0;
        break;

    case SYS_GETPID:
        result = sched_current_pid();
        break;

    case SYS_GETPPID: {
        struct task *task = sched_current();
        result = task ? task->ppid : 0;
        break;
    }

    case SYS_SLEEP_MS:
        sched_sleep_ms(a0);
        result = 0;
        break;

    case SYS_KILL:
        result = sched_kill((int)a0, (int)a1);
        break;

    case SYS_OPEN:
        result = sys_open((const char *)a0, (uint32_t)a1);
        break;

    case SYS_CLOSE:
        result = sys_close((int)a0);
        break;

    case SYS_READ:
        result = sys_read((int)a0, (void *)a1, (size_t)a2);
        break;

    case SYS_WRITE:
        result = sys_write((int)a0, (const void *)a1, (size_t)a2);
        break;

    case SYS_SEEK:
        result = sys_seek((int)a0, (int64_t)a1, (int)a2);
        break;

    case SYS_STAT:
        result = sys_stat((const char *)a0, (struct fs_stat *)a1);
        break;

    case SYS_READDIR:
        result = sys_readdir((const char *)a0, (int)a1, (struct fs_dirent *)a2);
        break;

    case SYS_MKDIR: {
        struct task *task = sched_current();
        char resolved[FS_PATH_MAX];
        if (!user_pointer_ok((const void *)a0, 1)) {
            result = -QE_INVAL;
            break;
        }
        fs_resolve_path(task ? task->cwd : "/", (const char *)a0, resolved,
                        sizeof(resolved));
        result = fs_mkdir(resolved, (uint32_t)a1);
        break;
    }

    case SYS_UNLINK: {
        struct task *task = sched_current();
        char resolved[FS_PATH_MAX];
        if (!user_pointer_ok((const void *)a0, 1)) {
            result = -QE_INVAL;
            break;
        }
        fs_resolve_path(task ? task->cwd : "/", (const char *)a0, resolved,
                        sizeof(resolved));
        result = fs_unlink(resolved);
        break;
    }

    case SYS_CHDIR:
        result = sys_chdir((const char *)a0);
        break;

    case SYS_GETCWD:
        result = sys_getcwd((char *)a0, (size_t)a1);
        break;

    case SYS_PUTS:
        if (!user_pointer_ok((const void *)a0, 1)) {
            result = -QE_INVAL;
            break;
        }
        console_write((const char *)a0, strlen((const char *)a0));
        result = 0;
        break;

    case SYS_GETCHAR:
        result = console_getchar();
        break;

    case SYS_UPTIME_MS:
        result = (int64_t)time_uptime_ms();
        break;

    case SYS_TIME:
        result = (int64_t)rtc_unix_time();
        break;

    case SYS_SYSINFO:
        result = sys_sysinfo((struct qira_sysinfo *)a0);
        break;

    case SYS_DMESG:
        result = sys_dmesg((char *)a0, (size_t)a1, (size_t)a2);
        break;

    default:
        KLOG_WARN("syscall", "unimplemented call %llu from task %d",
                  (unsigned long long)number, sched_current_pid());
        result = -QE_NOSYS;
        break;
    }

    UNUSED(a3);
    frame->rax = (uint64_t)result;
}
