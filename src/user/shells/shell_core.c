/*
 * Qira OS - shared shell engine
 *
 * Implements parsing, expansion, pipelines, redirection, variables, aliases,
 * history and line editing on behalf of both QCSH and UltraShell.
 *
 * Commands are C functions rather than separate executables. Pipelines are
 * therefore implemented by capturing a command's output into a buffer and
 * feeding it to the next stage through the QIRA_PIPE_INPUT variable, which
 * gives genuine `a | b | c` semantics without needing fork().
 */

#include <kernel/shell.h>
#include <kernel/console.h>
#include <kernel/string.h>
#include <kernel/printf.h>
#include <kernel/mm.h>
#include <kernel/fs.h>
#include <kernel/log.h>
#include <kernel/sched.h>
#include <kernel/input.h>
#include <kernel/syscall.h>

#define PIPE_BUFFER_SIZE 32768

void shell_init(struct shell *sh, const char *name,
                const struct shell_command *commands, int count)
{
    memset(sh, 0, sizeof(*sh));

    sh->name          = name;
    sh->commands      = commands;
    sh->command_count = count;
    sh->running       = true;
    sh->last_status   = 0;
    strlcpy(sh->cwd, "/home/user", sizeof(sh->cwd));

    /* Baseline environment. */
    shell_set_var(sh, "SHELL", name);
    shell_set_var(sh, "HOME", "/home/user");
    shell_set_var(sh, "PATH", "/bin:/usr/bin");
    shell_set_var(sh, "USER", "user");
    shell_set_var(sh, "PWD", sh->cwd);
    shell_set_var(sh, "TERM", "qira");
    shell_set_var(sh, "COLOR", "1");
}

/* --- shared scratch buffer -------------------------------------------- */

static char *scratch_buffer;

char *shell_scratch(void)
{
    if (!scratch_buffer) {
        scratch_buffer = kmalloc(SHELL_SCRATCH_SIZE);
        if (scratch_buffer) {
            scratch_buffer[0] = '\0';
        }
    }
    return scratch_buffer;
}

size_t shell_scratch_size(void)
{
    return SHELL_SCRATCH_SIZE;
}

/* --- output ----------------------------------------------------------- */

void shell_write(struct shell *sh, const char *text, size_t len)
{
    if (sh->sink && sh->sink->buffer) {
        struct shell_sink *sink = sh->sink;
        size_t space = (sink->capacity > sink->length + 1)
                           ? sink->capacity - sink->length - 1
                           : 0;
        size_t copy = MIN(len, space);
        if (copy < len) {
            sink->truncated = true;
        }
        memcpy(sink->buffer + sink->length, text, copy);
        sink->length += copy;
        sink->buffer[sink->length] = '\0';
        return;
    }
    console_write(text, len);
}

void shell_puts(struct shell *sh, const char *text)
{
    shell_write(sh, text, strlen(text));
}

void shell_printf(struct shell *sh, const char *fmt, ...)
{
    char    buf[2048];
    va_list ap;

    va_start(ap, fmt);
    int n = vsnprintf(buf, sizeof(buf), fmt, ap);
    va_end(ap);

    if (n > 0) {
        shell_write(sh, buf, MIN((size_t)n, sizeof(buf) - 1));
    }
}

void shell_error(struct shell *sh, const char *fmt, ...)
{
    char    buf[1024];
    va_list ap;

    va_start(ap, fmt);
    vsnprintf(buf, sizeof(buf), fmt, ap);
    va_end(ap);

    shell_color(sh, "\033[91m");
    shell_printf(sh, "%s: %s\n", sh->name, buf);
    shell_reset_color(sh);
}

static bool_t color_enabled(struct shell *sh)
{
    const char *value = shell_get_var(sh, "COLOR");
    return !sh->sink && value && value[0] != '0';
}

void shell_color(struct shell *sh, const char *ansi)
{
    if (color_enabled(sh)) {
        shell_puts(sh, ansi);
    }
}

void shell_reset_color(struct shell *sh)
{
    if (color_enabled(sh)) {
        shell_puts(sh, "\033[0m");
    }
}

/* --- variables -------------------------------------------------------- */

static struct shell_var *find_var(struct shell *sh, const char *name)
{
    for (int i = 0; i < sh->var_count; i++) {
        if (strcmp(sh->vars[i].name, name) == 0) {
            return &sh->vars[i];
        }
    }
    return NULL;
}

const char *shell_get_var(struct shell *sh, const char *name)
{
    struct shell_var *var = find_var(sh, name);
    return var ? var->value : NULL;
}

int shell_set_var(struct shell *sh, const char *name, const char *value)
{
    struct shell_var *var = find_var(sh, name);

    if (var) {
        if (var->readonly) {
            return -1;
        }
        strlcpy(var->value, value, sizeof(var->value));
        return 0;
    }
    if (sh->var_count >= SHELL_VARS_MAX) {
        return -1;
    }

    var = &sh->vars[sh->var_count++];
    strlcpy(var->name, name, sizeof(var->name));
    strlcpy(var->value, value, sizeof(var->value));
    var->exported = false;
    var->readonly = false;
    return 0;
}

int shell_unset_var(struct shell *sh, const char *name)
{
    for (int i = 0; i < sh->var_count; i++) {
        if (strcmp(sh->vars[i].name, name) != 0) {
            continue;
        }
        if (sh->vars[i].readonly) {
            return -1;
        }
        sh->vars[i] = sh->vars[--sh->var_count];
        return 0;
    }
    return -1;
}

int shell_var_count(struct shell *sh)
{
    return sh->var_count;
}

const struct shell_var *shell_var_at(struct shell *sh, int index)
{
    if (index < 0 || index >= sh->var_count) {
        return NULL;
    }
    return &sh->vars[index];
}

/* --- aliases ---------------------------------------------------------- */

int shell_set_alias(struct shell *sh, const char *name, const char *expansion)
{
    for (int i = 0; i < sh->alias_count; i++) {
        if (strcmp(sh->aliases[i].name, name) == 0) {
            strlcpy(sh->aliases[i].expansion, expansion,
                    sizeof(sh->aliases[i].expansion));
            return 0;
        }
    }
    if (sh->alias_count >= SHELL_ALIAS_MAX) {
        return -1;
    }

    struct shell_alias *alias = &sh->aliases[sh->alias_count++];
    strlcpy(alias->name, name, sizeof(alias->name));
    strlcpy(alias->expansion, expansion, sizeof(alias->expansion));
    return 0;
}

int shell_unset_alias(struct shell *sh, const char *name)
{
    for (int i = 0; i < sh->alias_count; i++) {
        if (strcmp(sh->aliases[i].name, name) == 0) {
            sh->aliases[i] = sh->aliases[--sh->alias_count];
            return 0;
        }
    }
    return -1;
}

const char *shell_get_alias(struct shell *sh, const char *name)
{
    for (int i = 0; i < sh->alias_count; i++) {
        if (strcmp(sh->aliases[i].name, name) == 0) {
            return sh->aliases[i].expansion;
        }
    }
    return NULL;
}

/* --- expansion -------------------------------------------------------- */

void shell_expand(struct shell *sh, const char *input, char *out, size_t size)
{
    size_t pos      = 0;
    bool_t in_single = false;

    for (const char *p = input; *p && pos + 1 < size; p++) {
        /* Single quotes suppress all expansion. */
        if (*p == '\'' && !in_single) {
            in_single = true;
            out[pos++] = *p;
            continue;
        }
        if (*p == '\'' && in_single) {
            in_single = false;
            out[pos++] = *p;
            continue;
        }
        if (in_single) {
            out[pos++] = *p;
            continue;
        }

        /* Backslash escapes the next character. */
        if (*p == '\\' && p[1]) {
            out[pos++] = *p++;
            if (pos + 1 < size) {
                out[pos++] = *p;
            }
            continue;
        }

        /* Home directory shorthand, only at the start of a word. */
        if (*p == '~' && (p == input || isspace((uint8_t)p[-1])) &&
            (p[1] == '\0' || p[1] == '/' || isspace((uint8_t)p[1]))) {
            const char *home = shell_get_var(sh, "HOME");
            if (home) {
                size_t len = strlen(home);
                len        = MIN(len, size - pos - 1);
                memcpy(out + pos, home, len);
                pos += len;
                continue;
            }
        }

        if (*p != '$') {
            out[pos++] = *p;
            continue;
        }

        /* $? is the previous exit status. */
        if (p[1] == '?') {
            char buf[16];
            snprintf(buf, sizeof(buf), "%d", sh->last_status);
            size_t len = MIN(strlen(buf), size - pos - 1);
            memcpy(out + pos, buf, len);
            pos += len;
            p++;
            continue;
        }
        /* $$ is the process id. */
        if (p[1] == '$') {
            char buf[16];
            snprintf(buf, sizeof(buf), "%d", sched_current_pid());
            size_t len = MIN(strlen(buf), size - pos - 1);
            memcpy(out + pos, buf, len);
            pos += len;
            p++;
            continue;
        }

        /* $NAME or ${NAME} */
        const char *start = p + 1;
        bool_t      braced = (*start == '{');
        if (braced) {
            start++;
        }

        char        name[SHELL_NAME_MAX];
        size_t      name_len = 0;
        const char *q        = start;
        while (*q && name_len + 1 < sizeof(name) &&
               (isalnum((uint8_t)*q) || *q == '_')) {
            name[name_len++] = *q++;
        }
        name[name_len] = '\0';

        if (name_len == 0) {
            out[pos++] = *p;
            continue;
        }
        if (braced) {
            if (*q != '}') {
                out[pos++] = *p;
                continue;
            }
            q++;
        }

        const char *value = shell_get_var(sh, name);
        if (value) {
            size_t len = MIN(strlen(value), size - pos - 1);
            memcpy(out + pos, value, len);
            pos += len;
        }
        p = q - 1;
    }

    out[pos] = '\0';
}

/* --- tokenising ------------------------------------------------------- */

int shell_tokenize(const char *input, char **argv, int max_args, char *storage,
                   size_t storage_size)
{
    int    argc   = 0;
    size_t used   = 0;
    const char *p = input;

    while (*p && argc < max_args) {
        while (isspace((uint8_t)*p)) {
            p++;
        }
        if (!*p) {
            break;
        }

        char  *token = storage + used;
        size_t len   = 0;
        char   quote = 0;

        while (*p) {
            if (quote) {
                if (*p == quote) {
                    quote = 0;
                    p++;
                    continue;
                }
            } else if (*p == '"' || *p == '\'') {
                quote = *p++;
                continue;
            } else if (isspace((uint8_t)*p)) {
                break;
            }

            /* Backslash escapes outside single quotes. */
            if (*p == '\\' && quote != '\'' && p[1]) {
                p++;
            }
            if (used + len + 2 >= storage_size) {
                break;
            }
            token[len++] = *p++;
        }

        token[len] = '\0';
        used += len + 1;
        argv[argc++] = token;
    }

    argv[argc] = NULL;
    return argc;
}

/* --- paths ------------------------------------------------------------ */

void shell_resolve(struct shell *sh, const char *path, char *out, size_t size)
{
    fs_resolve_path(sh->cwd, path, out, size);
}

/* --- command lookup --------------------------------------------------- */

const struct shell_command *shell_find_command(struct shell *sh, const char *name)
{
    for (int i = 0; i < sh->command_count; i++) {
        if (strcmp(sh->commands[i].name, name) == 0) {
            return &sh->commands[i];
        }
    }
    return NULL;
}

/* --- execution -------------------------------------------------------- */

/* Run a single (already expanded and tokenised) command. */
static int run_simple(struct shell *sh, int argc, char **argv)
{
    if (argc == 0) {
        return 0;
    }

    const struct shell_command *cmd = shell_find_command(sh, argv[0]);
    if (!cmd) {
        shell_color(sh, "\033[91m");
        shell_printf(sh, "%s: command not found: %s\n", sh->name, argv[0]);
        shell_reset_color(sh);
        shell_printf(sh, "Try 'help' for a list of available commands.\n");
        return 127;
    }

    if (cmd->flags & CMD_PRIVILEGED) {
        struct task *task = sched_current();
        if (task && task->uid != 0) {
            shell_error(sh, "%s: permission denied (requires root)", cmd->name);
            return 126;
        }
    }

    sh->commands_run++;
    return cmd->handler(sh, argc, argv);
}

/*
 * Execute one pipeline stage list. `segments` holds the individual commands;
 * the output of each is captured and handed to the next through the
 * QIRA_PIPE_INPUT variable, which pipe-aware commands read.
 */
static int run_pipeline(struct shell *sh, char segments[][SHELL_LINE_MAX],
                        int count)
{
    char *pipe_buffer = NULL;
    int   status      = 0;

    for (int stage = 0; stage < count; stage++) {
        char expanded[SHELL_LINE_MAX];
        shell_expand(sh, segments[stage], expanded, sizeof(expanded));

        /* Redirection: `> file`, `>> file`, `< file`. */
        char  redirect_path[FS_PATH_MAX] = {0};
        bool_t append   = false;
        bool_t redirect = false;

        char *arrow = strstr(expanded, ">>");
        if (arrow) {
            append   = true;
            redirect = true;
        } else {
            arrow = strchr(expanded, '>');
            if (arrow) {
                redirect = true;
            }
        }
        if (redirect) {
            char *target = arrow + (append ? 2 : 1);
            while (isspace((uint8_t)*target)) {
                target++;
            }
            /* Strip a trailing quote or whitespace from the filename. */
            char raw[FS_PATH_MAX];
            strlcpy(raw, target, sizeof(raw));
            char *end = raw + strlen(raw);
            while (end > raw && (isspace((uint8_t)end[-1]) || end[-1] == '"')) {
                *--end = '\0';
            }
            char *begin = raw;
            if (*begin == '"') {
                begin++;
            }
            shell_resolve(sh, begin, redirect_path, sizeof(redirect_path));
            *arrow = '\0';
        }

        char *input_redirect = strchr(expanded, '<');
        if (input_redirect) {
            char *source = input_redirect + 1;
            while (isspace((uint8_t)*source)) {
                source++;
            }
            char path[FS_PATH_MAX];
            char raw[FS_PATH_MAX];
            strlcpy(raw, source, sizeof(raw));
            char *end = raw + strlen(raw);
            while (end > raw && isspace((uint8_t)end[-1])) {
                *--end = '\0';
            }
            shell_resolve(sh, raw, path, sizeof(path));
            *input_redirect = '\0';

            char *input_data = shell_scratch();
            size_t got       = 0;
            if (input_data &&
                fs_read_file(path, input_data, shell_scratch_size() - 1, &got) == 0) {
                input_data[got] = '\0';
                shell_set_var(sh, "QIRA_PIPE_INPUT", input_data);
            } else {
                shell_error(sh, "%s: cannot open input file", path);
                return 1;
            }
        }

        /* Feed the previous stage's output in. */
        if (pipe_buffer) {
            shell_set_var(sh, "QIRA_PIPE_INPUT", pipe_buffer);
        }

        char  storage[SHELL_LINE_MAX * 2];
        char *argv[SHELL_ARGS_MAX];
        int   argc = shell_tokenize(expanded, argv, SHELL_ARGS_MAX - 1, storage,
                                    sizeof(storage));
        if (argc == 0) {
            continue;
        }

        /* Alias substitution (single level, to avoid loops). */
        const char *alias = shell_get_alias(sh, argv[0]);
        char        alias_line[SHELL_LINE_MAX];
        if (alias) {
            snprintf(alias_line, sizeof(alias_line), "%s", alias);
            for (int i = 1; i < argc; i++) {
                strlcat(alias_line, " ", sizeof(alias_line));
                strlcat(alias_line, argv[i], sizeof(alias_line));
            }
            argc = shell_tokenize(alias_line, argv, SHELL_ARGS_MAX - 1, storage,
                                  sizeof(storage));
        }

        bool_t capture = (stage + 1 < count) || redirect;

        struct shell_sink  sink;
        struct shell_sink *saved_sink = sh->sink;

        if (capture) {
            sink.buffer = kmalloc(PIPE_BUFFER_SIZE);
            if (!sink.buffer) {
                shell_error(sh, "out of memory setting up the pipeline");
                if (pipe_buffer) {
                    kfree(pipe_buffer);
                }
                return 1;
            }
            sink.buffer[0] = '\0';
            sink.capacity  = PIPE_BUFFER_SIZE;
            sink.length    = 0;
            sink.truncated = false;
            sh->sink       = &sink;
        }

        status = run_simple(sh, argc, argv);

        sh->sink = saved_sink;

        if (pipe_buffer) {
            kfree(pipe_buffer);
            pipe_buffer = NULL;
        }

        if (capture) {
            if (redirect) {
                struct file *file = fs_open(
                    redirect_path,
                    O_WRONLY | O_CREATE | (append ? O_APPEND : O_TRUNC));
                if (file) {
                    fs_write(file, sink.buffer, sink.length);
                    fs_close(file);
                } else {
                    shell_error(sh, "%s: cannot open for writing", redirect_path);
                    status = 1;
                }
                kfree(sink.buffer);
            } else {
                pipe_buffer = sink.buffer;
            }
        }
    }

    if (pipe_buffer) {
        kfree(pipe_buffer);
    }
    shell_unset_var(sh, "QIRA_PIPE_INPUT");
    return status;
}

/* Split on an unquoted separator character, returning the segment count. */
static int split_on(const char *line, char separator,
                    char out[][SHELL_LINE_MAX], int max_segments)
{
    int    count = 0;
    size_t pos   = 0;
    char   quote = 0;

    out[0][0] = '\0';

    for (const char *p = line; *p; p++) {
        if (quote) {
            if (*p == quote) {
                quote = 0;
            }
        } else if (*p == '"' || *p == '\'') {
            quote = *p;
        } else if (*p == separator) {
            out[count][pos] = '\0';
            if (++count >= max_segments) {
                return count;
            }
            pos = 0;
            out[count][0] = '\0';
            continue;
        }
        if (pos + 1 < SHELL_LINE_MAX) {
            out[count][pos++] = *p;
        }
    }

    out[count][pos] = '\0';
    return count + 1;
}

int shell_execute_line(struct shell *sh, const char *line)
{
    /* Skip blanks and comments. */
    const char *start = line;
    while (isspace((uint8_t)*start)) {
        start++;
    }
    if (*start == '\0' || *start == '#') {
        return sh->last_status;
    }

    if (sh->depth > 8) {
        shell_error(sh, "maximum command nesting depth exceeded");
        return 1;
    }
    sh->depth++;

    /* Statement separators: `;` runs unconditionally. */
    static char statements[SHELL_PIPE_MAX][SHELL_LINE_MAX];
    int statement_count = split_on(start, ';', statements, SHELL_PIPE_MAX);

    int status = sh->last_status;

    for (int s = 0; s < statement_count; s++) {
        /* `&&` and `||` chaining. */
        char *and_chain = strstr(statements[s], "&&");
        char *or_chain  = strstr(statements[s], "||");

        if (and_chain || or_chain) {
            bool_t use_and = and_chain && (!or_chain || and_chain < or_chain);
            char  *split   = use_and ? and_chain : or_chain;

            char left[SHELL_LINE_MAX];
            size_t left_len = MIN((size_t)(split - statements[s]),
                                  sizeof(left) - 1);
            memcpy(left, statements[s], left_len);
            left[left_len] = '\0';

            status = shell_execute_line(sh, left);
            sh->last_status = status;

            bool_t should_run = use_and ? (status == 0) : (status != 0);
            if (should_run) {
                status = shell_execute_line(sh, split + 2);
            }
            sh->last_status = status;
            continue;
        }

        static char stages[SHELL_PIPE_MAX][SHELL_LINE_MAX];
        int stage_count = split_on(statements[s], '|', stages, SHELL_PIPE_MAX);

        status          = run_pipeline(sh, stages, stage_count);
        sh->last_status = status;
    }

    sh->depth--;
    return status;
}

/* --- history ---------------------------------------------------------- */

void shell_add_history(struct shell *sh, const char *line)
{
    if (!*line) {
        return;
    }
    /* Skip consecutive duplicates. */
    if (sh->history_count > 0 &&
        strcmp(sh->history[(sh->history_count - 1) % SHELL_HISTORY_MAX], line) == 0) {
        return;
    }

    strlcpy(sh->history[sh->history_count % SHELL_HISTORY_MAX], line,
            SHELL_LINE_MAX);
    sh->history_count++;
    sh->history_pos = sh->history_count;
}

/* --- line editing ----------------------------------------------------- */

/* Redraw the input line after an edit. */
static void redraw(struct shell *sh, const char *prompt, const char *buf,
                   size_t length, size_t cursor, size_t previous_length)
{
    UNUSED(sh);
    console_putc('\r');
    console_puts(prompt);
    console_write(buf, length);

    /* Erase whatever the previous, longer line left behind. */
    for (size_t i = length; i < previous_length; i++) {
        console_putc(' ');
    }
    console_putc('\r');
    console_puts(prompt);
    console_write(buf, cursor);
}

/*
 * Complete a partial command name or path. Returns the number of matches and
 * fills `buf` with the common prefix when there is more than one.
 */
static int complete(struct shell *sh, char *buf, size_t size, size_t *length,
                    size_t *cursor)
{
    /* Find the word under the cursor. */
    size_t start = *cursor;
    while (start > 0 && !isspace((uint8_t)buf[start - 1])) {
        start--;
    }
    char partial[256];
    size_t partial_len = MIN(*cursor - start, sizeof(partial) - 1);
    memcpy(partial, buf + start, partial_len);
    partial[partial_len] = '\0';

    const char *matches[64];
    int         match_count = 0;

    if (start == 0) {
        /* First word: complete command names. */
        for (int i = 0; i < sh->command_count && match_count < 64; i++) {
            if (strncmp(sh->commands[i].name, partial, partial_len) == 0) {
                matches[match_count++] = sh->commands[i].name;
            }
        }
    } else {
        /* Later words: complete filenames in the referenced directory. */
        char dir_part[FS_PATH_MAX];
        char name_part[FS_NAME_MAX];

        const char *slash = strrchr(partial, '/');
        if (slash) {
            size_t dir_len = MIN((size_t)(slash - partial), sizeof(dir_part) - 1);
            memcpy(dir_part, partial, dir_len);
            dir_part[dir_len] = '\0';
            strlcpy(name_part, slash + 1, sizeof(name_part));
        } else {
            strlcpy(dir_part, ".", sizeof(dir_part));
            strlcpy(name_part, partial, sizeof(name_part));
        }

        char resolved[FS_PATH_MAX];
        shell_resolve(sh, dir_part, resolved, sizeof(resolved));

        static char name_storage[64][FS_NAME_MAX];
        struct fs_dirent entry;
        size_t name_len = strlen(name_part);

        for (int i = 0; match_count < 64; i++) {
            if (fs_readdir_path(resolved, i, &entry) != 0) {
                break;
            }
            if (strncmp(entry.name, name_part, name_len) == 0) {
                strlcpy(name_storage[match_count], entry.name, FS_NAME_MAX);
                matches[match_count] = name_storage[match_count];
                match_count++;
            }
        }
        /* Completion replaces only the final path component. */
        if (slash) {
            start += (size_t)(slash - partial) + 1;
            partial_len = strlen(name_part);
        }
    }

    if (match_count == 0) {
        return 0;
    }

    /* Find the longest common prefix of the matches. */
    size_t common = strlen(matches[0]);
    for (int i = 1; i < match_count; i++) {
        size_t j = 0;
        while (j < common && matches[i][j] == matches[0][j]) {
            j++;
        }
        common = j;
    }

    if (common > partial_len) {
        size_t insert = common - partial_len;
        if (*length + insert < size - 1) {
            memmove(buf + *cursor + insert, buf + *cursor, *length - *cursor + 1);
            memcpy(buf + *cursor, matches[0] + partial_len, insert);
            *length += insert;
            *cursor += insert;
        }
    }

    return match_count;
}

int shell_read_line(struct shell *sh, const char *prompt, char *buf, size_t size)
{
    size_t length = 0;
    size_t cursor = 0;

    buf[0]          = '\0';
    sh->history_pos = sh->history_count;

    console_puts(prompt);

    for (;;) {
        int ch = console_getchar();

        if (ch == '\n' || ch == '\r') {
            console_putc('\n');
            break;
        }

        /* Ctrl+C abandons the line. */
        if (ch == 3) {
            console_puts("^C\n");
            buf[0] = '\0';
            return 0;
        }
        /* Ctrl+D on an empty line requests exit. */
        if (ch == 4) {
            if (length == 0) {
                console_puts("exit\n");
                strlcpy(buf, "exit", size);
                return 4;
            }
            continue;
        }
        /* Ctrl+A / Ctrl+E move to the line ends. */
        if (ch == 1) {
            cursor = 0;
            redraw(sh, prompt, buf, length, cursor, length);
            continue;
        }
        if (ch == 5) {
            cursor = length;
            redraw(sh, prompt, buf, length, cursor, length);
            continue;
        }
        /* Ctrl+U clears the line, Ctrl+K clears to the end. */
        if (ch == 21) {
            size_t previous = length;
            memmove(buf, buf + cursor, length - cursor + 1);
            length -= cursor;
            cursor = 0;
            redraw(sh, prompt, buf, length, cursor, previous);
            continue;
        }
        if (ch == 11) {
            size_t previous = length;
            buf[cursor]     = '\0';
            length          = cursor;
            redraw(sh, prompt, buf, length, cursor, previous);
            continue;
        }
        /* Ctrl+L clears the screen. */
        if (ch == 12) {
            console_clear();
            redraw(sh, prompt, buf, length, cursor, 0);
            continue;
        }

        if (ch == '\t') {
            size_t previous = length;
            int    matches  = complete(sh, buf, size, &length, &cursor);
            if (matches > 1) {
                console_putc('\n');
                /* Re-run to list the candidates. */
                console_puts("(multiple matches)\n");
            }
            redraw(sh, prompt, buf, length, cursor, previous);
            continue;
        }

        if (ch == '\b' || ch == 127) {
            if (cursor > 0) {
                size_t previous = length;
                memmove(buf + cursor - 1, buf + cursor, length - cursor + 1);
                cursor--;
                length--;
                redraw(sh, prompt, buf, length, cursor, previous);
            }
            continue;
        }

        if (ch == KEY_LEFT) {
            if (cursor > 0) {
                cursor--;
                redraw(sh, prompt, buf, length, cursor, length);
            }
            continue;
        }
        if (ch == KEY_RIGHT) {
            if (cursor < length) {
                cursor++;
                redraw(sh, prompt, buf, length, cursor, length);
            }
            continue;
        }
        if (ch == KEY_HOME) {
            cursor = 0;
            redraw(sh, prompt, buf, length, cursor, length);
            continue;
        }
        if (ch == KEY_END) {
            cursor = length;
            redraw(sh, prompt, buf, length, cursor, length);
            continue;
        }
        if (ch == KEY_DELETE) {
            if (cursor < length) {
                size_t previous = length;
                memmove(buf + cursor, buf + cursor + 1, length - cursor);
                length--;
                redraw(sh, prompt, buf, length, cursor, previous);
            }
            continue;
        }

        if (ch == KEY_UP || ch == KEY_DOWN) {
            int target = sh->history_pos + ((ch == KEY_UP) ? -1 : 1);
            int oldest = MAX(0, sh->history_count - SHELL_HISTORY_MAX);

            if (target < oldest) {
                target = oldest;
            }
            if (target > sh->history_count) {
                target = sh->history_count;
            }

            size_t previous = length;
            sh->history_pos = target;

            if (target == sh->history_count) {
                buf[0] = '\0';
                length = cursor = 0;
            } else {
                strlcpy(buf, sh->history[target % SHELL_HISTORY_MAX], size);
                length = cursor = strlen(buf);
            }
            redraw(sh, prompt, buf, length, cursor, previous);
            continue;
        }

        /* Ignore anything else non-printable. */
        if (ch < 32 || ch > 126) {
            continue;
        }

        if (length + 1 >= size) {
            continue;
        }

        /* Insert at the cursor. */
        memmove(buf + cursor + 1, buf + cursor, length - cursor + 1);
        buf[cursor] = (char)ch;
        length++;
        cursor++;

        if (cursor == length) {
            console_putc((char)ch);
        } else {
            redraw(sh, prompt, buf, length, cursor, length);
        }
    }

    buf[length] = '\0';
    return (int)length;
}

/* --- prompt ----------------------------------------------------------- */

static void build_prompt(struct shell *sh, char *out, size_t size)
{
    const char *user = shell_get_var(sh, "USER");
    bool_t      color = color_enabled(sh);

    /* Shorten the home directory to `~`. */
    const char *home = shell_get_var(sh, "HOME");
    char        display[128];
    if (home && strncmp(sh->cwd, home, strlen(home)) == 0) {
        snprintf(display, sizeof(display), "~%s", sh->cwd + strlen(home));
    } else {
        strlcpy(display, sh->cwd, sizeof(display));
    }

    if (color) {
        snprintf(out, size, "\033[92m%s@qira\033[0m:\033[94m%s\033[0m%s ",
                 user ? user : "user", display,
                 (sh->last_status == 0) ? "$" : "\033[91m$\033[0m");
    } else {
        snprintf(out, size, "%s@qira:%s$ ", user ? user : "user", display);
    }
}

void shell_run(struct shell *sh)
{
    char line[SHELL_LINE_MAX];
    char prompt[256];

    sh->running = true;

    while (sh->running) {
        shell_set_var(sh, "PWD", sh->cwd);
        build_prompt(sh, prompt, sizeof(prompt));

        int length = shell_read_line(sh, prompt, line, sizeof(line));
        if (length < 0) {
            continue;
        }

        shell_add_history(sh, line);
        shell_execute_line(sh, line);
    }
}

/* --- shared builtins -------------------------------------------------- */

int shell_builtin_exit(struct shell *sh, int argc, char **argv)
{
    sh->running = false;
    return (argc > 1) ? atoi(argv[1]) : 0;
}

int shell_builtin_help(struct shell *sh, int argc, char **argv)
{
    if (argc > 1) {
        const struct shell_command *cmd = shell_find_command(sh, argv[1]);
        if (!cmd) {
            shell_error(sh, "no help for '%s': unknown command", argv[1]);
            return 1;
        }

        shell_color(sh, "\033[1m");
        shell_printf(sh, "%s", cmd->name);
        shell_reset_color(sh);
        shell_printf(sh, " - %s\n\n", cmd->summary);
        shell_printf(sh, "Usage: %s\n", cmd->usage ? cmd->usage : cmd->name);
        if (cmd->details) {
            shell_printf(sh, "\n%s\n", cmd->details);
        }
        return 0;
    }

    shell_color(sh, "\033[1m");
    shell_printf(sh, "%s built-in commands\n", sh->name);
    shell_reset_color(sh);
    shell_printf(sh, "\n");

    for (int i = 0; i < sh->command_count; i++) {
        const struct shell_command *cmd = &sh->commands[i];
        if (cmd->flags & CMD_HIDDEN) {
            continue;
        }
        shell_printf(sh, "  %-14s %s\n", cmd->name, cmd->summary);
    }

    shell_printf(sh, "\nUse 'help <command>' for detailed usage.\n");
    return 0;
}

int shell_builtin_history(struct shell *sh, int argc, char **argv)
{
    if (argc > 1 && strcmp(argv[1], "-c") == 0) {
        sh->history_count = 0;
        sh->history_pos   = 0;
        shell_printf(sh, "history cleared\n");
        return 0;
    }

    int oldest = MAX(0, sh->history_count - SHELL_HISTORY_MAX);
    for (int i = oldest; i < sh->history_count; i++) {
        shell_printf(sh, "%5d  %s\n", i + 1, sh->history[i % SHELL_HISTORY_MAX]);
    }
    return 0;
}

int shell_builtin_alias(struct shell *sh, int argc, char **argv)
{
    if (argc == 1) {
        for (int i = 0; i < sh->alias_count; i++) {
            shell_printf(sh, "alias %s='%s'\n", sh->aliases[i].name,
                         sh->aliases[i].expansion);
        }
        return 0;
    }

    /* alias name=value  or  alias name value... */
    char *equals = strchr(argv[1], '=');
    if (equals) {
        *equals = '\0';
        shell_set_alias(sh, argv[1], equals + 1);
        return 0;
    }
    if (argc >= 3) {
        char expansion[SHELL_VALUE_MAX] = {0};
        for (int i = 2; i < argc; i++) {
            if (i > 2) {
                strlcat(expansion, " ", sizeof(expansion));
            }
            strlcat(expansion, argv[i], sizeof(expansion));
        }
        shell_set_alias(sh, argv[1], expansion);
        return 0;
    }

    const char *value = shell_get_alias(sh, argv[1]);
    if (value) {
        shell_printf(sh, "alias %s='%s'\n", argv[1], value);
        return 0;
    }
    shell_error(sh, "alias: %s: not found", argv[1]);
    return 1;
}

int shell_builtin_unalias(struct shell *sh, int argc, char **argv)
{
    if (argc < 2) {
        shell_error(sh, "usage: unalias <name>");
        return 1;
    }
    if (shell_unset_alias(sh, argv[1]) != 0) {
        shell_error(sh, "unalias: %s: not found", argv[1]);
        return 1;
    }
    return 0;
}
