/*
 * Qira OS - About and Help
 *
 * Two small informational applications: "About" summarises the running system,
 * and "Help" documents the desktop, the shortcuts and the two shells.
 */

#include <kernel/desktop.h>
#include <kernel/version.h>
#include <kernel/cpu.h>
#include <kernel/mm.h>
#include <kernel/time.h>
#include <kernel/sched.h>
#include <kernel/string.h>
#include <kernel/printf.h>
#include <kernel/fs.h>
#include <kernel/net.h>
#include <kernel/pkg.h>
#include <kernel/shell.h>

/* --- About ------------------------------------------------------------ */

static void about_draw(struct window *win, int x, int y, int w, int h)
{
    const struct theme    *theme = desktop_theme();
    const struct cpu_info *cpu   = cpu_get_info();
    UNUSED(win);

    /* Header band. */
    fb_fill_gradient_v(x, y, w, 92, theme->titlebar_active, theme->window_bg);

    const char *title = "Qira OS";
    fb_draw_string_large(x + w / 2 - fb_text_width_large(title) / 2, y + 18, title,
                         theme->accent);

    char version[64];
    snprintf(version, sizeof(version), "version %s \"%s\"", QIRA_VERSION_STRING,
             QIRA_CODENAME);
    fb_draw_string(x + w / 2 - fb_text_width(version) / 2, y + 62, version,
                   theme->window_text);

    int line = 0;
    int base = y + 108;

    #define ROW(label, fmt, ...)                                              \
        do {                                                                  \
            char value[96];                                                   \
            snprintf(value, sizeof(value), fmt, __VA_ARGS__);                 \
            fb_draw_string(x + 20, base + line * 19, label,                   \
                           theme->titlebar_text_inactive);                    \
            fb_draw_string_clipped(x + 170, base + line * 19, value,          \
                                   theme->window_text, w - 190);              \
            line++;                                                           \
        } while (0)

    ROW("Architecture", "%s", "x86_64 (64-bit long mode)");
    ROW("Kernel build", "%s", QIRA_BUILD_ID);
    ROW("Built", "%s", QIRA_BUILD_DATE);
    line++;

    ROW("Processor", "%s", cpu->brand);
    ROW("Clock", "%llu MHz", (unsigned long long)(time_cpu_khz() / 1000));
    ROW("Memory", "%llu MiB total, %llu MiB free",
        (unsigned long long)(pmm_total_bytes() / (1024 * 1024)),
        (unsigned long long)(pmm_free_bytes() / (1024 * 1024)));
    ROW("Display", "%dx%d at 32 bpp", fb_width(), fb_height());
    line++;

    uint64_t seconds = time_uptime_ms() / 1000;
    ROW("Uptime", "%lluh %llum %llus", (unsigned long long)(seconds / 3600),
        (unsigned long long)((seconds % 3600) / 60),
        (unsigned long long)(seconds % 60));
    ROW("Tasks", "%d running", sched_task_count());
    ROW("Components", "%d installed", pkg_count());
    ROW("Network", "%d interface(s)", net_interface_count());
    line++;

    ROW("Maintainer", "%s", QIRA_MAINTAINER);
    ROW("Contact", "%s", QIRA_CONTACT);
    ROW("Project", "%s", QIRA_PROJECT_URL);
    ROW("Licence", "%s", "Apache License 2.0");

    #undef ROW

    /* Footer. */
    const char *footer = "A from-scratch operating system. Built for learning.";
    fb_draw_string(x + w / 2 - fb_text_width(footer) / 2, y + h - 26, footer,
                   theme->titlebar_text_inactive);
}

static const struct app_ops about_ops = {
    .draw = about_draw,
};

void app_about_register(void)
{
    desktop_register_app("About Qira OS", "i", &about_ops, 520, 470,
                         RGB(130, 190, 255), 0, true);
}

/* --- Help ------------------------------------------------------------- */

struct help_state {
    int page;
    int scroll;
};

/* Each page is a NULL-terminated list of lines. */
static const char *page_desktop[] = {
    "DESKTOP",
    "",
    "The Qira desktop composites a wallpaper, a stack of windows, a panel",
    "along the top edge, menus and notifications.",
    "",
    "Windows",
    "  Drag the title bar to move a window.",
    "  Drag the bottom-right grip to resize it.",
    "  The three circles close, maximise and minimise.",
    "  Drag a window to the top edge to maximise it.",
    "",
    "Panel",
    "  The Qira button on the left opens the application menu.",
    "  Each open window has a button; click it to raise or minimise.",
    "  The right side shows free memory, the task count and the clock.",
    "",
    "Keyboard shortcuts",
    "  Super+Space    open or close the application menu",
    "  Super+T        open a new terminal",
    "  Super+Q        close the focused window",
    "  Alt+Tab        cycle between windows",
    "  Alt+F4         close the focused window",
    NULL,
};

static const char *page_qcsh[] = {
    "QIRACONFIGSHELL (QCSH)",
    "",
    "QCSH is the administration and diagnostics shell. Start it from a",
    "terminal by typing 'qcsh', and return with 'ush'.",
    "",
    "System information",
    "  sysinfo        full summary of the running system",
    "  cpuinfo        processor model, features and clock",
    "  meminfo [-v]   physical and heap memory breakdown",
    "  hwinfo         detected hardware, PCI devices, audio",
    "  uptime         how long the system has been running",
    "",
    "Diagnostics",
    "  dmesg [-n N]   kernel log, optionally only the last N lines",
    "  loglevel LVL   change verbosity: error, warn, info, debug, trace",
    "  irqinfo        interrupt counts per IRQ line",
    "  diag           run the system health checks",
    "  selftest       exercise the heap, filesystem and libraries",
    "  benchmark      measure memory, allocator and filesystem speed",
    "",
    "Processes",
    "  ps [-l]        list tasks",
    "  top            tasks sorted by CPU time",
    "  taskinfo PID   detail for one task",
    "  kill PID       terminate a task",
    "",
    "Configuration and packages",
    "  config list | get KEY | set KEY VALUE | save | reload | reset",
    "  hostname [NAME]",
    "  service list | status | start | stop | restart NAME",
    "  pkg list | info | search | install | remove | update | stats",
    "",
    "Storage, network, security and power",
    "  df, mount, fsck",
    "  netinfo, ping ADDRESS",
    "  users, perms PATH [MODE]",
    "  reboot, poweroff",
    NULL,
};

static const char *page_ultrashell[] = {
    "ULTRASHELL",
    "",
    "UltraShell is the general-purpose shell for everyday work. It is the",
    "default in new terminals; reach QCSH with 'qcsh'.",
    "",
    "Navigation",
    "  pwd, cd [DIR|-], ls [-lah] [PATH], tree [PATH]",
    "  find [PATH] [-name PATTERN]",
    "",
    "Reading files",
    "  cat FILE...           head [-n N] FILE     tail [-n N] FILE",
    "  wc [FILE]             stat PATH",
    "  grep [-vinc] PATTERN [FILE]",
    "  sort [-r] [-u] [FILE] uniq [FILE]",
    "",
    "Writing files",
    "  echo [-n] TEXT        touch FILE...        mkdir [-p] DIR...",
    "  rm [-r] PATH...       cp SRC DST           mv SRC DST",
    "  write FILE TEXT...",
    "",
    "Text and arithmetic",
    "  rev, upper, lower",
    "  calc EXPRESSION       supports + - * / % and parentheses",
    "",
    "Environment and scripting",
    "  set [NAME=VALUE], unset NAME, env",
    "  alias [name=value], unalias NAME",
    "  source SCRIPT, repeat COUNT COMMAND, test CONDITION",
    "",
    "Pipelines and redirection",
    "  cat /proc/tasks | grep running",
    "  ls -l /etc > /tmp/listing.txt",
    "  echo more >> /tmp/listing.txt",
    "  cmd1 && cmd2      run cmd2 only if cmd1 succeeded",
    "  cmd1 || cmd2      run cmd2 only if cmd1 failed",
    "",
    "Variables expand with $NAME or ${NAME}; $? is the last exit status.",
    NULL,
};

static const char *page_filesystem[] = {
    "FILESYSTEM",
    "",
    "Qira mounts an in-memory root filesystem (QiraFS) populated from the",
    "boot ramdisk. Changes live in RAM and are lost on reboot.",
    "",
    "  /bin           system executables",
    "  /dev           device nodes",
    "  /etc           configuration, including qira.conf",
    "  /home/user     the default working directory",
    "  /proc          live kernel state, regenerated on every read",
    "  /tmp           scratch space, world writable",
    "  /usr/share/doc documentation",
    "  /var/log       logs",
    "",
    "Device nodes",
    "  /dev/null      discards writes, reads return end of file",
    "  /dev/zero      reads return zero bytes",
    "  /dev/random    pseudo-random bytes (not cryptographic)",
    "  /dev/console   the system console",
    "  /dev/serial    the COM1 serial port",
    "  /dev/kmsg      the kernel log ring buffer",
    "",
    "Useful /proc files",
    "  /proc/version      /proc/meminfo    /proc/cpuinfo",
    "  /proc/uptime       /proc/tasks      /proc/interrupts",
    "  /proc/filesystems  /proc/stat       /proc/display",
    "",
    "Try:  cat /proc/meminfo",
    "      ls -l /dev",
    NULL,
};

static const char **pages[] = {page_desktop, page_ultrashell, page_qcsh,
                               page_filesystem};
static const char *page_names[] = {"Desktop", "UltraShell", "QCSH", "Filesystem"};
#define PAGE_COUNT 4

static void help_on_open(struct window *win)
{
    window_set_title(win, "Help");
}

static void help_draw(struct window *win, int x, int y, int w, int h)
{
    struct help_state  *state = (struct help_state *)win->app_state;
    const struct theme *theme = desktop_theme();

    /* Page tabs. */
    fb_fill_rect(x, y, w, 26, theme->titlebar_inactive);
    int tab_width = MIN(120, w / PAGE_COUNT);
    for (int i = 0; i < PAGE_COUNT; i++) {
        int tx = x + i * tab_width;
        if (state->page == i) {
            fb_fill_rect(tx, y, tab_width, 26, theme->window_bg);
            fb_fill_rect(tx, y, tab_width, 2, theme->accent);
        }
        fb_draw_string_clipped(tx + 10, y + 5, page_names[i],
                               state->page == i ? theme->accent
                                                : theme->titlebar_text_inactive,
                               tab_width - 16);
    }
    fb_fill_rect(x, y + 25, w, 1, theme->border);

    /* Body. */
    const char **lines = pages[state->page];
    int total = 0;
    while (lines[total]) {
        total++;
    }

    int text_y  = y + 32;
    int visible = (h - (text_y - y) - 20) / FONT_HEIGHT;
    state->scroll = CLAMP(state->scroll, 0, MAX(total - visible, 0));

    for (int row = 0; row < visible; row++) {
        int index = state->scroll + row;
        if (index >= total) {
            break;
        }

        const char *line = lines[index];
        color_t     color = theme->window_text;

        /* Headings are all-caps lines; sub-headings start in column 0. */
        bool_t heading = true;
        for (const char *p = line; *p; p++) {
            if (*p >= 'a' && *p <= 'z') {
                heading = false;
                break;
            }
        }
        if (heading && line[0]) {
            color = theme->accent;
        } else if (line[0] && line[0] != ' ') {
            color = RGB(MIN(COLOR_R(theme->window_text) + 20, 255),
                        MIN(COLOR_G(theme->window_text) + 20, 255),
                        MIN(COLOR_B(theme->window_text) + 20, 255));
        } else if (line[0] == ' ') {
            color = theme->titlebar_text_inactive;
        }

        fb_draw_string_clipped(x + 14, text_y + row * FONT_HEIGHT, line, color,
                               w - 28);
    }

    /* Status bar. */
    int status_y = y + h - 18;
    fb_fill_rect(x, status_y, w, 18, theme->titlebar_inactive);
    char status[64];
    snprintf(status, sizeof(status), "Page %d of %d   Tab switches page",
             state->page + 1, PAGE_COUNT);
    fb_draw_string(x + 8, status_y + 1, status, theme->titlebar_text_inactive);
}

static bool_t help_on_key(struct window *win, uint32_t key, uint32_t modifiers)
{
    struct help_state *state = (struct help_state *)win->app_state;
    UNUSED(modifiers);

    switch (key) {
    case KEY_TAB:
        state->page   = (state->page + 1) % PAGE_COUNT;
        state->scroll = 0;
        return true;
    case KEY_LEFT:
        state->page   = (state->page + PAGE_COUNT - 1) % PAGE_COUNT;
        state->scroll = 0;
        return true;
    case KEY_RIGHT:
        state->page   = (state->page + 1) % PAGE_COUNT;
        state->scroll = 0;
        return true;
    case KEY_UP:
        state->scroll = MAX(state->scroll - 1, 0);
        return true;
    case KEY_DOWN:
        state->scroll++;
        return true;
    case KEY_PAGEUP:
        state->scroll = MAX(state->scroll - 12, 0);
        return true;
    case KEY_PAGEDOWN:
        state->scroll += 12;
        return true;
    case KEY_HOME:
        state->scroll = 0;
        return true;
    default:
        break;
    }
    return false;
}

static bool_t help_on_mouse(struct window *win, int x, int y, uint32_t buttons,
                            input_event_type_t type)
{
    struct help_state *state = (struct help_state *)win->app_state;

    if (type == INPUT_MOUSE_DOWN && (buttons & MOUSE_LEFT) && y < 26) {
        struct rect client;
        window_client_rect(win, &client);
        int tab_width = MIN(120, client.w / PAGE_COUNT);
        int tab = x / MAX(tab_width, 1);
        if (tab >= 0 && tab < PAGE_COUNT) {
            state->page   = tab;
            state->scroll = 0;
            return true;
        }
    }
    return false;
}

static const struct app_ops help_ops = {
    .draw     = help_draw,
    .on_key   = help_on_key,
    .on_mouse = help_on_mouse,
    .on_open  = help_on_open,
};

void app_help_register(void)
{
    desktop_register_app("Help", "?", &help_ops, 680, 480, RGB(160, 220, 180),
                         sizeof(struct help_state), true);
}
