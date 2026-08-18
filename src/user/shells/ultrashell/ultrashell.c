/*
 * Qira OS - UltraShell
 *
 * The general-purpose interactive shell, inspired by the ergonomics of Bash
 * and the discoverability of PowerShell. UltraShell owns everyday work:
 * navigating the filesystem, manipulating files, inspecting text, scripting,
 * and running the system's utilities.
 *
 * System administration and configuration live in QCSH instead; the two shells
 * share the engine in shell_core.c but expose different command sets.
 */

#include <kernel/shell.h>
#include <kernel/fs.h>
#include <kernel/string.h>
#include <kernel/printf.h>
#include <kernel/mm.h>
#include <kernel/time.h>
#include <kernel/log.h>
#include <kernel/sched.h>
#include <kernel/version.h>
#include <kernel/syscall.h>
#include <kernel/console.h>

static struct shell ultrashell;

/* Read the piped-in text, if any. */
static const char *pipe_input(struct shell *sh)
{
    return shell_get_var(sh, "QIRA_PIPE_INPUT");
}

/* --- navigation ------------------------------------------------------- */

static int cmd_pwd(struct shell *sh, int argc, char **argv)
{
    UNUSED(argc);
    UNUSED(argv);
    shell_printf(sh, "%s\n", sh->cwd);
    return 0;
}

static int cmd_cd(struct shell *sh, int argc, char **argv)
{
    const char *target = (argc > 1) ? argv[1] : shell_get_var(sh, "HOME");

    if (!target) {
        target = "/";
    }

    /* `cd -` returns to the previous directory. */
    if (strcmp(target, "-") == 0) {
        const char *previous = shell_get_var(sh, "OLDPWD");
        if (!previous) {
            shell_error(sh, "cd: OLDPWD is not set");
            return 1;
        }
        target = previous;
    }

    char resolved[FS_PATH_MAX];
    shell_resolve(sh, target, resolved, sizeof(resolved));

    struct fs_node *node = fs_lookup(resolved);
    if (!node) {
        shell_error(sh, "cd: %s: no such file or directory", target);
        return 1;
    }
    if (node->type != FS_DIR) {
        shell_error(sh, "cd: %s: not a directory", target);
        return 1;
    }

    shell_set_var(sh, "OLDPWD", sh->cwd);
    strlcpy(sh->cwd, resolved, sizeof(sh->cwd));
    shell_set_var(sh, "PWD", sh->cwd);

    /* Keep the owning task's cwd in sync so syscalls agree. */
    struct task *task = sched_current();
    if (task) {
        strlcpy(task->cwd, resolved, sizeof(task->cwd));
    }
    return 0;
}

static void format_bytes(uint64_t bytes, char *out, size_t size)
{
    if (bytes >= 1024ull * 1024 * 1024) {
        snprintf(out, size, "%lluG",
                 (unsigned long long)(bytes / (1024ull * 1024 * 1024)));
    } else if (bytes >= 1024 * 1024) {
        snprintf(out, size, "%lluM", (unsigned long long)(bytes / (1024 * 1024)));
    } else if (bytes >= 1024) {
        snprintf(out, size, "%lluK", (unsigned long long)(bytes / 1024));
    } else {
        snprintf(out, size, "%llu", (unsigned long long)bytes);
    }
}

static int cmd_ls(struct shell *sh, int argc, char **argv)
{
    bool_t long_format = false;
    bool_t show_all    = false;
    bool_t human       = false;
    const char *path   = NULL;

    for (int i = 1; i < argc; i++) {
        if (argv[i][0] == '-' && argv[i][1]) {
            for (const char *flag = argv[i] + 1; *flag; flag++) {
                switch (*flag) {
                case 'l': long_format = true; break;
                case 'a': show_all = true;    break;
                case 'h': human = true;       break;
                default:
                    shell_error(sh, "ls: unknown option -- '%c'", *flag);
                    return 1;
                }
            }
        } else {
            path = argv[i];
        }
    }

    char resolved[FS_PATH_MAX];
    shell_resolve(sh, path ? path : ".", resolved, sizeof(resolved));

    struct fs_node *node = fs_lookup(resolved);
    if (!node) {
        shell_error(sh, "ls: %s: no such file or directory", path ? path : ".");
        return 1;
    }

    /* Listing a single file just prints it. */
    if (node->type != FS_DIR) {
        shell_printf(sh, "%s\n", resolved);
        return 0;
    }

    struct fs_dirent entry;
    int    count      = 0;
    uint64_t total    = 0;

    if (long_format) {
        shell_printf(sh, "total contents of %s\n", resolved);
    }

    for (int i = 0; fs_readdir(node, i, &entry) == 0; i++) {
        if (!show_all && entry.name[0] == '.') {
            continue;
        }
        count++;
        total += entry.size;

        if (long_format) {
            char child_path[FS_PATH_MAX];
            snprintf(child_path, sizeof(child_path), "%s%s%s", resolved,
                     (strcmp(resolved, "/") == 0) ? "" : "/", entry.name);

            struct fs_stat st;
            char           perms[12] = "----------";
            char           stamp[32] = "";

            if (fs_stat(child_path, &st) == 0) {
                fs_format_permissions(st.permissions, (fs_node_type_t)st.type,
                                      perms);
                struct qira_time t;
                time_from_unix(st.modified, &t);
                snprintf(stamp, sizeof(stamp), "%04d-%02d-%02d %02d:%02d", t.year,
                         t.month, t.day, t.hour, t.minute);
            }

            char size_text[24];
            if (human) {
                format_bytes(entry.size, size_text, sizeof(size_text));
            } else {
                snprintf(size_text, sizeof(size_text), "%llu",
                         (unsigned long long)entry.size);
            }

            shell_printf(sh, "%s %8s %s  ", perms, size_text, stamp);
            if (entry.type == FS_DIR) {
                shell_color(sh, "\033[94m");
            } else if (entry.type == FS_DEV) {
                shell_color(sh, "\033[93m");
            }
            shell_printf(sh, "%s", entry.name);
            shell_reset_color(sh);
            shell_printf(sh, "%s\n", (entry.type == FS_DIR) ? "/" : "");
        } else {
            if (entry.type == FS_DIR) {
                shell_color(sh, "\033[94m");
                shell_printf(sh, "%s/", entry.name);
                shell_reset_color(sh);
            } else if (entry.type == FS_DEV) {
                shell_color(sh, "\033[93m");
                shell_printf(sh, "%s", entry.name);
                shell_reset_color(sh);
            } else {
                shell_printf(sh, "%s", entry.name);
            }
            shell_printf(sh, "  ");
        }
    }

    if (!long_format && count > 0) {
        shell_printf(sh, "\n");
    }
    if (long_format) {
        char size_text[24];
        format_bytes(total, size_text, sizeof(size_text));
        shell_printf(sh, "\n%d entries, %s\n", count, size_text);
    }
    return 0;
}

/* --- file contents ---------------------------------------------------- */

static int cmd_cat(struct shell *sh, int argc, char **argv)
{
    if (argc < 2) {
        const char *piped = pipe_input(sh);
        if (piped) {
            shell_puts(sh, piped);
            return 0;
        }
        shell_error(sh, "usage: cat <file>...");
        return 1;
    }

    int status = 0;
    for (int i = 1; i < argc; i++) {
        char resolved[FS_PATH_MAX];
        shell_resolve(sh, argv[i], resolved, sizeof(resolved));

        struct file *file = fs_open(resolved, O_RDONLY);
        if (!file) {
            shell_error(sh, "cat: %s: no such file or directory", argv[i]);
            status = 1;
            continue;
        }

        char    chunk[1024];
        ssize_t got;
        while ((got = fs_read(file, chunk, sizeof(chunk) - 1)) > 0) {
            chunk[got] = '\0';
            shell_write(sh, chunk, (size_t)got);
        }
        fs_close(file);
    }
    return status;
}

static int cmd_head(struct shell *sh, int argc, char **argv)
{
    int         lines = 10;
    const char *path  = NULL;

    for (int i = 1; i < argc; i++) {
        if (strcmp(argv[i], "-n") == 0 && i + 1 < argc) {
            lines = atoi(argv[++i]);
        } else if (argv[i][0] == '-' && isdigit((uint8_t)argv[i][1])) {
            lines = atoi(argv[i] + 1);
        } else {
            path = argv[i];
        }
    }

    char       *buffer = shell_scratch();
    const char *text   = NULL;

    if (!buffer) {
        shell_error(sh, "out of memory");
        return 1;
    }

    if (path) {
        char resolved[FS_PATH_MAX];
        shell_resolve(sh, path, resolved, sizeof(resolved));
        size_t got = 0;
        if (fs_read_file(resolved, buffer, shell_scratch_size() - 1, &got) != 0) {
            shell_error(sh, "head: %s: no such file or directory", path);
            return 1;
        }
        buffer[got] = '\0';
        text        = buffer;
    } else {
        text = pipe_input(sh);
        if (!text) {
            shell_error(sh, "usage: head [-n count] <file>");
            return 1;
        }
    }

    int printed = 0;
    for (const char *p = text; *p && printed < lines; p++) {
        char c = *p;
        shell_write(sh, &c, 1);
        if (c == '\n') {
            printed++;
        }
    }
    return 0;
}

static int cmd_tail(struct shell *sh, int argc, char **argv)
{
    int         lines = 10;
    const char *path  = NULL;

    for (int i = 1; i < argc; i++) {
        if (strcmp(argv[i], "-n") == 0 && i + 1 < argc) {
            lines = atoi(argv[++i]);
        } else if (argv[i][0] == '-' && isdigit((uint8_t)argv[i][1])) {
            lines = atoi(argv[i] + 1);
        } else {
            path = argv[i];
        }
    }

    char       *buffer = shell_scratch();
    const char *text   = NULL;

    if (!buffer) {
        shell_error(sh, "out of memory");
        return 1;
    }

    if (path) {
        char resolved[FS_PATH_MAX];
        shell_resolve(sh, path, resolved, sizeof(resolved));
        size_t got = 0;
        if (fs_read_file(resolved, buffer, shell_scratch_size() - 1, &got) != 0) {
            shell_error(sh, "tail: %s: no such file or directory", path);
            return 1;
        }
        buffer[got] = '\0';
        text        = buffer;
    } else {
        text = pipe_input(sh);
        if (!text) {
            shell_error(sh, "usage: tail [-n count] <file>");
            return 1;
        }
    }

    /* Walk backwards counting newlines to find the starting point. */
    size_t      len   = strlen(text);
    const char *start = text + len;
    int         seen  = 0;

    while (start > text) {
        if (start[-1] == '\n' && start != text + len) {
            if (++seen >= lines) {
                break;
            }
        }
        start--;
    }

    shell_puts(sh, start);
    return 0;
}

static int cmd_wc(struct shell *sh, int argc, char **argv)
{
    char       *buffer = shell_scratch();
    const char *text   = NULL;

    if (!buffer) {
        shell_error(sh, "out of memory");
        return 1;
    }
    const char *label = "";

    /*
     * Flags select which counts are printed; with none, all three are shown.
     * Parse them before looking for a path, or "wc -l" reads from a pipe and
     * then treats its own flag as a missing filename.
     */
    bool_t want_lines = false;
    bool_t want_words = false;
    bool_t want_chars = false;
    const char *path  = NULL;

    for (int i = 1; i < argc; i++) {
        if (argv[i][0] == '-' && argv[i][1] != '\0') {
            for (const char *f = argv[i] + 1; *f; f++) {
                switch (*f) {
                case 'l': want_lines = true; break;
                case 'w': want_words = true; break;
                case 'c': want_chars = true; break;
                default:
                    shell_error(sh, "wc: unknown option -%c", *f);
                    return 1;
                }
            }
        } else if (!path) {
            path = argv[i];
        }
    }

    if (!want_lines && !want_words && !want_chars) {
        want_lines = want_words = want_chars = true;
    }

    if (path) {
        char resolved[FS_PATH_MAX];
        shell_resolve(sh, path, resolved, sizeof(resolved));
        size_t got = 0;
        if (fs_read_file(resolved, buffer, shell_scratch_size() - 1, &got) != 0) {
            shell_error(sh, "wc: %s: no such file or directory", path);
            return 1;
        }
        buffer[got] = '\0';
        text        = buffer;
        label       = path;
    } else {
        text = pipe_input(sh);
        if (!text) {
            shell_error(sh, "usage: wc [-lwc] [file]");
            return 1;
        }
    }

    uint64_t lines = 0, words = 0, chars = 0;
    bool_t   in_word = false;

    for (const char *p = text; *p; p++) {
        chars++;
        if (*p == '\n') {
            lines++;
        }
        if (isspace((uint8_t)*p)) {
            in_word = false;
        } else if (!in_word) {
            in_word = true;
            words++;
        }
    }
    /* Count a final line that lacks a trailing newline. */
    if (chars > 0 && text[chars - 1] != '\n') {
        lines++;
    }

    if (want_lines) {
        shell_printf(sh, "%8llu", (unsigned long long)lines);
    }
    if (want_words) {
        shell_printf(sh, "%8llu", (unsigned long long)words);
    }
    if (want_chars) {
        shell_printf(sh, "%8llu", (unsigned long long)chars);
    }
    if (label[0]) {
        shell_printf(sh, " %s", label);
    }
    shell_printf(sh, "\n");
    return 0;
}

static int cmd_grep(struct shell *sh, int argc, char **argv)
{
    bool_t invert      = false;
    bool_t ignore_case = false;
    bool_t numbered    = false;
    bool_t count_only  = false;

    int index = 1;
    for (; index < argc && argv[index][0] == '-' && argv[index][1]; index++) {
        for (const char *flag = argv[index] + 1; *flag; flag++) {
            switch (*flag) {
            case 'v': invert = true;      break;
            case 'i': ignore_case = true; break;
            case 'n': numbered = true;    break;
            case 'c': count_only = true;  break;
            default:
                shell_error(sh, "grep: unknown option -- '%c'", *flag);
                return 1;
            }
        }
    }

    if (index >= argc) {
        shell_error(sh, "usage: grep [-vinc] <pattern> [file]");
        return 1;
    }

    const char *pattern = argv[index++];
    char       *buffer = shell_scratch();
    const char *text   = NULL;

    if (!buffer) {
        shell_error(sh, "out of memory");
        return 1;
    }

    if (index < argc) {
        char resolved[FS_PATH_MAX];
        shell_resolve(sh, argv[index], resolved, sizeof(resolved));
        size_t got = 0;
        if (fs_read_file(resolved, buffer, shell_scratch_size() - 1, &got) != 0) {
            shell_error(sh, "grep: %s: no such file or directory", argv[index]);
            return 1;
        }
        buffer[got] = '\0';
        text        = buffer;
    } else {
        text = pipe_input(sh);
        if (!text) {
            shell_error(sh, "grep: no input (supply a file or use a pipe)");
            return 1;
        }
    }

    /* Case-insensitive matching works on lowered copies. */
    char lowered_pattern[256];
    if (ignore_case) {
        size_t i = 0;
        for (; pattern[i] && i < sizeof(lowered_pattern) - 1; i++) {
            lowered_pattern[i] = (char)tolower((uint8_t)pattern[i]);
        }
        lowered_pattern[i] = '\0';
    }

    int         matches = 0;
    int         line_no = 0;
    const char *line    = text;

    while (*line) {
        const char *end = strchr(line, '\n');
        size_t      len = end ? (size_t)(end - line) : strlen(line);

        char current[1024];
        size_t copy = MIN(len, sizeof(current) - 1);
        memcpy(current, line, copy);
        current[copy] = '\0';
        line_no++;

        bool_t found;
        if (ignore_case) {
            char lowered[1024];
            for (size_t i = 0; i <= copy; i++) {
                lowered[i] = (char)tolower((uint8_t)current[i]);
            }
            found = strstr(lowered, lowered_pattern) != NULL;
        } else {
            found = strstr(current, pattern) != NULL;
        }

        if (found != (invert ? true : false)) {
            matches++;
            if (!count_only) {
                if (numbered) {
                    shell_printf(sh, "%d:", line_no);
                }
                shell_printf(sh, "%s\n", current);
            }
        }

        if (!end) {
            break;
        }
        line = end + 1;
    }

    if (count_only) {
        shell_printf(sh, "%d\n", matches);
    }
    return (matches > 0) ? 0 : 1;
}

static int cmd_sort(struct shell *sh, int argc, char **argv)
{
    bool_t reverse = false;
    bool_t unique  = false;
    const char *path = NULL;

    for (int i = 1; i < argc; i++) {
        if (strcmp(argv[i], "-r") == 0) {
            reverse = true;
        } else if (strcmp(argv[i], "-u") == 0) {
            unique = true;
        } else {
            path = argv[i];
        }
    }

    char       *buffer = shell_scratch();
    const char *text   = NULL;

    if (!buffer) {
        shell_error(sh, "out of memory");
        return 1;
    }

    if (path) {
        char resolved[FS_PATH_MAX];
        shell_resolve(sh, path, resolved, sizeof(resolved));
        size_t got = 0;
        if (fs_read_file(resolved, buffer, shell_scratch_size() - 1, &got) != 0) {
            shell_error(sh, "sort: %s: no such file or directory", path);
            return 1;
        }
        buffer[got] = '\0';
        text        = buffer;
    } else {
        text = pipe_input(sh);
        if (!text) {
            shell_error(sh, "usage: sort [-r] [-u] <file>");
            return 1;
        }
        strlcpy(buffer, text, shell_scratch_size());
        text = buffer;
    }

    /* Split into lines in place. */
    #define SORT_MAX_LINES 512
    static char *lines[SORT_MAX_LINES];
    int    count = 0;
    char  *work  = buffer;
    char  *save  = NULL;

    for (char *token = strtok_r(work, "\n", &save);
         token && count < SORT_MAX_LINES; token = strtok_r(NULL, "\n", &save)) {
        lines[count++] = token;
    }

    /* Insertion sort: input sizes here are small and it is stable. */
    for (int i = 1; i < count; i++) {
        char *key = lines[i];
        int   j   = i - 1;
        while (j >= 0 && (reverse ? strcmp(lines[j], key) < 0
                                  : strcmp(lines[j], key) > 0)) {
            lines[j + 1] = lines[j];
            j--;
        }
        lines[j + 1] = key;
    }

    for (int i = 0; i < count; i++) {
        if (unique && i > 0 && strcmp(lines[i], lines[i - 1]) == 0) {
            continue;
        }
        shell_printf(sh, "%s\n", lines[i]);
    }
    return 0;
}

static int cmd_uniq(struct shell *sh, int argc, char **argv)
{
    char       *buffer = shell_scratch();
    const char *text   = NULL;

    if (!buffer) {
        shell_error(sh, "out of memory");
        return 1;
    }

    if (argc > 1) {
        char resolved[FS_PATH_MAX];
        shell_resolve(sh, argv[1], resolved, sizeof(resolved));
        size_t got = 0;
        if (fs_read_file(resolved, buffer, shell_scratch_size() - 1, &got) != 0) {
            shell_error(sh, "uniq: %s: no such file or directory", argv[1]);
            return 1;
        }
        buffer[got] = '\0';
    } else {
        text = pipe_input(sh);
        if (!text) {
            shell_error(sh, "usage: uniq <file>");
            return 1;
        }
        strlcpy(buffer, text, shell_scratch_size());
    }

    char  previous[1024] = {0};
    char *save = NULL;
    bool_t first = true;

    for (char *token = strtok_r(buffer, "\n", &save); token;
         token       = strtok_r(NULL, "\n", &save)) {
        if (first || strcmp(token, previous) != 0) {
            shell_printf(sh, "%s\n", token);
            strlcpy(previous, token, sizeof(previous));
            first = false;
        }
    }
    return 0;
}

/* --- file manipulation ------------------------------------------------ */

static int cmd_echo(struct shell *sh, int argc, char **argv)
{
    bool_t newline = true;
    int    start   = 1;

    if (argc > 1 && strcmp(argv[1], "-n") == 0) {
        newline = false;
        start   = 2;
    }

    for (int i = start; i < argc; i++) {
        shell_printf(sh, "%s%s", argv[i], (i + 1 < argc) ? " " : "");
    }
    if (newline) {
        shell_printf(sh, "\n");
    }
    return 0;
}

static int cmd_touch(struct shell *sh, int argc, char **argv)
{
    if (argc < 2) {
        shell_error(sh, "usage: touch <file>...");
        return 1;
    }

    for (int i = 1; i < argc; i++) {
        char resolved[FS_PATH_MAX];
        shell_resolve(sh, argv[i], resolved, sizeof(resolved));

        struct fs_node *node = fs_lookup(resolved);
        if (node) {
            node->modified = rtc_unix_time();
            continue;
        }
        if (!fs_create(resolved, FS_FILE, 0644)) {
            shell_error(sh, "touch: cannot create '%s'", argv[i]);
            return 1;
        }
    }
    return 0;
}

static int cmd_mkdir(struct shell *sh, int argc, char **argv)
{
    if (argc < 2) {
        shell_error(sh, "usage: mkdir [-p] <directory>...");
        return 1;
    }

    bool_t parents = false;
    int    start   = 1;
    if (strcmp(argv[1], "-p") == 0) {
        parents = true;
        start   = 2;
    }

    for (int i = start; i < argc; i++) {
        char resolved[FS_PATH_MAX];
        shell_resolve(sh, argv[i], resolved, sizeof(resolved));

        if (parents) {
            /* Create each component in turn. */
            char partial[FS_PATH_MAX] = "";
            char work[FS_PATH_MAX];
            strlcpy(work, resolved, sizeof(work));

            char *save = NULL;
            for (char *token = strtok_r(work, "/", &save); token;
                 token       = strtok_r(NULL, "/", &save)) {
                strlcat(partial, "/", sizeof(partial));
                strlcat(partial, token, sizeof(partial));
                if (!fs_lookup(partial)) {
                    fs_mkdir(partial, 0755);
                }
            }
            continue;
        }

        int error = fs_mkdir(resolved, 0755);
        if (error != 0) {
            shell_error(sh, "mkdir: %s: %s", argv[i], qira_strerror(error));
            return 1;
        }
    }
    return 0;
}

static int cmd_rm(struct shell *sh, int argc, char **argv)
{
    if (argc < 2) {
        shell_error(sh, "usage: rm [-r] <file>...");
        return 1;
    }

    bool_t recursive = false;
    int    start     = 1;
    if (argv[1][0] == '-') {
        recursive = strchr(argv[1], 'r') != NULL;
        start     = 2;
    }

    int status = 0;
    for (int i = start; i < argc; i++) {
        char resolved[FS_PATH_MAX];
        shell_resolve(sh, argv[i], resolved, sizeof(resolved));

        struct fs_node *node = fs_lookup(resolved);
        if (!node) {
            shell_error(sh, "rm: %s: no such file or directory", argv[i]);
            status = 1;
            continue;
        }
        if (node->type == FS_DIR && node->children && !recursive) {
            shell_error(sh, "rm: %s: is a non-empty directory (use -r)", argv[i]);
            status = 1;
            continue;
        }

        /* Depth-first removal for -r. */
        if (recursive && node->type == FS_DIR) {
            struct fs_dirent entry;
            while (fs_readdir(node, 0, &entry) == 0) {
                char child[FS_PATH_MAX];
                snprintf(child, sizeof(child), "%s/%s", resolved, entry.name);
                char *sub_argv[] = {"rm", "-r", child};
                cmd_rm(sh, 3, sub_argv);
            }
        }

        int error = fs_unlink(resolved);
        if (error != 0) {
            shell_error(sh, "rm: %s: %s", argv[i], qira_strerror(error));
            status = 1;
        }
    }
    return status;
}

static int cmd_cp(struct shell *sh, int argc, char **argv)
{
    if (argc < 3) {
        shell_error(sh, "usage: cp <source> <destination>");
        return 1;
    }

    char source[FS_PATH_MAX], dest[FS_PATH_MAX];
    shell_resolve(sh, argv[1], source, sizeof(source));
    shell_resolve(sh, argv[2], dest, sizeof(dest));

    struct fs_node *src_node = fs_lookup(source);
    if (!src_node) {
        shell_error(sh, "cp: %s: no such file or directory", argv[1]);
        return 1;
    }
    if (src_node->type == FS_DIR) {
        shell_error(sh, "cp: %s: is a directory", argv[1]);
        return 1;
    }

    /* Copying into a directory keeps the original name. */
    struct fs_node *dest_node = fs_lookup(dest);
    if (dest_node && dest_node->type == FS_DIR) {
        strlcat(dest, "/", sizeof(dest));
        strlcat(dest, src_node->name, sizeof(dest));
    }

    struct file *in = fs_open(source, O_RDONLY);
    if (!in) {
        shell_error(sh, "cp: cannot read %s", argv[1]);
        return 1;
    }
    struct file *out = fs_open(dest, O_WRONLY | O_CREATE | O_TRUNC);
    if (!out) {
        fs_close(in);
        shell_error(sh, "cp: cannot write %s", argv[2]);
        return 1;
    }

    char    chunk[1024];
    ssize_t got;
    uint64_t total = 0;
    while ((got = fs_read(in, chunk, sizeof(chunk))) > 0) {
        fs_write(out, chunk, (size_t)got);
        total += (uint64_t)got;
    }

    fs_close(in);
    fs_close(out);
    return 0;
}

static int cmd_mv(struct shell *sh, int argc, char **argv)
{
    if (argc < 3) {
        shell_error(sh, "usage: mv <source> <destination>");
        return 1;
    }

    char source[FS_PATH_MAX], dest[FS_PATH_MAX];
    shell_resolve(sh, argv[1], source, sizeof(source));
    shell_resolve(sh, argv[2], dest, sizeof(dest));

    struct fs_node *dest_node = fs_lookup(dest);
    struct fs_node *src_node  = fs_lookup(source);
    if (!src_node) {
        shell_error(sh, "mv: %s: no such file or directory", argv[1]);
        return 1;
    }
    if (dest_node && dest_node->type == FS_DIR) {
        strlcat(dest, "/", sizeof(dest));
        strlcat(dest, src_node->name, sizeof(dest));
    }

    int error = fs_rename(source, dest);
    if (error != 0) {
        shell_error(sh, "mv: %s", qira_strerror(error));
        return 1;
    }
    return 0;
}

static int cmd_write(struct shell *sh, int argc, char **argv)
{
    if (argc < 3) {
        shell_error(sh, "usage: write <file> <text>...");
        return 1;
    }

    char resolved[FS_PATH_MAX];
    shell_resolve(sh, argv[1], resolved, sizeof(resolved));

    char content[2048] = "";
    for (int i = 2; i < argc; i++) {
        if (i > 2) {
            strlcat(content, " ", sizeof(content));
        }
        strlcat(content, argv[i], sizeof(content));
    }
    strlcat(content, "\n", sizeof(content));

    int error = fs_write_file(resolved, content, strlen(content));
    if (error != 0) {
        shell_error(sh, "write: %s: %s", argv[1], qira_strerror(error));
        return 1;
    }
    return 0;
}

static int cmd_stat(struct shell *sh, int argc, char **argv)
{
    if (argc < 2) {
        shell_error(sh, "usage: stat <path>");
        return 1;
    }

    char resolved[FS_PATH_MAX];
    shell_resolve(sh, argv[1], resolved, sizeof(resolved));

    struct fs_stat st;
    int error = fs_stat(resolved, &st);
    if (error != 0) {
        shell_error(sh, "stat: %s: %s", argv[1], qira_strerror(error));
        return 1;
    }

    char perms[12];
    fs_format_permissions(st.permissions, (fs_node_type_t)st.type, perms);

    struct qira_time created, modified;
    time_from_unix(st.created, &created);
    time_from_unix(st.modified, &modified);

    char created_text[32], modified_text[32];
    time_format(&created, created_text, sizeof(created_text));
    time_format(&modified, modified_text, sizeof(modified_text));

    shell_printf(sh, "  File: %s\n", resolved);
    shell_printf(sh, "  Size: %llu bytes\n", (unsigned long long)st.size);
    shell_printf(sh, "  Type: %s\n", fs_type_name((fs_node_type_t)st.type));
    shell_printf(sh, " Inode: %llu\n", (unsigned long long)st.inode);
    shell_printf(sh, "Access: %s (%04o)  Uid: %u  Gid: %u\n", perms,
                 st.permissions, st.uid, st.gid);
    shell_printf(sh, "Create: %s\n", created_text);
    shell_printf(sh, "Modify: %s\n", modified_text);
    return 0;
}

static int cmd_find(struct shell *sh, int argc, char **argv)
{
    const char *start_path = (argc > 1) ? argv[1] : ".";
    const char *pattern    = NULL;

    for (int i = 2; i < argc - 1; i++) {
        if (strcmp(argv[i], "-name") == 0) {
            pattern = argv[i + 1];
        }
    }

    char resolved[FS_PATH_MAX];
    shell_resolve(sh, start_path, resolved, sizeof(resolved));

    /* Iterative depth-first walk with an explicit stack. */
    #define FIND_STACK 64
    static char stack[FIND_STACK][FS_PATH_MAX];
    int depth = 0;
    strlcpy(stack[depth++], resolved, FS_PATH_MAX);

    int found = 0;
    while (depth > 0) {
        char current[FS_PATH_MAX];
        strlcpy(current, stack[--depth], sizeof(current));

        struct fs_node *node = fs_lookup(current);
        if (!node) {
            continue;
        }

        bool_t matches = true;
        if (pattern) {
            matches = strstr(node->name, pattern) != NULL;
        }
        if (matches) {
            shell_printf(sh, "%s\n", current);
            found++;
        }

        if (node->type == FS_DIR) {
            struct fs_dirent entry;
            for (int i = 0; fs_readdir(node, i, &entry) == 0; i++) {
                if (depth >= FIND_STACK) {
                    break;
                }
                snprintf(stack[depth], FS_PATH_MAX, "%s%s%s", current,
                         (strcmp(current, "/") == 0) ? "" : "/", entry.name);
                depth++;
            }
        }
    }

    return (found > 0) ? 0 : 1;
}

static int cmd_tree(struct shell *sh, int argc, char **argv)
{
    const char *path = (argc > 1) ? argv[1] : ".";
    char resolved[FS_PATH_MAX];
    shell_resolve(sh, path, resolved, sizeof(resolved));

    struct fs_node *root = fs_lookup(resolved);
    if (!root) {
        shell_error(sh, "tree: %s: no such file or directory", path);
        return 1;
    }

    shell_printf(sh, "%s\n", resolved);

    /* Recursion is bounded by the prefix buffer. */
    struct frame {
        struct fs_node *node;
        int             index;
    };
    static struct frame stack[32];
    char prefix[128] = "";
    int  depth       = 0;

    stack[depth].node  = root;
    stack[depth].index = 0;
    depth++;

    int files = 0, dirs = 0;

    while (depth > 0) {
        struct frame *frame = &stack[depth - 1];
        struct fs_dirent entry;

        if (fs_readdir(frame->node, frame->index, &entry) != 0) {
            depth--;
            size_t len = strlen(prefix);
            if (len >= 4) {
                prefix[len - 4] = '\0';
            }
            continue;
        }
        frame->index++;

        struct fs_dirent peek;
        bool_t last = (fs_readdir(frame->node, frame->index, &peek) != 0);

        shell_printf(sh, "%s%s%s%s\n", prefix, last ? "`-- " : "|-- ", entry.name,
                     (entry.type == FS_DIR) ? "/" : "");

        if (entry.type == FS_DIR) {
            dirs++;
            if (depth < 32) {
                /* Descend into the child directory. */
                char child_path[FS_PATH_MAX];
                snprintf(child_path, sizeof(child_path), "%s/%s",
                         (depth == 1) ? resolved : "", entry.name);

                struct fs_node *child = NULL;
                for (struct fs_node *c = frame->node->children; c; c = c->sibling) {
                    if (strcmp(c->name, entry.name) == 0) {
                        child = c;
                        break;
                    }
                }
                if (child) {
                    strlcat(prefix, last ? "    " : "|   ", sizeof(prefix));
                    stack[depth].node  = child;
                    stack[depth].index = 0;
                    depth++;
                }
            }
        } else {
            files++;
        }
    }

    shell_printf(sh, "\n%d directories, %d files\n", dirs, files);
    return 0;
}

/* --- text utilities --------------------------------------------------- */

static int cmd_rev(struct shell *sh, int argc, char **argv)
{
    const char *text = (argc > 1) ? argv[1] : pipe_input(sh);
    if (!text) {
        shell_error(sh, "usage: rev <text>");
        return 1;
    }

    char buffer[1024];
    strlcpy(buffer, text, sizeof(buffer));
    size_t len = strlen(buffer);

    /* Strip a trailing newline so it stays at the end. */
    bool_t had_newline = (len > 0 && buffer[len - 1] == '\n');
    if (had_newline) {
        buffer[--len] = '\0';
    }

    for (size_t i = 0; i < len / 2; i++) {
        char swap          = buffer[i];
        buffer[i]          = buffer[len - 1 - i];
        buffer[len - 1 - i] = swap;
    }

    shell_printf(sh, "%s\n", buffer);
    return 0;
}

static int cmd_upper(struct shell *sh, int argc, char **argv)
{
    const char *text = (argc > 1) ? argv[1] : pipe_input(sh);
    if (!text) {
        shell_error(sh, "usage: upper <text>");
        return 1;
    }
    for (const char *p = text; *p; p++) {
        char c = (char)toupper((uint8_t)*p);
        shell_write(sh, &c, 1);
    }
    if (argc > 1) {
        shell_printf(sh, "\n");
    }
    return 0;
}

static int cmd_lower(struct shell *sh, int argc, char **argv)
{
    const char *text = (argc > 1) ? argv[1] : pipe_input(sh);
    if (!text) {
        shell_error(sh, "usage: lower <text>");
        return 1;
    }
    for (const char *p = text; *p; p++) {
        char c = (char)tolower((uint8_t)*p);
        shell_write(sh, &c, 1);
    }
    if (argc > 1) {
        shell_printf(sh, "\n");
    }
    return 0;
}

/* --- arithmetic ------------------------------------------------------- */

/*
 * A small recursive-descent expression evaluator supporting
 * + - * / % ( ) and unary minus over 64-bit integers.
 */
struct calc_state {
    const char *p;
    bool_t      error;
};

static int64_t calc_expr(struct calc_state *st);

static void calc_skip(struct calc_state *st)
{
    while (isspace((uint8_t)*st->p)) {
        st->p++;
    }
}

static int64_t calc_primary(struct calc_state *st)
{
    calc_skip(st);

    if (*st->p == '(') {
        st->p++;
        int64_t value = calc_expr(st);
        calc_skip(st);
        if (*st->p == ')') {
            st->p++;
        } else {
            st->error = true;
        }
        return value;
    }
    if (*st->p == '-') {
        st->p++;
        return -calc_primary(st);
    }
    if (*st->p == '+') {
        st->p++;
        return calc_primary(st);
    }
    if (!isdigit((uint8_t)*st->p)) {
        st->error = true;
        return 0;
    }

    char *end     = NULL;
    int64_t value = (int64_t)strtoul(st->p, &end, 0);
    st->p         = end;
    return value;
}

static int64_t calc_term(struct calc_state *st)
{
    int64_t value = calc_primary(st);

    for (;;) {
        calc_skip(st);
        char op = *st->p;
        if (op != '*' && op != '/' && op != '%') {
            return value;
        }
        st->p++;
        int64_t rhs = calc_primary(st);

        if ((op == '/' || op == '%') && rhs == 0) {
            st->error = true;
            return 0;
        }
        if (op == '*') {
            value *= rhs;
        } else if (op == '/') {
            value /= rhs;
        } else {
            value %= rhs;
        }
    }
}

static int64_t calc_expr(struct calc_state *st)
{
    int64_t value = calc_term(st);

    for (;;) {
        calc_skip(st);
        char op = *st->p;
        if (op != '+' && op != '-') {
            return value;
        }
        st->p++;
        int64_t rhs = calc_term(st);
        value = (op == '+') ? value + rhs : value - rhs;
    }
}

static int cmd_calc(struct shell *sh, int argc, char **argv)
{
    if (argc < 2) {
        shell_error(sh, "usage: calc <expression>    e.g. calc '(2 + 3) * 8'");
        return 1;
    }

    char expression[512] = "";
    for (int i = 1; i < argc; i++) {
        if (i > 1) {
            strlcat(expression, " ", sizeof(expression));
        }
        strlcat(expression, argv[i], sizeof(expression));
    }

    struct calc_state st = {expression, false};
    int64_t           value = calc_expr(&st);

    calc_skip(&st);
    if (st.error || *st.p) {
        shell_error(sh, "calc: cannot parse '%s'", expression);
        return 1;
    }

    shell_printf(sh, "%lld\n", (long long)value);
    return 0;
}

/* --- variables and scripting ------------------------------------------ */

static int cmd_set(struct shell *sh, int argc, char **argv)
{
    if (argc == 1) {
        for (int i = 0; i < shell_var_count(sh); i++) {
            const struct shell_var *var = shell_var_at(sh, i);
            shell_printf(sh, "%s=%s\n", var->name, var->value);
        }
        return 0;
    }

    /* set NAME=VALUE  or  set NAME VALUE */
    char *equals = strchr(argv[1], '=');
    if (equals) {
        *equals = '\0';
        shell_set_var(sh, argv[1], equals + 1);
        return 0;
    }
    if (argc >= 3) {
        shell_set_var(sh, argv[1], argv[2]);
        return 0;
    }

    const char *value = shell_get_var(sh, argv[1]);
    shell_printf(sh, "%s=%s\n", argv[1], value ? value : "");
    return 0;
}

static int cmd_unset(struct shell *sh, int argc, char **argv)
{
    if (argc < 2) {
        shell_error(sh, "usage: unset <name>");
        return 1;
    }
    if (shell_unset_var(sh, argv[1]) != 0) {
        shell_error(sh, "unset: %s: not set or read-only", argv[1]);
        return 1;
    }
    return 0;
}

static int cmd_env(struct shell *sh, int argc, char **argv)
{
    UNUSED(argc);
    UNUSED(argv);
    for (int i = 0; i < shell_var_count(sh); i++) {
        const struct shell_var *var = shell_var_at(sh, i);
        shell_printf(sh, "%s=%s\n", var->name, var->value);
    }
    return 0;
}

static int cmd_source(struct shell *sh, int argc, char **argv)
{
    if (argc < 2) {
        shell_error(sh, "usage: source <script>");
        return 1;
    }

    char resolved[FS_PATH_MAX];
    shell_resolve(sh, argv[1], resolved, sizeof(resolved));

    char  *buffer = shell_scratch();
    size_t got    = 0;

    if (!buffer) {
        shell_error(sh, "out of memory");
        return 1;
    }
    if (fs_read_file(resolved, buffer, shell_scratch_size() - 1, &got) != 0) {
        shell_error(sh, "source: %s: no such file or directory", argv[1]);
        return 1;
    }
    buffer[got] = '\0';

    int   status = 0;
    char *save   = NULL;
    for (char *line = strtok_r(buffer, "\n", &save); line;
         line       = strtok_r(NULL, "\n", &save)) {
        if (sh->echo_commands) {
            shell_printf(sh, "+ %s\n", line);
        }
        status = shell_execute_line(sh, line);
    }
    return status;
}

static int cmd_repeat(struct shell *sh, int argc, char **argv)
{
    if (argc < 3) {
        shell_error(sh, "usage: repeat <count> <command>...");
        return 1;
    }

    int count = atoi(argv[1]);
    if (count < 1 || count > 1000) {
        shell_error(sh, "repeat: count must be between 1 and 1000");
        return 1;
    }

    char command[SHELL_LINE_MAX] = "";
    for (int i = 2; i < argc; i++) {
        if (i > 2) {
            strlcat(command, " ", sizeof(command));
        }
        strlcat(command, argv[i], sizeof(command));
    }

    int status = 0;
    for (int i = 0; i < count; i++) {
        status = shell_execute_line(sh, command);
    }
    return status;
}

static int cmd_test(struct shell *sh, int argc, char **argv)
{
    /* test -e/-f/-d <path>, or test <a> = <b> */
    if (argc == 3 && argv[1][0] == '-') {
        char resolved[FS_PATH_MAX];
        shell_resolve(sh, argv[2], resolved, sizeof(resolved));
        struct fs_node *node = fs_lookup(resolved);

        switch (argv[1][1]) {
        case 'e': return node ? 0 : 1;
        case 'f': return (node && node->type == FS_FILE) ? 0 : 1;
        case 'd': return (node && node->type == FS_DIR) ? 0 : 1;
        case 's': return (node && node->size > 0) ? 0 : 1;
        case 'z': return (argv[2][0] == '\0') ? 0 : 1;
        case 'n': return (argv[2][0] != '\0') ? 0 : 1;
        default:
            shell_error(sh, "test: unknown operator '%s'", argv[1]);
            return 2;
        }
    }
    if (argc == 4) {
        if (strcmp(argv[2], "=") == 0) {
            return strcmp(argv[1], argv[3]) == 0 ? 0 : 1;
        }
        if (strcmp(argv[2], "!=") == 0) {
            return strcmp(argv[1], argv[3]) != 0 ? 0 : 1;
        }
        if (strcmp(argv[2], "-eq") == 0) {
            return atoi(argv[1]) == atoi(argv[3]) ? 0 : 1;
        }
        if (strcmp(argv[2], "-ne") == 0) {
            return atoi(argv[1]) != atoi(argv[3]) ? 0 : 1;
        }
        if (strcmp(argv[2], "-lt") == 0) {
            return atoi(argv[1]) < atoi(argv[3]) ? 0 : 1;
        }
        if (strcmp(argv[2], "-gt") == 0) {
            return atoi(argv[1]) > atoi(argv[3]) ? 0 : 1;
        }
    }

    shell_error(sh, "usage: test -e|-f|-d|-s <path>  |  test <a> =|!=|-eq|-lt|-gt <b>");
    return 2;
}

/* --- system ----------------------------------------------------------- */

static int cmd_clear(struct shell *sh, int argc, char **argv)
{
    UNUSED(sh);
    UNUSED(argc);
    UNUSED(argv);
    console_clear();
    return 0;
}

static int cmd_date(struct shell *sh, int argc, char **argv)
{
    UNUSED(argc);
    UNUSED(argv);

    struct qira_time now;
    time_from_unix(rtc_unix_time(), &now);

    static const char *weekdays[] = {"Sunday",   "Monday", "Tuesday", "Wednesday",
                                     "Thursday", "Friday", "Saturday"};
    static const char *months[]   = {"January",   "February", "March",    "April",
                                     "May",       "June",     "July",     "August",
                                     "September", "October",  "November", "December"};

    shell_printf(sh, "%s, %d %s %d %02d:%02d:%02d UTC\n",
                 weekdays[now.weekday % 7], now.day, months[(now.month - 1) % 12],
                 now.year, now.hour, now.minute, now.second);
    return 0;
}

static int cmd_uptime(struct shell *sh, int argc, char **argv)
{
    UNUSED(argc);
    UNUSED(argv);

    uint64_t ms      = time_uptime_ms();
    uint64_t seconds = ms / 1000;

    shell_printf(sh, "up %llu day%s, %02llu:%02llu:%02llu  (%d tasks)\n",
                 (unsigned long long)(seconds / 86400),
                 (seconds / 86400 == 1) ? "" : "s",
                 (unsigned long long)((seconds % 86400) / 3600),
                 (unsigned long long)((seconds % 3600) / 60),
                 (unsigned long long)(seconds % 60), sched_task_count());
    return 0;
}

static int cmd_whoami(struct shell *sh, int argc, char **argv)
{
    UNUSED(argc);
    UNUSED(argv);

    struct task *task = sched_current();
    const char  *user = shell_get_var(sh, "USER");
    shell_printf(sh, "%s (uid %u)\n", user ? user : "user", task ? task->uid : 0);
    return 0;
}

static int cmd_which(struct shell *sh, int argc, char **argv)
{
    if (argc < 2) {
        shell_error(sh, "usage: which <command>");
        return 1;
    }

    const struct shell_command *cmd = shell_find_command(sh, argv[1]);
    if (cmd) {
        shell_printf(sh, "%s: shell builtin (%s)\n", argv[1], sh->name);
        return 0;
    }
    const char *alias = shell_get_alias(sh, argv[1]);
    if (alias) {
        shell_printf(sh, "%s: aliased to '%s'\n", argv[1], alias);
        return 0;
    }
    shell_printf(sh, "%s: not found\n", argv[1]);
    return 1;
}

static int cmd_sleep(struct shell *sh, int argc, char **argv)
{
    if (argc < 2) {
        shell_error(sh, "usage: sleep <seconds>");
        return 1;
    }
    int seconds = atoi(argv[1]);
    if (seconds < 0 || seconds > 60) {
        shell_error(sh, "sleep: duration must be between 0 and 60 seconds");
        return 1;
    }
    sched_sleep_ms((uint64_t)seconds * 1000);
    return 0;
}

static int cmd_version(struct shell *sh, int argc, char **argv)
{
    UNUSED(argc);
    UNUSED(argv);

    shell_color(sh, "\033[96m");
    shell_printf(sh, "UltraShell");
    shell_reset_color(sh);
    shell_printf(sh, " - the Qira OS general-purpose shell\n");
    shell_printf(sh, "Part of %s %s (%s)\n", QIRA_PROJECT_NAME,
                 QIRA_VERSION_STRING, QIRA_CODENAME);
    shell_printf(sh, "Build %s, %s\n", QIRA_BUILD_ID, QIRA_BUILD_DATE);
    shell_printf(sh, "%d built-in commands available.\n", sh->command_count);
    return 0;
}

static int cmd_qcsh(struct shell *sh, int argc, char **argv)
{
    struct shell *config = qcsh_instance();

    if (argc > 1) {
        /* Run a single QCSH command and return. */
        char line[SHELL_LINE_MAX] = "";
        for (int i = 1; i < argc; i++) {
            if (i > 1) {
                strlcat(line, " ", sizeof(line));
            }
            strlcat(line, argv[i], sizeof(line));
        }
        struct shell_sink *saved = config->sink;
        config->sink             = sh->sink;
        int status               = shell_execute_line(config, line);
        config->sink             = saved;
        return status;
    }

    /*
     * Ask the host to switch shells rather than nesting a read loop here:
     * inside a desktop terminal there is no console to block on.
     */
    sh->switch_request = "qcsh";
    config->running    = true;
    shell_printf(sh, "Switching to QiraConfigShell. Type 'ush' to return.\n");
    return 0;
}

/* --- command table ---------------------------------------------------- */

static const struct shell_command commands[] = {
    /* navigation */
    {"pwd", "print the working directory", "pwd", NULL, cmd_pwd, 0},
    {"cd", "change the working directory", "cd [directory|-]",
     "With no argument, changes to $HOME. 'cd -' returns to the previous\n"
     "directory, which is remembered in $OLDPWD.",
     cmd_cd, 0},
    {"ls", "list directory contents", "ls [-lah] [path]",
     "  -l  long format with permissions, size and modification time\n"
     "  -a  include entries beginning with a dot\n"
     "  -h  human readable sizes",
     cmd_ls, 0},
    {"tree", "show a directory tree", "tree [path]", NULL, cmd_tree, 0},
    {"find", "search for files by name", "find [path] [-name pattern]", NULL,
     cmd_find, 0},

    /* reading files */
    {"cat", "print file contents", "cat <file>...",
     "With no arguments, echoes piped input.", cmd_cat, 0},
    {"head", "print the first lines of a file", "head [-n count] [file]", NULL,
     cmd_head, 0},
    {"tail", "print the last lines of a file", "tail [-n count] [file]", NULL,
     cmd_tail, 0},
    {"wc", "count lines, words and characters", "wc [-lwc] [file]",
     "  -l  count lines only\n"
     "  -w  count words only\n"
     "  -c  count characters only\n\n"
     "With no flags all three counts are printed. Reads from a pipe when no\n"
     "file is given, so 'ls /etc | wc -l' works.",
     cmd_wc, 0},
    {"grep", "search text for a pattern", "grep [-vinc] <pattern> [file]",
     "  -v  invert the match\n"
     "  -i  ignore case\n"
     "  -n  prefix each line with its number\n"
     "  -c  print only the number of matching lines\n\n"
     "Reads from a pipe when no file is given, so 'cat x | grep foo' works.",
     cmd_grep, 0},
    {"sort", "sort lines of text", "sort [-r] [-u] [file]", NULL, cmd_sort, 0},
    {"uniq", "collapse repeated adjacent lines", "uniq [file]", NULL, cmd_uniq, 0},
    {"stat", "show detailed file information", "stat <path>", NULL, cmd_stat, 0},

    /* writing files */
    {"echo", "print text", "echo [-n] <text>...", NULL, cmd_echo, 0},
    {"touch", "create a file or update its timestamp", "touch <file>...", NULL,
     cmd_touch, 0},
    {"mkdir", "create directories", "mkdir [-p] <directory>...", NULL, cmd_mkdir, 0},
    {"rm", "remove files and directories", "rm [-r] <path>...", NULL, cmd_rm, 0},
    {"cp", "copy a file", "cp <source> <destination>", NULL, cmd_cp, 0},
    {"mv", "move or rename a file", "mv <source> <destination>", NULL, cmd_mv, 0},
    {"write", "write text to a file", "write <file> <text>...",
     "Overwrites the file. Use output redirection for more control:\n"
     "  echo hello > /tmp/greeting",
     cmd_write, 0},

    /* text utilities */
    {"rev", "reverse a string", "rev <text>", NULL, cmd_rev, 0},
    {"upper", "convert text to upper case", "upper <text>", NULL, cmd_upper, 0},
    {"lower", "convert text to lower case", "lower <text>", NULL, cmd_lower, 0},
    {"calc", "evaluate an arithmetic expression", "calc <expression>",
     "Supports + - * / % and parentheses over 64-bit integers.\n"
     "Example: calc '(17 + 5) * 3'",
     cmd_calc, 0},

    /* environment and scripting */
    {"set", "set or list shell variables", "set [NAME=VALUE]", NULL, cmd_set, 0},
    {"unset", "remove a shell variable", "unset <name>", NULL, cmd_unset, 0},
    {"env", "list all shell variables", "env", NULL, cmd_env, 0},
    {"alias", "define or list aliases", "alias [name=value]", NULL,
     shell_builtin_alias, 0},
    {"unalias", "remove an alias", "unalias <name>", NULL, shell_builtin_unalias, 0},
    {"source", "run commands from a file", "source <script>", NULL, cmd_source, 0},
    {"repeat", "run a command several times", "repeat <count> <command>...", NULL,
     cmd_repeat, 0},
    {"test", "evaluate a condition", "test -e|-f|-d <path> | test <a> = <b>",
     "Returns 0 when the condition holds, which makes it useful with && and ||:\n"
     "  test -f /etc/motd && cat /etc/motd",
     cmd_test, 0},

    /* system */
    {"clear", "clear the screen", "clear", NULL, cmd_clear, 0},
    {"date", "show the current date and time", "date", NULL, cmd_date, 0},
    {"uptime", "show how long the system has been running", "uptime", NULL,
     cmd_uptime, 0},
    {"whoami", "print the current user", "whoami", NULL, cmd_whoami, 0},
    {"which", "locate a command", "which <command>", NULL, cmd_which, 0},
    {"sleep", "pause for a number of seconds", "sleep <seconds>", NULL, cmd_sleep, 0},
    {"history", "show the command history", "history [-c]", NULL,
     shell_builtin_history, 0},
    {"help", "list commands or describe one", "help [command]", NULL,
     shell_builtin_help, 0},
    {"version", "show shell and system version", "version", NULL, cmd_version, 0},
    {"qcsh", "run a QiraConfigShell command", "qcsh [command...]",
     "With no arguments, switches to an interactive QCSH session.\n"
     "Otherwise runs a single QCSH command and returns the result here.",
     cmd_qcsh, 0},
    {"exit", "leave the shell", "exit [status]", NULL, shell_builtin_exit, 0},
};

/*
 * Commands from the networking and formats module are appended here, so
 * ultrashell.c stays about the shell rather than growing a section for every
 * subsystem. The merged table is built once at startup.
 */
extern const struct shell_command *ultrashell_net_commands(int *count);
extern const struct shell_command *ultrashell_sys_commands(int *count);

#define ULTRASHELL_MAX_COMMANDS 80
static struct shell_command merged[ULTRASHELL_MAX_COMMANDS];
static int                  merged_count;

static void build_command_table(void)
{
    merged_count = 0;

    for (size_t i = 0;
         i < ARRAY_SIZE(commands) && merged_count < ULTRASHELL_MAX_COMMANDS;
         i++) {
        merged[merged_count++] = commands[i];
    }

    const struct shell_command *(*modules[])(int *) = {
        ultrashell_net_commands,
        ultrashell_sys_commands,
    };

    for (size_t m = 0; m < ARRAY_SIZE(modules); m++) {
        int extra_count = 0;
        const struct shell_command *extra = modules[m](&extra_count);

        for (int i = 0;
             i < extra_count && merged_count < ULTRASHELL_MAX_COMMANDS; i++) {
            merged[merged_count++] = extra[i];
        }
    }
}

const struct shell_command *ultrashell_commands(int *count)
{
    if (merged_count == 0) {
        build_command_table();
    }
    *count = merged_count;
    return merged;
}

struct shell *ultrashell_instance(void)
{
    return &ultrashell;
}

void ultrashell_init(void)
{
    build_command_table();
    shell_init(&ultrashell, "ush", merged, merged_count);

    /* Convenience aliases familiar from other systems. */
    shell_set_alias(&ultrashell, "ll", "ls -l");
    shell_set_alias(&ultrashell, "la", "ls -la");
    shell_set_alias(&ultrashell, "dir", "ls");
    shell_set_alias(&ultrashell, "cls", "clear");
    shell_set_alias(&ultrashell, "type", "cat");
    shell_set_alias(&ultrashell, "md", "mkdir");
    shell_set_alias(&ultrashell, "..", "cd ..");

    KLOG_INFO("ultrashell", "%d commands registered",
              (int)ARRAY_SIZE(commands));
}
