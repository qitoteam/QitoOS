/*
 * QitoOS - clipboard, environment and utility commands for UltraShell
 *
 * The rest of the additions that came with the extended feature set: the
 * clipboard, random data, checksums, hex dumps, timing, watching a command,
 * and disk-free style reporting.
 */

#include <kernel/shell.h>
#include <kernel/clipboard.h>
#include <kernel/random.h>
#include <kernel/fs.h>
#include <kernel/string.h>
#include <kernel/printf.h>
#include <kernel/mm.h>
#include <kernel/time.h>
#include <kernel/sched.h>
#include <kernel/console.h>

/* --- clipboard --------------------------------------------------------- */

static int cmd_copy(struct shell *sh, int argc, char **argv)
{
    /* With no argument, copy whatever came down the pipe. */
    if (argc < 2) {
        const char *piped = shell_get_var(sh, "QITO_PIPE_INPUT");
        if (!piped || !piped[0]) {
            shell_error(sh, "usage: copy <text>   or   <command> | copy");
            return 1;
        }
        clipboard_set(piped, CLIP_TEXT, "ush");
        shell_printf(sh, "copied %llu bytes\n",
                     (unsigned long long)clipboard_length());
        return 0;
    }

    /* Copying a file's contents is the common case, so detect a path. */
    if (argc == 2) {
        char resolved[FS_PATH_MAX];
        shell_resolve(sh, argv[1], resolved, sizeof(resolved));

        struct fs_node *node = fs_lookup(resolved);
        if (node && node->type == FS_FILE) {
            char  *buffer = shell_scratch();
            size_t got    = 0;

            if (buffer &&
                fs_read_file(resolved, buffer, shell_scratch_size() - 1,
                             &got) == 0) {
                buffer[got] = '\0';
                clipboard_set_len(buffer, got, CLIP_TEXT, "ush");
                shell_printf(sh, "copied %llu bytes from %s\n",
                             (unsigned long long)got, resolved);
                return 0;
            }
        }
    }

    /* Otherwise join the arguments as literal text. */
    char text[1024] = "";
    for (int i = 1; i < argc; i++) {
        if (i > 1) {
            strlcat(text, " ", sizeof(text));
        }
        strlcat(text, argv[i], sizeof(text));
    }

    clipboard_set(text, CLIP_TEXT, "ush");
    shell_printf(sh, "copied %llu bytes\n",
                 (unsigned long long)clipboard_length());
    return 0;
}

static int cmd_paste(struct shell *sh, int argc, char **argv)
{
    const char *contents = clipboard_get();

    if (!contents) {
        shell_error(sh, "paste: the clipboard is empty");
        return 1;
    }

    /* `paste -o file` writes instead of printing. */
    if (argc >= 3 && strcmp(argv[1], "-o") == 0) {
        char resolved[FS_PATH_MAX];
        shell_resolve(sh, argv[2], resolved, sizeof(resolved));

        if (fs_write_file(resolved, contents, clipboard_length()) != 0) {
            shell_error(sh, "paste: cannot write %s", resolved);
            return 1;
        }
        shell_printf(sh, "wrote %llu bytes to %s\n",
                     (unsigned long long)clipboard_length(), resolved);
        return 0;
    }

    shell_write(sh, contents, clipboard_length());
    if (clipboard_length() && contents[clipboard_length() - 1] != '\n') {
        shell_printf(sh, "\n");
    }
    return 0;
}

static int cmd_clipboard(struct shell *sh, int argc, char **argv)
{
    if (argc >= 2 && strcmp(argv[1], "clear") == 0) {
        clipboard_clear();
        shell_printf(sh, "clipboard cleared\n");
        return 0;
    }

    char preview[80];
    clipboard_preview(preview, sizeof(preview));

    shell_printf(sh, "  %-12s %llu bytes\n", "Size",
                 (unsigned long long)clipboard_length());
    shell_printf(sh, "  %-12s %s\n", "Set by", clipboard_owner());
    shell_printf(sh, "  %-12s %s\n", "Contents", preview);
    return 0;
}

/* --- data and checksums ------------------------------------------------ */

static int cmd_random(struct shell *sh, int argc, char **argv)
{
    int count = (argc > 1) ? atoi(argv[1]) : 1;
    if (count < 1) {
        count = 1;
    }
    if (count > 64) {
        count = 64;
    }

    /* `random -x N` prints hexadecimal bytes instead of numbers. */
    if (argc > 2 && strcmp(argv[1], "-x") == 0) {
        int bytes = atoi(argv[2]);
        bytes = CLAMP(bytes, 1, 256);

        uint8_t buffer[256];
        random_bytes(buffer, (size_t)bytes);

        for (int i = 0; i < bytes; i++) {
            shell_printf(sh, "%02x", buffer[i]);
            if ((i % 32) == 31) {
                shell_printf(sh, "\n");
            }
        }
        if (bytes % 32) {
            shell_printf(sh, "\n");
        }
        return 0;
    }

    /* `random N M` picks numbers below M. */
    uint32_t bound = (argc > 2) ? (uint32_t)atoi(argv[2]) : 0;

    for (int i = 0; i < count; i++) {
        if (bound > 0) {
            shell_printf(sh, "%u\n", random_below(bound));
        } else {
            shell_printf(sh, "%llu\n", (unsigned long long)random_u64());
        }
    }
    return 0;
}

/* A 32-bit FNV-1a hash: small, fast and good enough to spot a changed file. */
static uint32_t fnv1a(const uint8_t *data, size_t len)
{
    uint32_t hash = 2166136261u;
    for (size_t i = 0; i < len; i++) {
        hash ^= data[i];
        hash *= 16777619u;
    }
    return hash;
}

static int cmd_checksum(struct shell *sh, int argc, char **argv)
{
    if (argc < 2) {
        shell_error(sh, "usage: checksum <file>...");
        return 1;
    }

    int failures = 0;

    for (int i = 1; i < argc; i++) {
        char resolved[FS_PATH_MAX];
        shell_resolve(sh, argv[i], resolved, sizeof(resolved));

        char  *buffer = shell_scratch();
        size_t got    = 0;

        if (!buffer ||
            fs_read_file(resolved, buffer, shell_scratch_size(), &got) != 0) {
            shell_error(sh, "checksum: %s: cannot read", argv[i]);
            failures++;
            continue;
        }

        shell_printf(sh, "%08x  %llu  %s\n", fnv1a((const uint8_t *)buffer, got),
                     (unsigned long long)got, argv[i]);
    }

    return failures ? 1 : 0;
}

static int cmd_hexdump(struct shell *sh, int argc, char **argv)
{
    if (argc < 2) {
        shell_error(sh, "usage: hexdump <file> [-n bytes]");
        return 1;
    }

    int limit = 256;
    for (int i = 2; i < argc; i++) {
        if (strcmp(argv[i], "-n") == 0 && i + 1 < argc) {
            limit = atoi(argv[++i]);
        }
    }
    limit = CLAMP(limit, 16, 4096);

    char resolved[FS_PATH_MAX];
    shell_resolve(sh, argv[1], resolved, sizeof(resolved));

    char  *buffer = shell_scratch();
    size_t got    = 0;

    if (!buffer ||
        fs_read_file(resolved, buffer, (size_t)limit, &got) != 0) {
        shell_error(sh, "hexdump: %s: cannot read", argv[1]);
        return 1;
    }

    const uint8_t *bytes = (const uint8_t *)buffer;

    for (size_t offset = 0; offset < got; offset += 16) {
        shell_printf(sh, "%08llx  ", (unsigned long long)offset);

        for (size_t i = 0; i < 16; i++) {
            if (offset + i < got) {
                shell_printf(sh, "%02x ", bytes[offset + i]);
            } else {
                shell_printf(sh, "   ");
            }
            if (i == 7) {
                shell_printf(sh, " ");
            }
        }

        shell_printf(sh, " |");
        for (size_t i = 0; i < 16 && offset + i < got; i++) {
            uint8_t c = bytes[offset + i];
            shell_printf(sh, "%c", (c >= 32 && c < 127) ? c : '.');
        }
        shell_printf(sh, "|\n");
    }

    shell_printf(sh, "%llu bytes\n", (unsigned long long)got);
    return 0;
}

/* --- timing and repetition --------------------------------------------- */

static int cmd_time(struct shell *sh, int argc, char **argv)
{
    if (argc < 2) {
        shell_error(sh, "usage: time <command> [arguments...]");
        return 1;
    }

    char line[SHELL_LINE_MAX] = "";
    for (int i = 1; i < argc; i++) {
        if (i > 1) {
            strlcat(line, " ", sizeof(line));
        }
        strlcat(line, argv[i], sizeof(line));
    }

    uint64_t start = time_uptime_us();
    int status = shell_execute_line(sh, line);
    uint64_t elapsed = time_uptime_us() - start;

    shell_printf(sh, "\nreal  %llu.%03llu s   (%llu us)   status %d\n",
                 (unsigned long long)(elapsed / 1000000),
                 (unsigned long long)((elapsed / 1000) % 1000),
                 (unsigned long long)elapsed, status);
    return status;
}

static int cmd_watch(struct shell *sh, int argc, char **argv)
{
    if (argc < 2) {
        shell_error(sh, "usage: watch [-n count] <command>");
        return 1;
    }

    int first = 1;
    int times = 5;

    if (strcmp(argv[1], "-n") == 0 && argc > 3) {
        times = CLAMP(atoi(argv[2]), 1, 60);
        first = 3;
    }

    char line[SHELL_LINE_MAX] = "";
    for (int i = first; i < argc; i++) {
        if (i > first) {
            strlcat(line, " ", sizeof(line));
        }
        strlcat(line, argv[i], sizeof(line));
    }

    for (int iteration = 0; iteration < times; iteration++) {
        struct qito_time now;
        time_from_unix(rtc_unix_time(), &now);

        shell_color(sh, "\033[96m");
        shell_printf(sh, "--- %s   %02d:%02d:%02d   (%d of %d)\n", line,
                     now.hour, now.minute, now.second, iteration + 1, times);
        shell_reset_color(sh);

        shell_execute_line(sh, line);

        if (iteration + 1 < times) {
            sched_sleep_ms(1000);
        }
    }
    return 0;
}

/* --- system reporting -------------------------------------------------- */

static int cmd_free(struct shell *sh, int argc, char **argv)
{
    UNUSED(argc);
    UNUSED(argv);

    struct heap_stats heap;
    heap_get_stats(&heap);

    uint64_t total = pmm_total_bytes();
    uint64_t used  = pmm_used_bytes();
    uint64_t free_bytes = pmm_free_bytes();

    shell_printf(sh, "%-12s %10s %10s %10s\n", "", "TOTAL", "USED", "FREE");
    shell_printf(sh, "%-12s %9lluM %9lluM %9lluM\n", "Physical",
                 (unsigned long long)(total / (1024 * 1024)),
                 (unsigned long long)(used / (1024 * 1024)),
                 (unsigned long long)(free_bytes / (1024 * 1024)));
    shell_printf(sh, "%-12s %9lluK %9lluK %9lluK\n", "Heap",
                 (unsigned long long)(heap.total_bytes / 1024),
                 (unsigned long long)(heap.used_bytes / 1024),
                 (unsigned long long)(heap.free_bytes / 1024));

    int percent = total ? (int)((used * 100) / total) : 0;
    shell_printf(sh, "\n[");
    for (int i = 0; i < 40; i++) {
        shell_printf(sh, "%s", (i < percent * 40 / 100) ? "#" : ".");
    }
    shell_printf(sh, "] %d%% used\n", percent);
    return 0;
}

static int cmd_uptime_load(struct shell *sh, int argc, char **argv)
{
    UNUSED(argc);
    UNUSED(argv);

    uint64_t seconds = time_uptime_ms() / 1000;
    uint32_t load    = load_average_centi();

    shell_printf(sh, "up %llu:%02llu:%02llu,  %d task(s),  load %u.%02u\n",
                 (unsigned long long)(seconds / 3600),
                 (unsigned long long)((seconds % 3600) / 60),
                 (unsigned long long)(seconds % 60), sched_task_count(),
                 load / 100, load % 100);
    return 0;
}

static int cmd_yes(struct shell *sh, int argc, char **argv)
{
    /* Bounded, unlike the Unix original: an unbounded loop here would wedge
     * the shell with no way to interrupt it. */
    const char *text = (argc > 1) ? argv[1] : "y";
    int count = (argc > 2) ? CLAMP(atoi(argv[2]), 1, 1000) : 10;

    for (int i = 0; i < count; i++) {
        shell_printf(sh, "%s\n", text);
    }
    return 0;
}

static int cmd_seq(struct shell *sh, int argc, char **argv)
{
    if (argc < 2) {
        shell_error(sh, "usage: seq [first] <last> [step]");
        return 1;
    }

    int first = 1, last = 0, step = 1;

    if (argc == 2) {
        last = atoi(argv[1]);
    } else if (argc == 3) {
        first = atoi(argv[1]);
        last  = atoi(argv[2]);
    } else {
        first = atoi(argv[1]);
        step  = atoi(argv[2]);
        last  = atoi(argv[3]);
    }

    if (step == 0) {
        shell_error(sh, "seq: the step cannot be zero");
        return 1;
    }

    int emitted = 0;
    for (int value = first;
         (step > 0 ? value <= last : value >= last) && emitted < 10000;
         value += step) {
        shell_printf(sh, "%d\n", value);
        emitted++;
    }
    return 0;
}

/* --- the table --------------------------------------------------------- */

static const struct shell_command commands[] = {
    {"copy", "put text on the system clipboard", "copy <text|file>",
     "With no argument, copies piped input. Anything copied here can be\n"
     "pasted in the editor, Notes or any other application.",
     cmd_copy, 0},
    {"paste", "print or save the clipboard", "paste [-o file]", NULL,
     cmd_paste, 0},
    {"clipboard", "show what is on the clipboard", "clipboard [clear]", NULL,
     cmd_clipboard, 0},

    {"random", "generate random numbers or bytes",
     "random [count] [bound]   or   random -x <bytes>",
     "Not cryptographically secure; suitable for sampling and test data.",
     cmd_random, 0},
    {"checksum", "hash a file", "checksum <file>...",
     "Prints an FNV-1a hash, which is enough to tell whether a file changed.",
     cmd_checksum, 0},
    {"hexdump", "show a file as hexadecimal", "hexdump <file> [-n bytes]", NULL,
     cmd_hexdump, 0},

    {"time", "measure how long a command takes", "time <command>...", NULL,
     cmd_time, 0},
    {"watch", "run a command repeatedly", "watch [-n count] <command>", NULL,
     cmd_watch, 0},

    {"free", "memory usage summary", "free", NULL, cmd_free, 0},
    {"load", "uptime and load average", "load", NULL, cmd_uptime_load, 0},
    {"seq", "print a sequence of numbers", "seq [first] <last> [step]", NULL,
     cmd_seq, 0},
    {"yes", "repeat a string", "yes [text] [count]", NULL, cmd_yes, 0},
};

const struct shell_command *ultrashell_sys_commands(int *count)
{
    *count = (int)ARRAY_SIZE(commands);
    return commands;
}
