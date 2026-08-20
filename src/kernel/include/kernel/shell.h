/*
 * QitoOS - shell framework
 *
 * Both QCSH and UltraShell are built on this layer. It provides the pieces a
 * genuine shell needs and that neither wants to reimplement:
 *
 *   - tokenising with quoting and escapes,
 *   - variable expansion,
 *   - pipeline and redirection parsing,
 *   - an extensible command registry with help metadata,
 *   - command history and line editing,
 *   - a captured output stream so pipelines can be implemented without
 *     requiring real processes.
 */
#ifndef QITO_SHELL_H
#define QITO_SHELL_H

#include <kernel/types.h>

#define SHELL_LINE_MAX     1024
#define SHELL_ARGS_MAX     64
#define SHELL_HISTORY_MAX  64
#define SHELL_VARS_MAX     64
#define SHELL_PIPE_MAX     8
#define SHELL_ALIAS_MAX    32
#define SHELL_NAME_MAX     32
#define SHELL_VALUE_MAX    256

struct shell;

/*
 * A command receives its parsed arguments and the shell it is running in.
 * Output must go through shell_printf() so pipelines and redirection work.
 * The return value becomes $? .
 */
typedef int (*shell_command_fn)(struct shell *sh, int argc, char **argv);

struct shell_command {
    const char      *name;
    const char      *summary;    /* one line, shown by `help`            */
    const char      *usage;      /* argument syntax                      */
    const char      *details;    /* long help, may be NULL               */
    shell_command_fn handler;
    uint32_t         flags;
};

#define CMD_HIDDEN     0x01   /* omit from the default help listing      */
#define CMD_PRIVILEGED 0x02   /* requires uid 0                          */

struct shell_var {
    char name[SHELL_NAME_MAX];
    char value[SHELL_VALUE_MAX];
    bool_t exported;
    bool_t readonly;
};

struct shell_alias {
    char name[SHELL_NAME_MAX];
    char expansion[SHELL_VALUE_MAX];
};

/* Where a command's output currently goes. */
struct shell_sink {
    char  *buffer;      /* non-NULL when capturing (pipes, substitution) */
    size_t capacity;
    size_t length;
    bool_t truncated;
};

struct shell {
    const char *name;
    const char *prompt_format;

    const struct shell_command *commands;
    int                          command_count;

    struct shell_var   vars[SHELL_VARS_MAX];
    int                var_count;

    struct shell_alias aliases[SHELL_ALIAS_MAX];
    int                alias_count;

    char   history[SHELL_HISTORY_MAX][SHELL_LINE_MAX];
    int    history_count;
    int    history_pos;

    char   cwd[256];
    int    last_status;
    bool_t running;
    bool_t echo_commands;

    struct shell_sink *sink;   /* NULL means write to the console         */

    /* Nesting guard for scripts and command substitution. */
    int    depth;

    uint64_t commands_run;

    /*
     * Set by the `qcsh` / `ush` builtins to ask the host (the terminal
     * application) to switch which shell it is driving. A windowed terminal
     * cannot nest an interactive shell_run() loop, because there is no
     * blocking console to read from.
     */
    const char *switch_request;
};

/* --- lifecycle -------------------------------------------------------- */
void shell_init(struct shell *sh, const char *name,
                const struct shell_command *commands, int count);
void shell_run(struct shell *sh);
int  shell_execute_line(struct shell *sh, const char *line);

/* --- output ----------------------------------------------------------- */
void shell_printf(struct shell *sh, const char *fmt, ...) PRINTF_FMT(2, 3);
void shell_puts(struct shell *sh, const char *text);
void shell_write(struct shell *sh, const char *text, size_t len);
void shell_error(struct shell *sh, const char *fmt, ...) PRINTF_FMT(2, 3);

/* Colour helpers that respect the shell's colour setting. */
void shell_color(struct shell *sh, const char *ansi);
void shell_reset_color(struct shell *sh);

/* --- variables -------------------------------------------------------- */
const char *shell_get_var(struct shell *sh, const char *name);
int  shell_set_var(struct shell *sh, const char *name, const char *value);
int  shell_unset_var(struct shell *sh, const char *name);
int  shell_var_count(struct shell *sh);
const struct shell_var *shell_var_at(struct shell *sh, int index);

/*
 * Shared scratch buffer for commands that need to hold a whole file in
 * memory. Kernel task stacks are a fixed size, so large buffers are allocated
 * once from the heap rather than placed on the stack by every command.
 * Only one command runs at a time within a shell, so a single buffer is safe.
 */
#define SHELL_SCRATCH_SIZE 16384
char  *shell_scratch(void);
size_t shell_scratch_size(void);

/* --- aliases ---------------------------------------------------------- */
int  shell_set_alias(struct shell *sh, const char *name, const char *expansion);
int  shell_unset_alias(struct shell *sh, const char *name);
const char *shell_get_alias(struct shell *sh, const char *name);

/* --- helpers for command implementations ------------------------------ */
const struct shell_command *shell_find_command(struct shell *sh, const char *name);

/* Expand $VAR, ${VAR}, $?, and ~ in `input`. */
void shell_expand(struct shell *sh, const char *input, char *out, size_t size);

/* Split a command string into argv. Returns argc. */
int  shell_tokenize(const char *input, char **argv, int max_args, char *storage,
                    size_t storage_size);

/* Resolve a path argument against the shell's cwd. */
void shell_resolve(struct shell *sh, const char *path, char *out, size_t size);

/* Shared builtins usable by both shells. */
int  shell_builtin_help(struct shell *sh, int argc, char **argv);
int  shell_builtin_history(struct shell *sh, int argc, char **argv);
int  shell_builtin_alias(struct shell *sh, int argc, char **argv);
int  shell_builtin_unalias(struct shell *sh, int argc, char **argv);
int  shell_builtin_exit(struct shell *sh, int argc, char **argv);

void shell_add_history(struct shell *sh, const char *line);

/* Read a line with editing, history and tab completion. */
int  shell_read_line(struct shell *sh, const char *prompt, char *buf, size_t size);

/* Registered shells (for the terminal application). */
struct shell *qcsh_instance(void);
struct shell *ultrashell_instance(void);
void qcsh_init(void);
void ultrashell_init(void);

const struct shell_command *qcsh_commands(int *count);
const struct shell_command *ultrashell_commands(int *count);

#endif /* QITO_SHELL_H */
