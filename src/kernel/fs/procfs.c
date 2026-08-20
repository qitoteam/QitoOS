/*
 * QitoOS - process/system information filesystem
 *
 * Each node under /proc regenerates its contents on demand from live kernel
 * state, which means `cat /proc/meminfo` always shows current numbers and the
 * shells get their system information through the ordinary file API.
 */

#include <kernel/fs.h>
#include <kernel/mm.h>
#include <kernel/log.h>
#include <kernel/string.h>
#include <kernel/printf.h>
#include <kernel/sched.h>
#include <kernel/time.h>
#include <kernel/cpu.h>
#include <kernel/irq.h>
#include <kernel/input.h>
#include <kernel/version.h>
#include <kernel/fb.h>
#include <kernel/net.h>

#define PROC_BUFFER 8192

/* Builder used by the generators, so they cannot overrun the node buffer. */
struct proc_writer {
    char  *buf;
    size_t size;
    size_t pos;
};

static void pw_printf(struct proc_writer *w, const char *fmt, ...) PRINTF_FMT(2, 3);

static void pw_printf(struct proc_writer *w, const char *fmt, ...)
{
    if (w->pos >= w->size) {
        return;
    }
    va_list ap;
    va_start(ap, fmt);
    int n = vsnprintf(w->buf + w->pos, w->size - w->pos, fmt, ap);
    va_end(ap);

    if (n > 0) {
        w->pos += MIN((size_t)n, w->size - w->pos - 1);
    }
}

typedef void (*proc_generator)(struct proc_writer *w);

/* Human-readable byte counts. */
static void format_size(uint64_t bytes, char *out, size_t size)
{
    if (bytes >= 1024ull * 1024 * 1024) {
        snprintf(out, size, "%llu.%llu GiB",
                 (unsigned long long)(bytes / (1024ull * 1024 * 1024)),
                 (unsigned long long)((bytes % (1024ull * 1024 * 1024)) /
                                      (1024ull * 1024 * 102)));
    } else if (bytes >= 1024 * 1024) {
        snprintf(out, size, "%llu MiB", (unsigned long long)(bytes / (1024 * 1024)));
    } else if (bytes >= 1024) {
        snprintf(out, size, "%llu KiB", (unsigned long long)(bytes / 1024));
    } else {
        snprintf(out, size, "%llu B", (unsigned long long)bytes);
    }
}

/* --- generators ------------------------------------------------------- */

static void gen_version(struct proc_writer *w)
{
    pw_printf(w, "%s %s (%s)\n", QITO_PROJECT_NAME, QITO_VERSION_STRING,
              QITO_CODENAME);
    pw_printf(w, "architecture: x86_64\n");
    pw_printf(w, "build: %s\n", QITO_BUILD_ID);
    pw_printf(w, "built: %s\n", QITO_BUILD_DATE);
    pw_printf(w, "maintainer: %s\n", QITO_MAINTAINER);
    pw_printf(w, "url: %s\n", QITO_PROJECT_URL);
}

static void gen_meminfo(struct proc_writer *w)
{
    struct heap_stats heap;
    heap_get_stats(&heap);

    pw_printf(w, "MemTotal:      %12llu kB\n",
              (unsigned long long)(pmm_total_bytes() / 1024));
    pw_printf(w, "MemFree:       %12llu kB\n",
              (unsigned long long)(pmm_free_bytes() / 1024));
    pw_printf(w, "MemUsed:       %12llu kB\n",
              (unsigned long long)(pmm_used_bytes() / 1024));
    pw_printf(w, "Reserved:      %12llu kB\n",
              (unsigned long long)(pmm_reserved_bytes() / 1024));
    pw_printf(w, "HeapTotal:     %12llu kB\n",
              (unsigned long long)(heap.total_bytes / 1024));
    pw_printf(w, "HeapUsed:      %12llu kB\n",
              (unsigned long long)(heap.used_bytes / 1024));
    pw_printf(w, "HeapFree:      %12llu kB\n",
              (unsigned long long)(heap.free_bytes / 1024));
    pw_printf(w, "HeapBlocks:    %12llu\n", (unsigned long long)heap.block_count);
    pw_printf(w, "Allocations:   %12llu\n", (unsigned long long)heap.allocations);
    pw_printf(w, "Frees:         %12llu\n", (unsigned long long)heap.frees);
    pw_printf(w, "PageSize:      %12llu\n", (unsigned long long)PAGE_SIZE);
}

static void gen_cpuinfo(struct proc_writer *w)
{
    const struct cpu_info *cpu = cpu_get_info();

    pw_printf(w, "processor       : 0\n");
    pw_printf(w, "vendor_id       : %s\n", cpu->vendor);
    pw_printf(w, "model name      : %s\n", cpu->brand);
    pw_printf(w, "cpu family      : %u\n", cpu->family);
    pw_printf(w, "model           : %u\n", cpu->model);
    pw_printf(w, "stepping        : %u\n", cpu->stepping);
    pw_printf(w, "cpu MHz         : %llu\n",
              (unsigned long long)(time_cpu_khz() / 1000));
    pw_printf(w, "address sizes   : %u bits physical, %u bits virtual\n",
              cpu->phys_addr_bits, cpu->virt_addr_bits);
    pw_printf(w, "flags           :");
    if (cpu->has_sse)       pw_printf(w, " sse");
    if (cpu->has_sse2)      pw_printf(w, " sse2");
    if (cpu->has_apic)      pw_printf(w, " apic");
    if (cpu->has_nx)        pw_printf(w, " nx");
    if (cpu->has_tsc)       pw_printf(w, " tsc");
    if (cpu->has_msr)       pw_printf(w, " msr");
    if (cpu->has_long_mode) pw_printf(w, " lm");
    pw_printf(w, "\n");
}

static void gen_uptime(struct proc_writer *w)
{
    uint64_t ms = time_uptime_ms();

    pw_printf(w, "%llu.%02llu\n", (unsigned long long)(ms / 1000),
              (unsigned long long)((ms % 1000) / 10));
}

static void gen_tasks(struct proc_writer *w)
{
    static struct task snapshot[MAX_TASKS];
    int count = sched_snapshot(snapshot, MAX_TASKS);

    pw_printf(w, "%5s %5s %-16s %-9s %-9s %8s %6s\n", "PID", "PPID", "NAME",
              "STATE", "PRIORITY", "TICKS", "UID");
    for (int i = 0; i < count; i++) {
        struct task *t = &snapshot[i];
        pw_printf(w, "%5d %5d %-16s %-9s %-9s %8llu %6u\n", t->pid, t->ppid,
                  t->name, sched_state_name(t->state),
                  sched_priority_name(t->priority),
                  (unsigned long long)t->ticks_total, t->uid);
    }
    pw_printf(w, "\ntasks: %d, context switches: %llu\n", count,
              (unsigned long long)sched_context_switches());
}

static void gen_interrupts(struct proc_writer *w)
{
    pw_printf(w, "%4s %12s  %s\n", "IRQ", "COUNT", "HANDLER");
    for (uint8_t irq = 0; irq < IRQ_COUNT; irq++) {
        uint64_t count = irq_get_count(irq);
        const char *name = irq_get_name(irq);
        if (count == 0 && strcmp(name, "-") == 0) {
            continue;
        }
        pw_printf(w, "%4u %12llu  %s\n", irq, (unsigned long long)count, name);
    }
    pw_printf(w, "\ntotal interrupts: %llu\n",
              (unsigned long long)interrupt_total_count());
}

static void gen_filesystems(struct proc_writer *w)
{
    struct fs_statistics stats;
    fs_get_statistics(&stats);

    char size[32];
    format_size(stats.total_bytes, size, sizeof(size));

    pw_printf(w, "filesystem: qitofs (in-memory root)\n");
    pw_printf(w, "nodes:       %llu\n", (unsigned long long)stats.nodes);
    pw_printf(w, "files:       %llu\n", (unsigned long long)stats.files);
    pw_printf(w, "directories: %llu\n", (unsigned long long)stats.directories);
    pw_printf(w, "devices:     %llu\n", (unsigned long long)stats.devices);
    pw_printf(w, "data:        %s\n", size);
    pw_printf(w, "open files:  %llu\n", (unsigned long long)stats.open_files);
}

static void gen_stat(struct proc_writer *w)
{
    struct log_stats logs;
    log_get_stats(&logs);

    pw_printf(w, "uptime_ms         %llu\n", (unsigned long long)time_uptime_ms());
    pw_printf(w, "context_switches  %llu\n",
              (unsigned long long)sched_context_switches());
    pw_printf(w, "interrupts        %llu\n",
              (unsigned long long)interrupt_total_count());
    pw_printf(w, "timer_ticks       %llu\n", (unsigned long long)time_ticks());
    pw_printf(w, "input_events      %llu\n",
              (unsigned long long)input_event_count());
    pw_printf(w, "keys_pressed      %llu\n",
              (unsigned long long)keyboard_key_count());
    pw_printf(w, "mouse_packets     %llu\n",
              (unsigned long long)mouse_packet_count());
    pw_printf(w, "log_records       %llu\n", (unsigned long long)logs.records);
    pw_printf(w, "tasks             %d\n", sched_task_count());
    if (fb_available()) {
        const struct fb_info *fb = fb_get_info();
        pw_printf(w, "frames_presented  %llu\n", (unsigned long long)fb->frames);
    }
}

static void gen_display(struct proc_writer *w)
{
    if (!fb_available()) {
        pw_printf(w, "no framebuffer available\n");
        return;
    }
    const struct fb_info *fb = fb_get_info();
    pw_printf(w, "resolution: %dx%d\n", fb->width, fb->height);
    pw_printf(w, "depth:      %d bpp\n", fb->bpp);
    pw_printf(w, "pitch:      %d pixels\n", fb->pitch);
    pw_printf(w, "address:    %p\n", (void *)fb->front);
    pw_printf(w, "frames:     %llu\n", (unsigned long long)fb->frames);
}

/* --- node plumbing ---------------------------------------------------- */

struct proc_node {
    proc_generator generate;
};

static int proc_refresh(struct fs_node *node)
{
    struct proc_node *proc = (struct proc_node *)node->backing;

    if (!proc || !proc->generate) {
        return 0;
    }
    if (!node->data) {
        node->data = kmalloc(PROC_BUFFER);
        if (!node->data) {
            return -1;
        }
        node->capacity = PROC_BUFFER;
    }

    struct proc_writer writer = {(char *)node->data, PROC_BUFFER, 0};
    proc->generate(&writer);

    node->data[writer.pos] = '\0';
    node->size             = writer.pos;
    return 0;
}

static ssize_t proc_read(struct fs_node *node, uint64_t offset, void *buf,
                         size_t len)
{
    if (offset >= node->size) {
        return 0;
    }
    size_t count = MIN(len, (size_t)(node->size - offset));
    memcpy(buf, node->data + offset, count);
    return (ssize_t)count;
}

static const struct fs_ops proc_ops = {
    .read    = proc_read,
    .refresh = proc_refresh,
};

static void add_proc_file(const char *name, proc_generator generate)
{
    struct proc_node *proc = kzalloc(sizeof(struct proc_node));
    if (!proc) {
        return;
    }
    proc->generate = generate;

    struct fs_node *node = fs_register_node("/proc", name, FS_FILE, &proc_ops, proc);
    if (node) {
        node->permissions = 0444;
    }
}

void procfs_init(void)
{
    add_proc_file("version", gen_version);
    add_proc_file("meminfo", gen_meminfo);
    add_proc_file("cpuinfo", gen_cpuinfo);
    add_proc_file("uptime", gen_uptime);
    add_proc_file("tasks", gen_tasks);
    add_proc_file("interrupts", gen_interrupts);
    add_proc_file("filesystems", gen_filesystems);
    add_proc_file("stat", gen_stat);
    add_proc_file("display", gen_display);

    KLOG_INFO("procfs", "/proc populated with live kernel state");
}
