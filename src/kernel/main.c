/*
 * Qira OS - kernel entry and system startup
 *
 * Brings the machine up in ordered stages, each of which depends only on the
 * stages before it:
 *
 *   1. console      serial output, so a failure anywhere later is visible
 *   2. cpu          descriptor tables, interrupt vectors, the PIC
 *   3. memory       physical frames, page tables, the kernel heap
 *   4. time         real-time clock, TSC calibration, the scheduler tick
 *   5. devices      PCI, input, graphics, audio
 *   6. filesystem   VFS, the boot ramdisk, /dev and /proc
 *   7. services     configuration, packages, networking
 *   8. userspace    shells, the desktop, and the scheduler
 */

#include <kernel/types.h>
#include <kernel/log.h>
#include <kernel/serial.h>
#include <kernel/cpu.h>
#include <kernel/irq.h>
#include <kernel/mm.h>
#include <kernel/time.h>
#include <kernel/fb.h>
#include <kernel/string.h>
#include <kernel/io.h>
#include <kernel/version.h>
#include <kernel/printf.h>
#include <kernel/console.h>
#include <kernel/fs.h>
#include <kernel/sched.h>
#include <kernel/syscall.h>
#include <kernel/input.h>
#include <kernel/shell.h>
#include <kernel/config.h>
#include <kernel/service.h>
#include <kernel/pkg.h>
#include <kernel/pci.h>
#include <kernel/net.h>
#include <kernel/audio.h>
#include <kernel/power.h>
#include <kernel/desktop.h>
#include <kernel/ipc.h>
#include <kernel/font.h>
#include <kernel/qac.h>
#include <kernel/lqx.h>
#include <kernel/splash.h>
#include <kernel/clipboard.h>
#include <kernel/random.h>

#include "../boot/bootinfo.h"

/* Declared in the filesystem backends. */
void devfs_init(void);
void procfs_init(void);

/*
 * The boot info block lives in low memory that the page allocator will later
 * hand out, so it is copied somewhere safe before anything else runs.
 */
static struct qira_boot_info boot_info_copy;

const struct qira_boot_info *kernel_boot_info(void)
{
    return &boot_info_copy;
}

static void serial_log_sink(const char *text, size_t len, log_level_t level)
{
    UNUSED(level);
    serial_write_len(text, len);
}

static void print_banner(void)
{
    kputs("\n");
    kputs("   ___  _            ___  ___\n");
    kputs("  / _ \\(_)_ __ __ _ / _ \\/ __|\n");
    kputs(" | | | | | '__/ _` | | | \\__ \\\n");
    kputs(" | |_| | | | | (_| | |_| |___/\n");
    kputs("  \\__\\_\\_|_|  \\__,_|\\___/|___/\n");
    kprintf("\n Qira OS %s \"%s\" - x86_64\n", QIRA_VERSION_STRING, QIRA_CODENAME);
    kprintf(" Build %s, built %s\n", QIRA_BUILD_ID, QIRA_BUILD_DATE);
    kprintf(" %s\n\n", QIRA_PROJECT_URL);
}

/*
 * The init task: runs once the scheduler is live, finishes bringing userspace
 * up, and then starts the desktop.
 */
static void init_task(void *arg)
{
    UNUSED(arg);

    KLOG_INFO("init", "userspace starting");

    /* Shells. */
    qcsh_init();
    ultrashell_init();

    /* Optional startup sound. */
    if (config_get_bool("audio.startup_chime", true)) {
        audio_startup_chime();
    }

    /* Hand over to the desktop environment, which owns the display. */
    desktop_run();

    KLOG_WARN("init", "desktop exited; system is idle");
    for (;;) {
        sched_sleep_ms(1000);
    }
}

void kmain(struct qira_boot_info *boot);

void kmain(struct qira_boot_info *boot)
{
    /* --- 1. earliest output ------------------------------------------ */
    serial_init();
    log_init();
    log_add_sink(serial_log_sink);
    log_set_level(LOG_DEBUG);

    print_banner();

    if (boot->magic != QIRA_BOOT_MAGIC) {
        panic("bad boot info magic 0x%x (expected 0x%x): loader/kernel mismatch",
              boot->magic, QIRA_BOOT_MAGIC);
    }
    memcpy(&boot_info_copy, boot, sizeof(boot_info_copy));
    boot = &boot_info_copy;

    KLOG_INFO("boot", "boot protocol v%u, BIOS drive 0x%02x", boot->version,
              boot->boot_drive);
    KLOG_INFO("boot", "kernel %llu KiB at 0x%llx",
              (unsigned long long)(boot->kernel_size / 1024),
              (unsigned long long)boot->kernel_phys);
    if (boot->ramdisk_size) {
        KLOG_INFO("boot", "ramdisk %llu KiB at 0x%llx",
                  (unsigned long long)(boot->ramdisk_size / 1024),
                  (unsigned long long)boot->ramdisk_phys);
    }
    if (boot->fb_valid) {
        KLOG_INFO("boot", "framebuffer %ux%u %ubpp at 0x%llx", boot->fb_width,
                  boot->fb_height, boot->fb_bpp,
                  (unsigned long long)boot->fb_addr);
    }
    if (boot->acpi_rsdp) {
        KLOG_INFO("boot", "ACPI RSDP at 0x%llx",
                  (unsigned long long)boot->acpi_rsdp);
    }
    if (boot->cmdline[0]) {
        KLOG_INFO("boot", "command line: %s", boot->cmdline);
    }

    /* --- 2. CPU ------------------------------------------------------- */
    cpu_detect();
    gdt_init();
    idt_init();
    pic_init();

    /* --- 3. memory ---------------------------------------------------- */
    pmm_init(boot);
    vmm_init(boot);
    heap_init();

    /* --- 4. time, then interrupts on ---------------------------------- */
    time_init();
    cpu_sti();
    KLOG_INFO("boot", "interrupts enabled");

    /* --- 5. devices --------------------------------------------------- */
    pci_init();

    bool_t graphics = fb_init(boot);
    if (graphics) {
        splash_begin();
        splash_update("Starting Qira OS", 8);
    }

    console_init();
    input_init();
    keyboard_init();
    mouse_init();

    if (graphics) {
        splash_update("Detecting input devices", 26);
    }

    power_init();
    audio_init();

    /* --- 6. filesystem ------------------------------------------------ */
    fs_init();
    if (boot->ramdisk_size) {
        fs_mount_ramdisk((const void *)(uintptr_t)boot->ramdisk_phys,
                         (size_t)boot->ramdisk_size);
    }
    devfs_init();
    procfs_init();

    if (graphics) {
        splash_update("Mounting filesystems", 48);
    }

    /* --- 7. system services ------------------------------------------- */
    config_init();
    log_set_level((log_level_t)config_get_int("log.level", LOG_INFO));

    /* Typefaces and icons: both read settings and the filesystem. */
    font_init();
    qac_init();

    if (graphics) {
        splash_update("Loading fonts and icons", 60);
    }

    random_init();
    clipboard_init();
    syscall_init();
    ipc_init();
    service_init();
    lqx_init();
    net_init();
    pkg_init();

    if (graphics) {
        splash_update("Starting system services", 72);
    }

    /* --- 8. scheduler and userspace ----------------------------------- */
    sched_init();

    int pid = sched_create_kernel_task("init", init_task, NULL, PRIO_NORMAL);
    if (pid < 0) {
        panic("cannot create the init task");
    }

    if (graphics) {
        splash_update("Starting the desktop", 94);
    }

    KLOG_INFO("boot", "startup complete in %llu ms",
              (unsigned long long)time_uptime_ms());

    /* Never returns: the timer interrupt switches to the init task. */
    sched_start();
}
