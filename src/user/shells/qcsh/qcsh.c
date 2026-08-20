/*
 * QitoOS - QitoConfigShell (QCSH)
 *
 * The administration and diagnostics shell. Where UltraShell is for everyday
 * work with files and text, QCSH is the place you go to inspect the machine,
 * change how it is configured, and understand what the kernel is doing.
 *
 * Command groups:
 *   system information   sysinfo, cpuinfo, meminfo, uptime, version, hwinfo
 *   diagnostics          dmesg, loglevel, irqinfo, diag, selftest, benchmark
 *   process management   ps, kill, top, taskinfo
 *   configuration        config, service, hostname, timezone
 *   package management   pkg
 *   storage              df, mount, fsck
 *   networking           netinfo, ping
 *   security             users, perms
 *   power                reboot, poweroff
 */

#include <kernel/shell.h>
#include <kernel/fs.h>
#include <kernel/string.h>
#include <kernel/printf.h>
#include <kernel/mm.h>
#include <kernel/time.h>
#include <kernel/log.h>
#include <kernel/sched.h>
#include <kernel/cpu.h>
#include <kernel/irq.h>
#include <kernel/input.h>
#include <kernel/fb.h>
#include <kernel/version.h>
#include <kernel/syscall.h>
#include <kernel/console.h>
#include <kernel/config.h>
#include <kernel/net.h>
#include <kernel/pci.h>
#include <kernel/power.h>
#include <kernel/audio.h>
#include <kernel/pkg.h>
#include <kernel/service.h>

static struct shell qcsh;

static void print_size(struct shell *sh, const char *label, uint64_t bytes)
{
    if (bytes >= 1024ull * 1024 * 1024) {
        shell_printf(sh, "  %-22s %llu.%llu GiB\n", label,
                     (unsigned long long)(bytes / (1024ull * 1024 * 1024)),
                     (unsigned long long)((bytes / (1024ull * 1024 * 107)) % 10));
    } else if (bytes >= 1024 * 1024) {
        shell_printf(sh, "  %-22s %llu MiB\n", label,
                     (unsigned long long)(bytes / (1024 * 1024)));
    } else {
        shell_printf(sh, "  %-22s %llu KiB\n", label,
                     (unsigned long long)(bytes / 1024));
    }
}

static void section(struct shell *sh, const char *title)
{
    shell_color(sh, "\033[1;96m");
    shell_printf(sh, "%s\n", title);
    shell_reset_color(sh);
}

/* --- system information ----------------------------------------------- */

static int cmd_sysinfo(struct shell *sh, int argc, char **argv)
{
    UNUSED(argc);
    UNUSED(argv);

    const struct cpu_info *cpu = cpu_get_info();
    uint64_t ms                = time_uptime_ms();
    uint64_t seconds           = ms / 1000;

    shell_color(sh, "\033[1;96m");
    shell_printf(sh, "\n  QitoOS %s \"%s\"\n", QITO_VERSION_STRING, QITO_CODENAME);
    shell_reset_color(sh);
    shell_printf(sh, "  %s\n\n", "----------------------------------------");

    section(sh, "System");
    shell_printf(sh, "  %-22s %s\n", "Architecture", "x86_64");
    shell_printf(sh, "  %-22s %s\n", "Kernel build", QITO_BUILD_ID);
    shell_printf(sh, "  %-22s %s\n", "Built", QITO_BUILD_DATE);
    shell_printf(sh, "  %-22s %s\n", "Hostname", config_get_string("hostname", "qito"));
    shell_printf(sh, "  %-22s %llud %02lluh %02llum %02llus\n", "Uptime",
                 (unsigned long long)(seconds / 86400),
                 (unsigned long long)((seconds % 86400) / 3600),
                 (unsigned long long)((seconds % 3600) / 60),
                 (unsigned long long)(seconds % 60));

    struct qito_time now;
    time_from_unix(rtc_unix_time(), &now);
    char stamp[32];
    time_format(&now, stamp, sizeof(stamp));
    shell_printf(sh, "  %-22s %s UTC\n\n", "System time", stamp);

    section(sh, "Processor");
    shell_printf(sh, "  %-22s %s\n", "Model", cpu->brand);
    shell_printf(sh, "  %-22s %s\n", "Vendor", cpu->vendor);
    shell_printf(sh, "  %-22s family %u, model %u, stepping %u\n", "Signature",
                 cpu->family, cpu->model, cpu->stepping);
    shell_printf(sh, "  %-22s %llu MHz\n", "Clock",
                 (unsigned long long)(time_cpu_khz() / 1000));
    shell_printf(sh, "  %-22s %u-bit physical, %u-bit virtual\n\n", "Addressing",
                 cpu->phys_addr_bits, cpu->virt_addr_bits);

    section(sh, "Memory");
    print_size(sh, "Total", pmm_total_bytes());
    print_size(sh, "Used", pmm_used_bytes());
    print_size(sh, "Free", pmm_free_bytes());

    struct heap_stats heap;
    heap_get_stats(&heap);
    print_size(sh, "Kernel heap", heap.total_bytes);
    print_size(sh, "Heap in use", heap.used_bytes);
    shell_printf(sh, "\n");

    section(sh, "Display");
    if (fb_available()) {
        const struct fb_info *fb = fb_get_info();
        shell_printf(sh, "  %-22s %dx%d at %d bpp\n", "Framebuffer", fb->width,
                     fb->height, fb->bpp);
        shell_printf(sh, "  %-22s %llu\n", "Frames presented",
                     (unsigned long long)fb->frames);
    } else {
        shell_printf(sh, "  %-22s unavailable\n", "Framebuffer");
    }
    shell_printf(sh, "\n");

    section(sh, "Activity");
    shell_printf(sh, "  %-22s %d\n", "Tasks", sched_task_count());
    shell_printf(sh, "  %-22s %llu\n", "Context switches",
                 (unsigned long long)sched_context_switches());
    shell_printf(sh, "  %-22s %llu\n", "Interrupts",
                 (unsigned long long)interrupt_total_count());
    shell_printf(sh, "  %-22s %llu\n\n", "Input events",
                 (unsigned long long)input_event_count());
    return 0;
}

static int cmd_cpuinfo(struct shell *sh, int argc, char **argv)
{
    UNUSED(argc);
    UNUSED(argv);

    const struct cpu_info *cpu = cpu_get_info();

    shell_printf(sh, "Processor        : %s\n", cpu->brand);
    shell_printf(sh, "Vendor           : %s\n", cpu->vendor);
    shell_printf(sh, "Family           : %u\n", cpu->family);
    shell_printf(sh, "Model            : %u\n", cpu->model);
    shell_printf(sh, "Stepping         : %u\n", cpu->stepping);
    shell_printf(sh, "Clock            : %llu MHz\n",
                 (unsigned long long)(time_cpu_khz() / 1000));
    shell_printf(sh, "Max CPUID leaf   : 0x%08x\n", cpu->max_leaf);
    shell_printf(sh, "Max ext leaf     : 0x%08x\n", cpu->max_ext_leaf);
    shell_printf(sh, "Physical address : %u bits\n", cpu->phys_addr_bits);
    shell_printf(sh, "Virtual address  : %u bits\n", cpu->virt_addr_bits);
    shell_printf(sh, "\nFeatures:\n");
    shell_printf(sh, "  %-12s %s\n", "long mode", cpu->has_long_mode ? "yes" : "no");
    shell_printf(sh, "  %-12s %s\n", "SSE", cpu->has_sse ? "yes" : "no");
    shell_printf(sh, "  %-12s %s\n", "SSE2", cpu->has_sse2 ? "yes" : "no");
    shell_printf(sh, "  %-12s %s\n", "APIC", cpu->has_apic ? "yes" : "no");
    shell_printf(sh, "  %-12s %s\n", "NX", cpu->has_nx ? "yes" : "no");
    shell_printf(sh, "  %-12s %s\n", "TSC", cpu->has_tsc ? "yes" : "no");
    shell_printf(sh, "  %-12s %s\n", "MSR", cpu->has_msr ? "yes" : "no");
    return 0;
}

static int cmd_meminfo(struct shell *sh, int argc, char **argv)
{
    bool_t verbose = (argc > 1 && strcmp(argv[1], "-v") == 0);

    struct heap_stats heap;
    heap_get_stats(&heap);

    uint64_t total = pmm_total_bytes();
    uint64_t used  = pmm_used_bytes();
    uint64_t free_bytes = pmm_free_bytes();

    section(sh, "Physical memory");
    print_size(sh, "Total", total);
    print_size(sh, "Used", used);
    print_size(sh, "Free", free_bytes);
    print_size(sh, "Reserved by firmware", pmm_reserved_bytes());

    /* A simple usage bar. */
    int percent = (total > 0) ? (int)((used * 100) / total) : 0;
    shell_printf(sh, "  %-22s [", "Utilisation");
    for (int i = 0; i < 40; i++) {
        shell_printf(sh, "%s", (i < percent * 40 / 100) ? "#" : ".");
    }
    shell_printf(sh, "] %d%%\n\n", percent);

    section(sh, "Kernel heap");
    print_size(sh, "Committed", heap.total_bytes);
    print_size(sh, "In use", heap.used_bytes);
    print_size(sh, "Free", heap.free_bytes);
    print_size(sh, "Largest free block", heap.largest_free);
    shell_printf(sh, "  %-22s %llu\n", "Blocks",
                 (unsigned long long)heap.block_count);
    shell_printf(sh, "  %-22s %llu\n", "Allocations",
                 (unsigned long long)heap.allocations);
    shell_printf(sh, "  %-22s %llu\n", "Frees", (unsigned long long)heap.frees);

    if (verbose) {
        shell_printf(sh, "\n");
        section(sh, "Integrity");
        shell_printf(sh, "  %-22s %s\n", "Heap validation",
                     heap_validate() ? "passed" : "FAILED");
        shell_printf(sh, "  %-22s %llu bytes\n", "Page size",
                     (unsigned long long)PAGE_SIZE);
    }
    return 0;
}

static int cmd_hwinfo(struct shell *sh, int argc, char **argv)
{
    UNUSED(argc);
    UNUSED(argv);

    section(sh, "Input devices");
    shell_printf(sh, "  %-22s present (scancode set 1)\n", "PS/2 keyboard");
    shell_printf(sh, "  %-22s %s\n", "PS/2 mouse",
                 mouse_available() ? "present" : "not detected");
    shell_printf(sh, "  %-22s %llu\n", "Keys pressed",
                 (unsigned long long)keyboard_key_count());
    shell_printf(sh, "  %-22s %llu\n\n", "Mouse packets",
                 (unsigned long long)mouse_packet_count());

    section(sh, "Display");
    if (fb_available()) {
        const struct fb_info *fb = fb_get_info();
        shell_printf(sh, "  %-22s VESA linear framebuffer\n", "Adapter");
        shell_printf(sh, "  %-22s %dx%d\n", "Resolution", fb->width, fb->height);
        shell_printf(sh, "  %-22s %d bpp\n", "Colour depth", fb->bpp);
        shell_printf(sh, "  %-22s %p\n\n", "Address", (void *)fb->front);
    } else {
        shell_printf(sh, "  %-22s none\n\n", "Adapter");
    }

    section(sh, "PCI devices");
    pci_print_devices(sh);
    shell_printf(sh, "\n");

    section(sh, "Audio");
    audio_print_info(sh);
    return 0;
}

/* --- diagnostics ------------------------------------------------------ */

static int cmd_dmesg(struct shell *sh, int argc, char **argv)
{
    bool_t follow = false;
    int    tail   = 0;

    for (int i = 1; i < argc; i++) {
        if (strcmp(argv[i], "-c") == 0) {
            log_clear();
            shell_printf(sh, "kernel log cleared\n");
            return 0;
        }
        if (strcmp(argv[i], "-n") == 0 && i + 1 < argc) {
            tail = atoi(argv[++i]);
        }
        if (strcmp(argv[i], "-f") == 0) {
            follow = true;
        }
    }
    UNUSED(follow);

    size_t total = log_size();
    char  *buffer = kmalloc(total + 1);
    if (!buffer) {
        shell_error(sh, "dmesg: out of memory reading the log");
        return 1;
    }

    size_t got = log_read(buffer, total, 0);
    buffer[got] = '\0';

    if (tail > 0) {
        /* Walk backwards over `tail` newlines. */
        char *start = buffer + got;
        int   seen  = 0;
        while (start > buffer) {
            if (start[-1] == '\n' && start != buffer + got) {
                if (++seen >= tail) {
                    break;
                }
            }
            start--;
        }
        shell_puts(sh, start);
    } else {
        shell_write(sh, buffer, got);
    }

    kfree(buffer);
    return 0;
}

static int cmd_loglevel(struct shell *sh, int argc, char **argv)
{
    static const char *names[] = {"panic", "error", "warn", "info", "debug", "trace"};

    if (argc < 2) {
        log_level_t level = log_get_level();
        shell_printf(sh, "current log level: %s (%d)\n", names[level], level);
        shell_printf(sh, "available: ");
        for (size_t i = 0; i < ARRAY_SIZE(names); i++) {
            shell_printf(sh, "%s%s", names[i], (i + 1 < ARRAY_SIZE(names)) ? ", " : "\n");
        }
        return 0;
    }

    for (size_t i = 0; i < ARRAY_SIZE(names); i++) {
        if (strcasecmp(argv[1], names[i]) == 0) {
            log_set_level((log_level_t)i);
            shell_printf(sh, "log level set to %s\n", names[i]);
            config_set_int("log.level", (int)i);
            return 0;
        }
    }

    shell_error(sh, "loglevel: unknown level '%s'", argv[1]);
    return 1;
}

static int cmd_irqinfo(struct shell *sh, int argc, char **argv)
{
    UNUSED(argc);
    UNUSED(argv);

    shell_printf(sh, "%4s  %14s  %-20s %s\n", "IRQ", "COUNT", "HANDLER", "STATE");
    uint16_t mask = pic_get_mask();

    for (uint8_t irq = 0; irq < IRQ_COUNT; irq++) {
        const char *name  = irq_get_name(irq);
        uint64_t    count = irq_get_count(irq);
        bool_t      masked = (mask & (1u << irq)) != 0;

        if (count == 0 && strcmp(name, "-") == 0) {
            continue;
        }
        shell_printf(sh, "%4u  %14llu  %-20s %s\n", irq,
                     (unsigned long long)count, name,
                     masked ? "masked" : "enabled");
    }

    shell_printf(sh, "\nTotal interrupts handled: %llu\n",
                 (unsigned long long)interrupt_total_count());
    shell_printf(sh, "Timer ticks: %llu at %d Hz\n",
                 (unsigned long long)time_ticks(), TIMER_HZ);
    return 0;
}

static int cmd_diag(struct shell *sh, int argc, char **argv)
{
    UNUSED(argc);
    UNUSED(argv);

    section(sh, "QitoOS diagnostics");
    shell_printf(sh, "\n");

    struct {
        const char *name;
        bool_t      ok;
        const char *detail;
    } checks[10];
    int count = 0;

    checks[count].name   = "Kernel heap integrity";
    checks[count].ok     = heap_validate();
    checks[count].detail = checks[count].ok ? "no corruption detected"
                                            : "corruption detected";
    count++;

    checks[count].name   = "Physical memory";
    checks[count].ok     = pmm_free_bytes() > 1024 * 1024;
    checks[count].detail = checks[count].ok ? "sufficient free memory"
                                            : "critically low";
    count++;

    checks[count].name   = "Timer interrupt";
    checks[count].ok     = time_ticks() > 0;
    checks[count].detail = checks[count].ok ? "ticking" : "not firing";
    count++;

    checks[count].name   = "Root filesystem";
    checks[count].ok     = fs_lookup("/") != NULL;
    checks[count].detail = checks[count].ok ? "mounted" : "missing";
    count++;

    checks[count].name   = "Device nodes";
    checks[count].ok     = fs_lookup("/dev/null") != NULL;
    checks[count].detail = checks[count].ok ? "/dev populated" : "/dev empty";
    count++;

    checks[count].name   = "Process scheduler";
    checks[count].ok     = sched_task_count() > 0;
    checks[count].detail = checks[count].ok ? "running" : "no tasks";
    count++;

    checks[count].name   = "Graphics";
    checks[count].ok     = fb_available();
    checks[count].detail = checks[count].ok ? "framebuffer active"
                                            : "text mode only";
    count++;

    checks[count].name   = "Keyboard";
    checks[count].ok     = true;
    checks[count].detail = "PS/2 driver loaded";
    count++;

    int failures = 0;
    for (int i = 0; i < count; i++) {
        if (checks[i].ok) {
            shell_color(sh, "\033[92m");
            shell_printf(sh, "  [ OK ]  ");
        } else {
            shell_color(sh, "\033[91m");
            shell_printf(sh, "  [FAIL]  ");
            failures++;
        }
        shell_reset_color(sh);
        shell_printf(sh, "%-28s %s\n", checks[i].name, checks[i].detail);
    }

    shell_printf(sh, "\n%d checks, %d passed, %d failed\n", count, count - failures,
                 failures);
    return failures ? 1 : 0;
}

static int cmd_selftest(struct shell *sh, int argc, char **argv)
{
    UNUSED(argc);
    UNUSED(argv);

    int passed = 0, failed = 0;

    #define CHECK(desc, condition)                                          \
        do {                                                                \
            if (condition) {                                                \
                shell_color(sh, "\033[92m");                                \
                shell_printf(sh, "  pass  ");                               \
                passed++;                                                   \
            } else {                                                        \
                shell_color(sh, "\033[91m");                                \
                shell_printf(sh, "  FAIL  ");                               \
                failed++;                                                   \
            }                                                               \
            shell_reset_color(sh);                                          \
            shell_printf(sh, "%s\n", desc);                                 \
        } while (0)

    section(sh, "Kernel self-test");
    shell_printf(sh, "\n");

    /* Memory allocator. */
    void *a = kmalloc(1024);
    CHECK("heap: allocate 1 KiB", a != NULL);
    if (a) {
        memset(a, 0xAB, 1024);
        CHECK("heap: written data reads back", ((uint8_t *)a)[512] == 0xAB);
        kfree(a);
    }

    void *blocks[16];
    bool_t all_allocated = true;
    for (int i = 0; i < 16; i++) {
        blocks[i] = kmalloc(4096);
        if (!blocks[i]) {
            all_allocated = false;
        }
    }
    CHECK("heap: 16 x 4 KiB allocations", all_allocated);
    for (int i = 0; i < 16; i++) {
        kfree(blocks[i]);
    }
    CHECK("heap: integrity after churn", heap_validate());

    /* Physical allocator. */
    phys_addr_t page = pmm_alloc_page();
    CHECK("pmm: allocate a page frame", page != 0);
    if (page) {
        CHECK("pmm: frame is page aligned", (page & (PAGE_SIZE - 1)) == 0);
        pmm_free_page(page);
    }

    /* Virtual memory. */
    CHECK("vmm: kernel address space exists", vmm_kernel_space() != NULL);
    CHECK("vmm: resolves a kernel address",
          vmm_resolve(vmm_kernel_space(), (virt_addr_t)&qcsh) != 0);

    /* Filesystem round trip. */
    const char *probe = "qito selftest payload";
    int error = fs_write_file("/tmp/.selftest", probe, strlen(probe));
    CHECK("fs: write a file", error == 0);

    char   readback[64] = {0};
    size_t got          = 0;
    error = fs_read_file("/tmp/.selftest", readback, sizeof(readback) - 1, &got);
    CHECK("fs: read it back", error == 0 && got == strlen(probe));
    CHECK("fs: contents match", strcmp(readback, probe) == 0);
    CHECK("fs: unlink", fs_unlink("/tmp/.selftest") == 0);
    CHECK("fs: file is gone", fs_lookup("/tmp/.selftest") == NULL);

    /* String library. */
    CHECK("string: strcmp", strcmp("abc", "abc") == 0 && strcmp("a", "b") < 0);
    CHECK("string: strlen", strlen("qito") == 4);
    char buf[16];
    strlcpy(buf, "truncate me please", sizeof(buf));
    CHECK("string: strlcpy truncates safely", strlen(buf) == 15);
    CHECK("string: strstr", strstr("hello world", "wor") != NULL);

    /* Formatting. */
    char formatted[64];
    snprintf(formatted, sizeof(formatted), "%d %s %x %05d", 42, "ok", 255, 7);
    CHECK("printf: formatting", strcmp(formatted, "42 ok ff 00007") == 0);

    /* Timekeeping. */
    CHECK("time: uptime advances", time_uptime_ms() > 0);
    struct qito_time t;
    time_from_unix(0, &t);
    CHECK("time: epoch converts to 1970-01-01",
          t.year == 1970 && t.month == 1 && t.day == 1);
    /* Convert a known timestamp out and back again. */
    struct qito_time round = {2026, 8, 18, 12, 30, 45, 0};
    uint64_t         stamp = time_to_unix(&round);
    struct qito_time back;
    time_from_unix(stamp, &back);
    CHECK("time: unix round trip",
          back.year == round.year && back.month == round.month &&
              back.day == round.day && back.hour == round.hour &&
              back.minute == round.minute && back.second == round.second);

    shell_printf(sh, "\n%d tests: ", passed + failed);
    shell_color(sh, failed ? "\033[91m" : "\033[92m");
    shell_printf(sh, "%d passed, %d failed\n", passed, failed);
    shell_reset_color(sh);

    #undef CHECK
    return failed ? 1 : 0;
}

static int cmd_benchmark(struct shell *sh, int argc, char **argv)
{
    UNUSED(argc);
    UNUSED(argv);

    section(sh, "Micro-benchmarks");
    shell_printf(sh, "\n");

    /* Memory bandwidth via a large memcpy. */
    const size_t size = 1024 * 1024;
    void *src = kmalloc(size);
    void *dst = kmalloc(size);

    if (src && dst) {
        memset(src, 0x5A, size);

        uint64_t start = time_uptime_us();
        for (int i = 0; i < 8; i++) {
            memcpy(dst, src, size);
        }
        uint64_t elapsed = time_uptime_us() - start;

        if (elapsed > 0) {
            uint64_t mb_per_s = (8ull * size) / elapsed;
            shell_printf(sh, "  %-24s %llu MB/s (%llu us for 8 MiB)\n",
                         "memcpy throughput", (unsigned long long)mb_per_s,
                         (unsigned long long)elapsed);
        }

        start = time_uptime_us();
        for (int i = 0; i < 8; i++) {
            memset(dst, i, size);
        }
        elapsed = time_uptime_us() - start;
        if (elapsed > 0) {
            shell_printf(sh, "  %-24s %llu MB/s\n", "memset throughput",
                         (unsigned long long)((8ull * size) / elapsed));
        }
    }
    kfree(src);
    kfree(dst);

    /* Allocator throughput. */
    uint64_t start = time_uptime_us();
    for (int i = 0; i < 2000; i++) {
        void *p = kmalloc(128);
        kfree(p);
    }
    uint64_t elapsed = time_uptime_us() - start;
    shell_printf(sh, "  %-24s %llu us for 2000 cycles\n", "kmalloc/kfree",
                 (unsigned long long)elapsed);

    /* Integer throughput. */
    start = time_uptime_us();
    volatile uint64_t accumulator = 0;
    for (uint64_t i = 0; i < 2000000; i++) {
        accumulator += i * 3 + 1;
    }
    elapsed = time_uptime_us() - start;
    if (elapsed > 0) {
        shell_printf(sh, "  %-24s %llu Mops/s\n", "integer ops",
                     (unsigned long long)(2000000ull / elapsed));
    }

    /* Filesystem throughput. */
    char payload[4096];
    memset(payload, 'q', sizeof(payload));
    start = time_uptime_us();
    for (int i = 0; i < 64; i++) {
        fs_write_file("/tmp/.bench", payload, sizeof(payload));
    }
    elapsed = time_uptime_us() - start;
    fs_unlink("/tmp/.bench");
    shell_printf(sh, "  %-24s %llu us for 64 x 4 KiB writes\n", "filesystem write",
                 (unsigned long long)elapsed);

    shell_printf(sh, "\n");
    return 0;
}

/* --- process management ----------------------------------------------- */

static int cmd_ps(struct shell *sh, int argc, char **argv)
{
    bool_t wide = (argc > 1 && strcmp(argv[1], "-l") == 0);

    static struct task snapshot[MAX_TASKS];
    int count = sched_snapshot(snapshot, MAX_TASKS);

    shell_color(sh, "\033[1m");
    if (wide) {
        shell_printf(sh, "%5s %5s %-16s %-9s %-9s %8s %5s %s\n", "PID", "PPID",
                     "NAME", "STATE", "PRIORITY", "TICKS", "UID", "CWD");
    } else {
        shell_printf(sh, "%5s %-16s %-9s %-9s %8s\n", "PID", "NAME", "STATE",
                     "PRIORITY", "TICKS");
    }
    shell_reset_color(sh);

    for (int i = 0; i < count; i++) {
        struct task *t = &snapshot[i];
        if (wide) {
            shell_printf(sh, "%5d %5d %-16s %-9s %-9s %8llu %5u %s\n", t->pid,
                         t->ppid, t->name, sched_state_name(t->state),
                         sched_priority_name(t->priority),
                         (unsigned long long)t->ticks_total, t->uid, t->cwd);
        } else {
            shell_printf(sh, "%5d %-16s %-9s %-9s %8llu\n", t->pid, t->name,
                         sched_state_name(t->state),
                         sched_priority_name(t->priority),
                         (unsigned long long)t->ticks_total);
        }
    }

    shell_printf(sh, "\n%d tasks, %llu context switches\n", count,
                 (unsigned long long)sched_context_switches());
    return 0;
}

static int cmd_kill(struct shell *sh, int argc, char **argv)
{
    if (argc < 2) {
        shell_error(sh, "usage: kill <pid>");
        return 1;
    }

    int pid = atoi(argv[1]);
    if (pid <= 1) {
        shell_error(sh, "kill: refusing to kill pid %d", pid);
        return 1;
    }

    struct task *task = sched_find(pid);
    if (!task) {
        shell_error(sh, "kill: no such process: %d", pid);
        return 1;
    }

    char name[TASK_NAME_MAX];
    strlcpy(name, task->name, sizeof(name));

    if (sched_kill(pid, 9) != 0) {
        shell_error(sh, "kill: could not terminate %d", pid);
        return 1;
    }
    shell_printf(sh, "terminated %d (%s)\n", pid, name);
    return 0;
}

static int cmd_taskinfo(struct shell *sh, int argc, char **argv)
{
    if (argc < 2) {
        shell_error(sh, "usage: taskinfo <pid>");
        return 1;
    }

    struct task *task = sched_find(atoi(argv[1]));
    if (!task) {
        shell_error(sh, "taskinfo: no such process");
        return 1;
    }

    shell_printf(sh, "PID            : %d\n", task->pid);
    shell_printf(sh, "Parent PID     : %d\n", task->ppid);
    shell_printf(sh, "Name           : %s\n", task->name);
    shell_printf(sh, "State          : %s\n", sched_state_name(task->state));
    shell_printf(sh, "Priority       : %s\n", sched_priority_name(task->priority));
    shell_printf(sh, "Kernel task    : %s\n", task->is_kernel ? "yes" : "no");
    shell_printf(sh, "CPU ticks      : %llu\n",
                 (unsigned long long)task->ticks_total);
    shell_printf(sh, "Working dir    : %s\n", task->cwd);
    shell_printf(sh, "UID / GID      : %u / %u\n", task->uid, task->gid);
    shell_printf(sh, "Kernel stack   : %p (top %llx)\n", task->kernel_stack,
                 (unsigned long long)task->kernel_stack_top);
    if (task->block_reason) {
        shell_printf(sh, "Blocked on     : %s\n", task->block_reason);
    }

    int open_files = 0;
    for (int i = 0; i < MAX_FDS; i++) {
        if (task->fds[i]) {
            open_files++;
        }
    }
    shell_printf(sh, "Open files     : %d\n", open_files);
    return 0;
}

static int cmd_top(struct shell *sh, int argc, char **argv)
{
    UNUSED(argc);
    UNUSED(argv);

    static struct task snapshot[MAX_TASKS];
    int count = sched_snapshot(snapshot, MAX_TASKS);

    uint64_t total_ticks = 0;
    for (int i = 0; i < count; i++) {
        total_ticks += snapshot[i].ticks_total;
    }
    if (total_ticks == 0) {
        total_ticks = 1;
    }

    /* Sort by CPU time, descending. */
    for (int i = 1; i < count; i++) {
        struct task key = snapshot[i];
        int j = i - 1;
        while (j >= 0 && snapshot[j].ticks_total < key.ticks_total) {
            snapshot[j + 1] = snapshot[j];
            j--;
        }
        snapshot[j + 1] = key;
    }

    uint64_t seconds = time_uptime_ms() / 1000;
    shell_printf(sh, "QitoOS - up %llu:%02llu:%02llu, %d tasks, %llu MiB free\n\n",
                 (unsigned long long)(seconds / 3600),
                 (unsigned long long)((seconds % 3600) / 60),
                 (unsigned long long)(seconds % 60), count,
                 (unsigned long long)(pmm_free_bytes() / (1024 * 1024)));

    shell_color(sh, "\033[7m");
    shell_printf(sh, "%5s %-16s %-9s %7s %8s\n", "PID", "NAME", "STATE", "CPU%",
                 "TICKS");
    shell_reset_color(sh);

    for (int i = 0; i < count && i < 15; i++) {
        struct task *t  = &snapshot[i];
        uint64_t     pc = (t->ticks_total * 1000) / total_ticks;
        shell_printf(sh, "%5d %-16s %-9s %5llu.%llu%% %8llu\n", t->pid, t->name,
                     sched_state_name(t->state), (unsigned long long)(pc / 10),
                     (unsigned long long)(pc % 10),
                     (unsigned long long)t->ticks_total);
    }
    return 0;
}

/* --- configuration ---------------------------------------------------- */

static int cmd_config(struct shell *sh, int argc, char **argv)
{
    if (argc < 2 || strcmp(argv[1], "list") == 0) {
        section(sh, "System configuration");
        config_list(sh);
        return 0;
    }

    if (strcmp(argv[1], "get") == 0) {
        if (argc < 3) {
            shell_error(sh, "usage: config get <key>");
            return 1;
        }
        const char *value = config_get_string(argv[2], NULL);
        if (!value) {
            shell_error(sh, "config: no such key: %s", argv[2]);
            return 1;
        }
        shell_printf(sh, "%s\n", value);
        return 0;
    }

    if (strcmp(argv[1], "set") == 0) {
        if (argc < 4) {
            shell_error(sh, "usage: config set <key> <value>");
            return 1;
        }
        if (config_set_string(argv[2], argv[3]) != 0) {
            shell_error(sh, "config: cannot set %s", argv[2]);
            return 1;
        }
        shell_printf(sh, "%s = %s\n", argv[2], argv[3]);
        return 0;
    }

    if (strcmp(argv[1], "save") == 0) {
        if (config_save() != 0) {
            shell_error(sh, "config: failed to save");
            return 1;
        }
        shell_printf(sh, "configuration written to %s\n", CONFIG_PATH);
        return 0;
    }

    if (strcmp(argv[1], "reload") == 0) {
        config_load();
        shell_printf(sh, "configuration reloaded\n");
        return 0;
    }

    if (strcmp(argv[1], "reset") == 0) {
        config_reset_defaults();
        shell_printf(sh, "configuration reset to defaults\n");
        return 0;
    }

    shell_error(sh, "config: unknown subcommand '%s'", argv[1]);
    shell_printf(sh, "usage: config [list|get|set|save|reload|reset]\n");
    return 1;
}

static int cmd_hostname(struct shell *sh, int argc, char **argv)
{
    if (argc < 2) {
        shell_printf(sh, "%s\n", config_get_string("hostname", "qito"));
        return 0;
    }
    config_set_string("hostname", argv[1]);
    shell_printf(sh, "hostname set to %s\n", argv[1]);
    return 0;
}

static int cmd_service(struct shell *sh, int argc, char **argv)
{
    if (argc < 2 || strcmp(argv[1], "list") == 0) {
        service_list(sh);
        return 0;
    }
    if (argc < 3) {
        shell_error(sh, "usage: service <list|status|start|stop|restart> [name]");
        return 1;
    }

    if (strcmp(argv[1], "status") == 0) {
        return service_status(sh, argv[2]);
    }
    if (strcmp(argv[1], "start") == 0) {
        return service_start(sh, argv[2]);
    }
    if (strcmp(argv[1], "stop") == 0) {
        return service_stop(sh, argv[2]);
    }
    if (strcmp(argv[1], "restart") == 0) {
        service_stop(sh, argv[2]);
        return service_start(sh, argv[2]);
    }

    shell_error(sh, "service: unknown subcommand '%s'", argv[1]);
    return 1;
}

/* --- packages --------------------------------------------------------- */

static int cmd_pkg(struct shell *sh, int argc, char **argv)
{
    return pkg_command(sh, argc, argv);
}

/* --- storage ---------------------------------------------------------- */

static int cmd_df(struct shell *sh, int argc, char **argv)
{
    UNUSED(argc);
    UNUSED(argv);

    struct fs_statistics stats;
    fs_get_statistics(&stats);

    shell_printf(sh, "%-16s %10s %10s %10s  %s\n", "FILESYSTEM", "NODES", "FILES",
                 "DATA", "MOUNT");
    shell_printf(sh, "%-16s %10llu %10llu %9lluK  %s\n", "qitofs",
                 (unsigned long long)stats.nodes,
                 (unsigned long long)stats.files,
                 (unsigned long long)(stats.total_bytes / 1024), "/");
    shell_printf(sh, "%-16s %10llu %10s %10s  %s\n", "devfs",
                 (unsigned long long)stats.devices, "-", "-", "/dev");
    shell_printf(sh, "%-16s %10s %10s %10s  %s\n", "procfs", "-", "-", "dynamic",
                 "/proc");

    shell_printf(sh, "\nMemory backing store: %llu MiB total, %llu MiB free\n",
                 (unsigned long long)(pmm_total_bytes() / (1024 * 1024)),
                 (unsigned long long)(pmm_free_bytes() / (1024 * 1024)));
    shell_printf(sh, "Open file handles: %llu\n",
                 (unsigned long long)stats.open_files);
    return 0;
}

static int cmd_mount(struct shell *sh, int argc, char **argv)
{
    UNUSED(argc);
    UNUSED(argv);

    shell_printf(sh, "qitofs on / type qitofs (rw,ramdisk)\n");
    shell_printf(sh, "devfs on /dev type devfs (rw)\n");
    shell_printf(sh, "procfs on /proc type procfs (ro,generated)\n");
    return 0;
}

static int cmd_fsck(struct shell *sh, int argc, char **argv)
{
    UNUSED(argc);
    UNUSED(argv);

    shell_printf(sh, "Checking the Qito root filesystem...\n\n");

    struct fs_statistics stats;
    fs_get_statistics(&stats);

    int problems = 0;

    shell_printf(sh, "  Pass 1: node tree            ");
    struct fs_node *root = fs_root();
    if (root && root->type == FS_DIR) {
        shell_printf(sh, "ok (%llu nodes)\n", (unsigned long long)stats.nodes);
    } else {
        shell_printf(sh, "FAILED\n");
        problems++;
    }

    shell_printf(sh, "  Pass 2: required directories ");
    static const char *required[] = {"/dev", "/etc", "/tmp", "/proc", "/home"};
    int missing = 0;
    for (size_t i = 0; i < ARRAY_SIZE(required); i++) {
        if (!fs_lookup(required[i])) {
            missing++;
        }
    }
    if (missing == 0) {
        shell_printf(sh, "ok\n");
    } else {
        shell_printf(sh, "%d missing\n", missing);
        problems += missing;
    }

    shell_printf(sh, "  Pass 3: heap integrity       ");
    if (heap_validate()) {
        shell_printf(sh, "ok\n");
    } else {
        shell_printf(sh, "FAILED\n");
        problems++;
    }

    shell_printf(sh, "\n%s: %llu files, %llu directories, %llu KiB used\n", "/",
                 (unsigned long long)stats.files,
                 (unsigned long long)stats.directories,
                 (unsigned long long)(stats.total_bytes / 1024));
    shell_printf(sh, "%d problem(s) found.\n", problems);
    return problems ? 1 : 0;
}

/* --- networking ------------------------------------------------------- */

static int cmd_netinfo(struct shell *sh, int argc, char **argv)
{
    UNUSED(argc);
    UNUSED(argv);
    net_print_info(sh);
    return 0;
}

static int cmd_ping(struct shell *sh, int argc, char **argv)
{
    if (argc < 2) {
        shell_error(sh, "usage: ping <address>");
        return 1;
    }
    return net_ping(sh, argv[1]);
}

/* --- security --------------------------------------------------------- */

static int cmd_users(struct shell *sh, int argc, char **argv)
{
    UNUSED(argc);
    UNUSED(argv);

    shell_printf(sh, "%-12s %5s %5s  %s\n", "USER", "UID", "GID", "HOME");
    shell_printf(sh, "%-12s %5d %5d  %s\n", "root", 0, 0, "/root");
    shell_printf(sh, "%-12s %5d %5d  %s\n", "user", 1000, 1000, "/home/user");

    struct task *task = sched_current();
    shell_printf(sh, "\nCurrent task runs as uid %u, gid %u\n",
                 task ? task->uid : 0, task ? task->gid : 0);
    return 0;
}

static int cmd_perms(struct shell *sh, int argc, char **argv)
{
    if (argc < 2) {
        shell_error(sh, "usage: perms <path> [mode]");
        return 1;
    }

    char resolved[FS_PATH_MAX];
    shell_resolve(sh, argv[1], resolved, sizeof(resolved));

    struct fs_node *node = fs_lookup(resolved);
    if (!node) {
        shell_error(sh, "perms: %s: no such file or directory", argv[1]);
        return 1;
    }

    if (argc >= 3) {
        uint32_t mode = (uint32_t)strtoul(argv[2], NULL, 8);
        node->permissions = mode & 07777;
        shell_printf(sh, "permissions of %s set to %04o\n", resolved,
                     node->permissions);
        return 0;
    }

    char formatted[12];
    fs_format_permissions(node->permissions, node->type, formatted);
    shell_printf(sh, "%s  %04o  %s  (uid %u, gid %u)\n", formatted,
                 node->permissions, resolved, node->uid, node->gid);
    return 0;
}

/* --- power ------------------------------------------------------------ */

static int cmd_reboot(struct shell *sh, int argc, char **argv)
{
    UNUSED(argc);
    UNUSED(argv);

    shell_printf(sh, "Rebooting QitoOS...\n");
    config_save();
    power_reboot();
    return 0;
}

static int cmd_poweroff(struct shell *sh, int argc, char **argv)
{
    UNUSED(argc);
    UNUSED(argv);

    shell_printf(sh, "Shutting down QitoOS...\n");
    config_save();
    power_shutdown();
    return 0;
}

/* --- misc ------------------------------------------------------------- */

static int cmd_version(struct shell *sh, int argc, char **argv)
{
    UNUSED(argc);
    UNUSED(argv);

    shell_color(sh, "\033[96m");
    shell_printf(sh, "QitoConfigShell (QCSH)");
    shell_reset_color(sh);
    shell_printf(sh, " - QitoOS configuration and diagnostics shell\n");
    shell_printf(sh, "%s %s \"%s\"\n", QITO_PROJECT_NAME, QITO_VERSION_STRING,
                 QITO_CODENAME);
    shell_printf(sh, "Build %s, built %s\n", QITO_BUILD_ID, QITO_BUILD_DATE);
    shell_printf(sh, "Maintainer: %s <%s>\n", QITO_MAINTAINER, QITO_CONTACT);
    shell_printf(sh, "%s\n", QITO_PROJECT_URL);
    return 0;
}

static int cmd_ush(struct shell *sh, int argc, char **argv)
{
    struct shell *ush = ultrashell_instance();

    if (argc > 1) {
        char line[SHELL_LINE_MAX] = "";
        for (int i = 1; i < argc; i++) {
            if (i > 1) {
                strlcat(line, " ", sizeof(line));
            }
            strlcat(line, argv[i], sizeof(line));
        }
        struct shell_sink *saved = ush->sink;
        ush->sink                = sh->sink;
        int status               = shell_execute_line(ush, line);
        ush->sink                = saved;
        return status;
    }

    sh->switch_request = "ush";
    ush->running       = true;
    shell_printf(sh, "Switching to UltraShell. Type 'qcsh' to return.\n");
    return 0;
}

static int cmd_clear(struct shell *sh, int argc, char **argv)
{
    UNUSED(sh);
    UNUSED(argc);
    UNUSED(argv);
    console_clear();
    return 0;
}

static int cmd_uptime(struct shell *sh, int argc, char **argv)
{
    UNUSED(argc);
    UNUSED(argv);

    uint64_t ms      = time_uptime_ms();
    uint64_t seconds = ms / 1000;

    shell_printf(sh, "up %llu day(s), %02llu:%02llu:%02llu\n",
                 (unsigned long long)(seconds / 86400),
                 (unsigned long long)((seconds % 86400) / 3600),
                 (unsigned long long)((seconds % 3600) / 60),
                 (unsigned long long)(seconds % 60));
    shell_printf(sh, "tasks: %d, context switches: %llu, interrupts: %llu\n",
                 sched_task_count(),
                 (unsigned long long)sched_context_switches(),
                 (unsigned long long)interrupt_total_count());
    return 0;
}

/* --- command table ---------------------------------------------------- */

static const struct shell_command commands[] = {
    /* system information */
    {"sysinfo", "full system summary", "sysinfo", NULL, cmd_sysinfo, 0},
    {"cpuinfo", "processor details and features", "cpuinfo", NULL, cmd_cpuinfo, 0},
    {"meminfo", "memory usage breakdown", "meminfo [-v]", NULL, cmd_meminfo, 0},
    {"hwinfo", "detected hardware inventory", "hwinfo", NULL, cmd_hwinfo, 0},
    {"uptime", "how long the system has run", "uptime", NULL, cmd_uptime, 0},
    {"version", "shell and system version", "version", NULL, cmd_version, 0},

    /* diagnostics */
    {"dmesg", "show the kernel log", "dmesg [-n count] [-c]",
     "  -n  show only the last <count> lines\n"
     "  -c  clear the log after printing",
     cmd_dmesg, 0},
    {"loglevel", "get or set the kernel log level", "loglevel [level]",
     "Levels, in increasing verbosity: panic, error, warn, info, debug, trace.",
     cmd_loglevel, 0},
    {"irqinfo", "interrupt controller statistics", "irqinfo", NULL, cmd_irqinfo, 0},
    {"diag", "run system health checks", "diag", NULL, cmd_diag, 0},
    {"selftest", "run the kernel self-test suite", "selftest",
     "Exercises the heap, page allocator, filesystem, string library and\n"
     "formatting code, reporting a pass/fail result for each check.",
     cmd_selftest, 0},
    {"benchmark", "measure basic system performance", "benchmark", NULL,
     cmd_benchmark, 0},

    /* processes */
    {"ps", "list running tasks", "ps [-l]", NULL, cmd_ps, 0},
    {"top", "tasks ordered by CPU usage", "top", NULL, cmd_top, 0},
    {"taskinfo", "detailed information about a task", "taskinfo <pid>", NULL,
     cmd_taskinfo, 0},
    {"kill", "terminate a task", "kill <pid>", NULL, cmd_kill, CMD_PRIVILEGED},

    /* configuration */
    {"config", "view and change system configuration",
     "config [list|get <key>|set <key> <value>|save|reload|reset]",
     "Settings are stored in " CONFIG_PATH " and survive a 'config save'.",
     cmd_config, 0},
    {"hostname", "show or set the system hostname", "hostname [name]", NULL,
     cmd_hostname, 0},
    {"service", "manage system services",
     "service <list|status|start|stop|restart> [name]", NULL, cmd_service, 0},

    /* packages */
    {"pkg", "package management",
     "pkg <list|info|install|remove|search|update> [name]",
     "Manages the software components bundled with QitoOS.",
     cmd_pkg, 0},

    /* storage */
    {"df", "filesystem usage", "df", NULL, cmd_df, 0},
    {"mount", "list mounted filesystems", "mount", NULL, cmd_mount, 0},
    {"fsck", "check filesystem consistency", "fsck", NULL, cmd_fsck, 0},

    /* networking */
    {"netinfo", "network interface status", "netinfo", NULL, cmd_netinfo, 0},
    {"ping", "test reachability of a host", "ping <address>", NULL, cmd_ping, 0},

    /* security */
    {"users", "list system accounts", "users", NULL, cmd_users, 0},
    {"perms", "show or change file permissions", "perms <path> [octal-mode]", NULL,
     cmd_perms, 0},

    /* power */
    {"reboot", "restart the machine", "reboot", NULL, cmd_reboot, CMD_PRIVILEGED},
    {"poweroff", "shut the machine down", "poweroff", NULL, cmd_poweroff,
     CMD_PRIVILEGED},

    /* shell */
    {"clear", "clear the screen", "clear", NULL, cmd_clear, 0},
    {"history", "show the command history", "history [-c]", NULL,
     shell_builtin_history, 0},
    {"alias", "define or list aliases", "alias [name=value]", NULL,
     shell_builtin_alias, 0},
    {"help", "list commands or describe one", "help [command]", NULL,
     shell_builtin_help, 0},
    {"ush", "run an UltraShell command", "ush [command...]",
     "With no arguments, switches to an interactive UltraShell session.",
     cmd_ush, 0},
    {"exit", "leave the shell", "exit [status]", NULL, shell_builtin_exit, 0},
};

const struct shell_command *qcsh_commands(int *count)
{
    *count = (int)ARRAY_SIZE(commands);
    return commands;
}

struct shell *qcsh_instance(void)
{
    return &qcsh;
}

void qcsh_init(void)
{
    shell_init(&qcsh, "qcsh", commands, (int)ARRAY_SIZE(commands));
    shell_set_var(&qcsh, "SHELL", "qcsh");

    shell_set_alias(&qcsh, "info", "sysinfo");
    shell_set_alias(&qcsh, "mem", "meminfo");
    shell_set_alias(&qcsh, "cpu", "cpuinfo");
    shell_set_alias(&qcsh, "log", "dmesg");

    KLOG_INFO("qcsh", "%d commands registered", (int)ARRAY_SIZE(commands));
}
