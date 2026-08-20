/*
 * QitoOS - component registry and the `pkg` command
 */

#include <kernel/pkg.h>
#include <kernel/shell.h>
#include <kernel/string.h>
#include <kernel/log.h>
#include <kernel/version.h>
#include <kernel/config.h>
#include <kernel/fb.h>
#include <kernel/net.h>
#include <kernel/audio.h>
#include <kernel/input.h>

static struct package packages[PKG_MAX];
static int            package_count;

static void add(const char *name, const char *version, const char *summary,
                pkg_kind_t kind, bool_t installed, uint32_t size_kb)
{
    if (package_count >= PKG_MAX) {
        return;
    }

    struct package *pkg = &packages[package_count++];
    strlcpy(pkg->name, name, sizeof(pkg->name));
    strlcpy(pkg->version, version, sizeof(pkg->version));
    strlcpy(pkg->summary, summary, sizeof(pkg->summary));
    pkg->kind      = kind;
    pkg->installed = installed;
    pkg->enabled   = installed;
    pkg->size_kb   = size_kb;
}

void pkg_init(void)
{
    package_count = 0;
    const char *v = QITO_VERSION_STRING;

    /* Core system. */
    add("qito-kernel", v, "QitoOS kernel: memory, tasks, syscalls", PKG_CORE,
        true, 96);
    add("qito-boot", v, "bootloader and ISO boot chain", PKG_CORE, true, 4);
    add("qitofs", v, "in-memory root filesystem and VFS", PKG_CORE, true, 24);
    add("libq", v, "freestanding C runtime shared by kernel and userspace",
        PKG_LIBRARY, true, 18);

    /* Drivers. */
    add("drv-vesa", v, "VESA linear framebuffer graphics", PKG_DRIVER,
        fb_available(), 12);
    add("drv-ps2", v, "PS/2 keyboard and mouse", PKG_DRIVER, true, 8);
    add("drv-serial", v, "16550 UART serial console", PKG_DRIVER, true, 4);
    add("drv-pci", v, "PCI bus enumeration", PKG_DRIVER, true, 6);
    add("drv-ne2000", v, "NE2000/RTL8029 ethernet", PKG_DRIVER,
        net_interface_count() > 1, 10);
    add("drv-pcspk", v, "PC speaker audio", PKG_DRIVER, true, 3);
    add("drv-rtc", v, "CMOS real-time clock", PKG_DRIVER, true, 3);

    /* Shells and desktop. */
    add("qcsh", v, "QitoConfigShell: configuration and diagnostics", PKG_CORE,
        true, 28);
    add("ultrashell", v, "UltraShell: general-purpose command shell", PKG_CORE,
        true, 32);
    add("qito-desktop", v, "desktop environment and window manager", PKG_CORE,
        true, 48);

    /* Applications. */
    add("app-terminal", v, "terminal emulator hosting both shells",
        PKG_APPLICATION, true, 10);
    add("app-files", v, "file manager", PKG_APPLICATION, true, 9);
    add("app-editor", v, "text editor", PKG_APPLICATION, true, 11);
    add("app-sysmon", v, "system monitor and task manager", PKG_APPLICATION, true,
        8);
    add("app-settings", v, "settings and configuration panel", PKG_APPLICATION,
        true, 9);
    add("app-logs", v, "kernel log viewer", PKG_APPLICATION, true, 5);
    add("app-about", v, "system information window", PKG_APPLICATION, true, 4);
    add("app-calculator", v, "calculator", PKG_APPLICATION, true, 6);
    add("app-clock", v, "clock and stopwatch", PKG_APPLICATION, true, 5);

    /* Networking. */
    add("net-ipv4", v, "IPv4, ARP and ICMP protocol stack", PKG_LIBRARY, true, 14);

    uint32_t total = 0;
    int      installed = 0;
    for (int i = 0; i < package_count; i++) {
        if (packages[i].installed) {
            installed++;
            total += packages[i].size_kb;
        }
    }

    KLOG_INFO("pkg", "%d components registered, %d installed (%u KiB)",
              package_count, installed, total);
}

int pkg_count(void)
{
    return package_count;
}

const struct package *pkg_at(int index)
{
    if (index < 0 || index >= package_count) {
        return NULL;
    }
    return &packages[index];
}

const struct package *pkg_find(const char *name)
{
    for (int i = 0; i < package_count; i++) {
        if (strcmp(packages[i].name, name) == 0) {
            return &packages[i];
        }
    }
    return NULL;
}

static const char *kind_name(pkg_kind_t kind)
{
    switch (kind) {
    case PKG_CORE:        return "core";
    case PKG_DRIVER:      return "driver";
    case PKG_APPLICATION: return "application";
    case PKG_LIBRARY:     return "library";
    default:              return "optional";
    }
}

static int pkg_list(struct shell *sh, const char *filter)
{
    shell_printf(sh, "%-16s %-8s %-12s %-9s %6s  %s\n", "COMPONENT", "VERSION",
                 "KIND", "STATE", "SIZE", "SUMMARY");

    int shown = 0;
    uint32_t total = 0;

    for (int i = 0; i < package_count; i++) {
        const struct package *pkg = &packages[i];

        if (filter && strstr(pkg->name, filter) == NULL &&
            strstr(pkg->summary, filter) == NULL) {
            continue;
        }

        shell_printf(sh, "%-16s %-8s %-12s %-9s %5uK  %s\n", pkg->name,
                     pkg->version, kind_name(pkg->kind),
                     pkg->installed ? (pkg->enabled ? "enabled" : "disabled")
                                    : "available",
                     pkg->size_kb, pkg->summary);
        shown++;
        if (pkg->installed) {
            total += pkg->size_kb;
        }
    }

    shell_printf(sh, "\n%d component(s) shown, %u KiB installed.\n", shown, total);
    return 0;
}

static int pkg_info(struct shell *sh, const char *name)
{
    const struct package *pkg = pkg_find(name);

    if (!pkg) {
        shell_printf(sh, "pkg: no such component: %s\n", name);
        return 1;
    }

    shell_printf(sh, "Component   : %s\n", pkg->name);
    shell_printf(sh, "Version     : %s\n", pkg->version);
    shell_printf(sh, "Kind        : %s\n", kind_name(pkg->kind));
    shell_printf(sh, "State       : %s\n",
                 pkg->installed ? (pkg->enabled ? "installed and enabled"
                                                : "installed but disabled")
                                : "not installed");
    shell_printf(sh, "Size        : %u KiB\n", pkg->size_kb);
    shell_printf(sh, "Summary     : %s\n", pkg->summary);
    shell_printf(sh, "Removable   : %s\n", pkg->kind == PKG_CORE ? "no" : "yes");
    return 0;
}

int pkg_command(struct shell *sh, int argc, char **argv)
{
    if (argc < 2 || strcmp(argv[1], "list") == 0) {
        return pkg_list(sh, (argc > 2) ? argv[2] : NULL);
    }

    if (strcmp(argv[1], "info") == 0) {
        if (argc < 3) {
            shell_printf(sh, "usage: pkg info <component>\n");
            return 1;
        }
        return pkg_info(sh, argv[2]);
    }

    if (strcmp(argv[1], "search") == 0) {
        if (argc < 3) {
            shell_printf(sh, "usage: pkg search <text>\n");
            return 1;
        }
        return pkg_list(sh, argv[2]);
    }

    if (strcmp(argv[1], "install") == 0 || strcmp(argv[1], "remove") == 0) {
        if (argc < 3) {
            shell_printf(sh, "usage: pkg %s <component>\n", argv[1]);
            return 1;
        }

        struct package *pkg = NULL;
        for (int i = 0; i < package_count; i++) {
            if (strcmp(packages[i].name, argv[2]) == 0) {
                pkg = &packages[i];
                break;
            }
        }
        if (!pkg) {
            shell_printf(sh, "pkg: no such component: %s\n", argv[2]);
            return 1;
        }

        bool_t installing = strcmp(argv[1], "install") == 0;

        if (!installing && pkg->kind == PKG_CORE) {
            shell_printf(sh, "pkg: %s is a core component and cannot be removed\n",
                         pkg->name);
            return 1;
        }
        if (installing && pkg->installed) {
            shell_printf(sh, "pkg: %s is already installed\n", pkg->name);
            return 0;
        }

        /*
         * Components are compiled into the image, so `install` and `remove`
         * toggle whether the component is active rather than moving files.
         */
        pkg->enabled = installing;
        shell_printf(sh, "pkg: %s %s\n", pkg->name,
                     installing ? "enabled" : "disabled");
        shell_printf(sh, "note: components are built into the QitoOS image, so "
                         "this only changes whether it is active.\n");
        return 0;
    }

    if (strcmp(argv[1], "update") == 0) {
        shell_printf(sh, "pkg: QitoOS components are versioned with the system "
                         "image.\n");
        shell_printf(sh, "     Installed version: %s (%s)\n", QITO_VERSION_STRING,
                     QITO_CODENAME);
        shell_printf(sh, "     Build: %s\n", QITO_BUILD_ID);
        shell_printf(sh, "     Update by building and booting a newer ISO from\n");
        shell_printf(sh, "     %s\n", QITO_PROJECT_URL);
        return 0;
    }

    if (strcmp(argv[1], "stats") == 0) {
        int counts[5] = {0};
        uint32_t total = 0;
        for (int i = 0; i < package_count; i++) {
            counts[packages[i].kind]++;
            if (packages[i].installed) {
                total += packages[i].size_kb;
            }
        }
        shell_printf(sh, "Components by kind:\n");
        shell_printf(sh, "  core         %d\n", counts[PKG_CORE]);
        shell_printf(sh, "  drivers      %d\n", counts[PKG_DRIVER]);
        shell_printf(sh, "  applications %d\n", counts[PKG_APPLICATION]);
        shell_printf(sh, "  libraries    %d\n", counts[PKG_LIBRARY]);
        shell_printf(sh, "  optional     %d\n", counts[PKG_OPTIONAL]);
        shell_printf(sh, "\nTotal installed size: %u KiB\n", total);
        return 0;
    }

    shell_printf(sh, "pkg: unknown subcommand '%s'\n", argv[1]);
    shell_printf(sh, "usage: pkg <list|info|search|install|remove|update|stats> "
                     "[component]\n");
    return 1;
}
