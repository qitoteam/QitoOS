/*
 * QitoOS - Terminal application
 *
 * A windowed terminal that hosts QCSH and UltraShell. Each terminal window
 * owns a scrollback buffer and its own shell state, so several terminals can
 * be open at once with independent working directories and history.
 *
 * The shell engine writes through a per-window sink, which is how command
 * output reaches the terminal instead of the boot console.
 */

#include <kernel/desktop.h>
#include <kernel/shell.h>
#include <kernel/string.h>
#include <kernel/printf.h>
#include <kernel/mm.h>
#include <kernel/log.h>
#include <kernel/version.h>
#include <kernel/time.h>
#include <kernel/config.h>
#include <kernel/console.h>
#include <kernel/serial.h>
#include "../../boot/bootinfo.h"

#define TERM_COLS       160
#define TERM_ROWS       400   /* includes scrollback */
#define TERM_INPUT_MAX  512

struct term_cell {
    char    ch;
    color_t fg;
};

struct terminal_state {
    struct term_cell cells[TERM_ROWS][TERM_COLS];
    int    cursor_row;
    int    cursor_col;
    int    scroll_offset;   /* rows scrolled back from the bottom */
    int    total_rows;      /* rows written so far                */

    char   input[TERM_INPUT_MAX];
    int    input_len;
    int    input_cursor;

    /* Which shell this window is driving. */
    struct shell *shell;
    bool_t  is_qcsh;

    /* Per-window shell state so terminals are independent. */
    struct shell instance;

    color_t current_color;
    bool_t  initialised;
    uint64_t last_blink_ms;
    bool_t  cursor_visible;

    /* Escape sequence parsing for coloured output. */
    int  esc_state;
    int  esc_params[4];
    int  esc_param_count;
};

static const color_t ansi_palette[16] = {
    RGB(40, 44, 56),    RGB(230, 100, 100), RGB(130, 210, 130), RGB(230, 205, 110),
    RGB(100, 160, 240), RGB(200, 130, 230), RGB(100, 210, 220), RGB(205, 212, 226),
    RGB(100, 106, 124), RGB(255, 130, 130), RGB(160, 240, 160), RGB(255, 235, 150),
    RGB(140, 190, 255), RGB(225, 165, 255), RGB(140, 240, 250), RGB(248, 250, 255),
};

#define DEFAULT_FG ansi_palette[7]

/* The terminal currently receiving shell output. */
static struct terminal_state *active_terminal;

/* Ensures a command-line autorun script executes only once. */
static bool_t autorun_done;

static void term_run(struct terminal_state *term, const char *line);
static void term_prompt(struct terminal_state *term);
static void term_printf(struct terminal_state *term, const char *fmt, ...)
    PRINTF_FMT(2, 3);

/* --- output ----------------------------------------------------------- */

static void term_newline(struct terminal_state *term)
{
    term->cursor_col = 0;
    term->cursor_row++;

    if (term->cursor_row >= TERM_ROWS) {
        /* Scroll the whole buffer up by one row. */
        memmove(&term->cells[0], &term->cells[1],
                sizeof(term->cells) - sizeof(term->cells[0]));
        term->cursor_row = TERM_ROWS - 1;
        for (int c = 0; c < TERM_COLS; c++) {
            term->cells[term->cursor_row][c].ch = ' ';
            term->cells[term->cursor_row][c].fg = DEFAULT_FG;
        }
    }
    if (term->cursor_row >= term->total_rows) {
        term->total_rows = term->cursor_row + 1;
    }
}

/* Handle a byte of an ANSI escape sequence. Returns true if consumed. */
static bool_t term_escape(struct terminal_state *term, char c)
{
    switch (term->esc_state) {
    case 0:
        if (c == '\033') {
            term->esc_state       = 1;
            term->esc_param_count = 0;
            memset(term->esc_params, 0, sizeof(term->esc_params));
            return true;
        }
        return false;
    case 1:
        term->esc_state = (c == '[') ? 2 : 0;
        return true;
    case 2:
        if (c >= '0' && c <= '9') {
            if (term->esc_param_count == 0) {
                term->esc_param_count = 1;
            }
            int index = term->esc_param_count - 1;
            term->esc_params[index] = term->esc_params[index] * 10 + (c - '0');
            return true;
        }
        if (c == ';') {
            if (term->esc_param_count < 4) {
                term->esc_param_count++;
            }
            return true;
        }
        if (c == 'm') {
            if (term->esc_param_count == 0) {
                term->current_color = DEFAULT_FG;
            }
            for (int i = 0; i < term->esc_param_count; i++) {
                int code = term->esc_params[i];
                if (code == 0) {
                    term->current_color = DEFAULT_FG;
                } else if (code >= 30 && code <= 37) {
                    term->current_color = ansi_palette[code - 30];
                } else if (code >= 90 && code <= 97) {
                    term->current_color = ansi_palette[code - 90 + 8];
                } else if (code == 1) {
                    /* Bold: brighten the current colour. */
                    for (int p = 0; p < 8; p++) {
                        if (term->current_color == ansi_palette[p]) {
                            term->current_color = ansi_palette[p + 8];
                            break;
                        }
                    }
                }
            }
        }
        term->esc_state = 0;
        return true;
    default:
        term->esc_state = 0;
        return false;
    }
}

static void term_putc(struct terminal_state *term, char c)
{
    if (term_escape(term, c)) {
        return;
    }

    switch (c) {
    case '\n':
        term_newline(term);
        return;
    case '\r':
        term->cursor_col = 0;
        return;
    case '\t':
        term->cursor_col = (term->cursor_col + 8) & ~7;
        if (term->cursor_col >= TERM_COLS) {
            term_newline(term);
        }
        return;
    case '\b':
        if (term->cursor_col > 0) {
            term->cursor_col--;
            term->cells[term->cursor_row][term->cursor_col].ch = ' ';
        }
        return;
    case '\0':
        return;
    default:
        break;
    }

    if (c < 32 || c > 126) {
        return;
    }
    if (term->cursor_col >= TERM_COLS) {
        term_newline(term);
    }

    term->cells[term->cursor_row][term->cursor_col].ch = c;
    term->cells[term->cursor_row][term->cursor_col].fg = term->current_color;
    term->cursor_col++;
}

static void term_write(struct terminal_state *term, const char *text, size_t len)
{
    for (size_t i = 0; i < len; i++) {
        term_putc(term, text[i]);
    }
    term->scroll_offset = 0;   /* jump to the bottom on new output */
}

static void term_printf(struct terminal_state *term, const char *fmt, ...)
{
    char    buf[1024];
    va_list ap;

    va_start(ap, fmt);
    int n = vsnprintf(buf, sizeof(buf), fmt, ap);
    va_end(ap);

    if (n > 0) {
        term_write(term, buf, MIN((size_t)n, sizeof(buf) - 1));
    }
}

/*
 * When the kernel command line asks for it, shell output is echoed to the
 * serial port as well as the window. Automated boot tests have no way to read
 * the framebuffer text, so this is how they observe command results.
 */
static bool_t echo_to_serial;

/* Console hook: routes shell output into the active terminal window. */
static void terminal_output_hook(const char *text, size_t len)
{
    if (active_terminal) {
        term_write(active_terminal, text, len);
    }
    if (echo_to_serial) {
        serial_write_len(text, len);
    }
}

/* --- prompt ----------------------------------------------------------- */

static void term_prompt(struct terminal_state *term)
{
    struct shell *sh = term->shell;

    const char *home = shell_get_var(sh, "HOME");
    char        display[96];

    if (home && strncmp(sh->cwd, home, strlen(home)) == 0) {
        snprintf(display, sizeof(display), "~%s", sh->cwd + strlen(home));
    } else {
        strlcpy(display, sh->cwd, sizeof(display));
    }

    term->current_color = ansi_palette[10];
    term_printf(term, "%s@qito", shell_get_var(sh, "USER"));
    term->current_color = DEFAULT_FG;
    term_printf(term, ":");
    term->current_color = ansi_palette[12];
    term_printf(term, "%s", display);
    term->current_color = DEFAULT_FG;
    term_printf(term, "%s ", term->is_qcsh ? "#" : "$");
}

static void term_banner(struct terminal_state *term)
{
    term->current_color = ansi_palette[14];
    term_printf(term, "QitoOS %s \"%s\"\n", QITO_VERSION_STRING, QITO_CODENAME);
    term->current_color = DEFAULT_FG;
    term_printf(term, "%s ready. Type 'help' for commands.\n",
                term->is_qcsh ? "QitoConfigShell (QCSH)" : "UltraShell");
    term_printf(term, "Switch shells with '%s'.\n\n",
                term->is_qcsh ? "ush" : "qcsh");
}

/* --- command execution ------------------------------------------------ */

static void term_run(struct terminal_state *term, const char *line)
{
    struct terminal_state *previous = active_terminal;
    active_terminal = term;

    /* Route the shell's console writes into this window. */
    console_set_hook(terminal_output_hook);

    shell_add_history(term->shell, line);
    shell_execute_line(term->shell, line);

    console_set_hook(NULL);
    active_terminal = previous;

    /* Honour a shell switch requested by the `qcsh` or `ush` builtins. */
    if (term->shell->switch_request) {
        const char *target = term->shell->switch_request;
        term->shell->switch_request = NULL;

        if (strcmp(target, "qcsh") == 0) {
            term->is_qcsh = true;
            term->shell   = qcsh_instance();
        } else if (strcmp(target, "ush") == 0) {
            term->is_qcsh = false;
            term->shell   = ultrashell_instance();
        }
        term->shell->switch_request = NULL;
    }

    /* `exit` closes the terminal window rather than the shell. */
    if (!term->shell->running) {
        term->shell->running = true;
        term_printf(term, "(use the window close button to quit the terminal)\n");
    }
}

/* --- application hooks ------------------------------------------------ */

static void terminal_on_open(struct window *win)
{
    struct terminal_state *term = (struct terminal_state *)win->app_state;

    for (int r = 0; r < TERM_ROWS; r++) {
        for (int c = 0; c < TERM_COLS; c++) {
            term->cells[r][c].ch = ' ';
            term->cells[r][c].fg = DEFAULT_FG;
        }
    }

    term->current_color = DEFAULT_FG;
    term->total_rows    = 1;
    term->cursor_visible = true;

    /* Start with whichever shell the configuration prefers. */
    const char *preferred = config_get_string("terminal.default_shell", "ush");
    term->is_qcsh = (strcmp(preferred, "qcsh") == 0);
    term->shell   = term->is_qcsh ? qcsh_instance() : ultrashell_instance();

    term_banner(term);
    term_prompt(term);

    term->initialised = true;

    /*
     * Automated tests cannot type, so `autorun=cmd1;cmd2` on the kernel
     * command line is executed in the first terminal that opens. The commands
     * and their output land in the scrollback exactly as if typed.
     */
    if (!autorun_done) {
        extern const struct qito_boot_info *kernel_boot_info(void);
        const char *cmdline = kernel_boot_info()->cmdline;

        /* `echo=serial` mirrors shell output to COM1 for the test harness. */
        if (strstr(cmdline, "echo=serial")) {
            echo_to_serial = true;
        }

        const char *option = strstr(cmdline, "autorun=");
        if (option) {
            autorun_done = true;
            option += 8;

            char script[256];
            strlcpy(script, option, sizeof(script));

            /*
             * The option ends at the next space, so underscores stand in for
             * spaces inside the commands themselves:
             *     autorun=ls_-l_/etc;calc_2+2
             */
            char *space = strchr(script, ' ');
            if (space) {
                *space = '\0';
            }
            for (char *c = script; *c; c++) {
                if (*c == '_') {
                    *c = ' ';
                }
            }

            char *save = NULL;
            for (char *command = strtok_r(script, ";", &save); command;
                 command       = strtok_r(NULL, ";", &save)) {
                if (echo_to_serial) {
                    serial_write("\n--QITO-SHELL$ ");
                    serial_write(command);
                    serial_write("\n");
                }
                term_printf(term, "%s\n", command);
                term_run(term, command);
                term_prompt(term);
            }
            if (echo_to_serial) {
                serial_write("\n--QITO-AUTORUN-COMPLETE--\n");
            }
        }
    }
}

static void terminal_draw(struct window *win, int x, int y, int w, int h)
{
    struct terminal_state *term = (struct terminal_state *)win->app_state;
    const struct theme    *theme = desktop_theme();

    /* Terminal background is darker than the window chrome. */
    fb_fill_rect(x, y, w, h, RGB(16, 18, 26));

    int padding      = 6;
    int visible_cols = MIN((w - padding * 2) / FONT_WIDTH, TERM_COLS);
    int visible_rows = (h - padding * 2) / FONT_HEIGHT;

    if (visible_cols <= 0 || visible_rows <= 0) {
        return;
    }

    /* Which buffer row appears at the top of the view. */
    int last_row  = term->cursor_row;
    int first_row = last_row - visible_rows + 1 - term->scroll_offset;
    if (first_row < 0) {
        first_row = 0;
    }

    for (int row = 0; row < visible_rows; row++) {
        int buffer_row = first_row + row;
        if (buffer_row > last_row || buffer_row >= TERM_ROWS) {
            break;
        }

        int py = y + padding + row * FONT_HEIGHT;
        for (int col = 0; col < visible_cols; col++) {
            struct term_cell *cell = &term->cells[buffer_row][col];
            if (cell->ch != ' ' && cell->ch != '\0') {
                fb_draw_char(x + padding + col * FONT_WIDTH, py, cell->ch, cell->fg);
            }
        }
    }

    /* The input line is drawn after the buffered output. */
    int prompt_row = last_row - first_row;
    if (prompt_row >= 0 && prompt_row < visible_rows && term->scroll_offset == 0) {
        int px = x + padding + term->cursor_col * FONT_WIDTH;
        int py = y + padding + prompt_row * FONT_HEIGHT;

        for (int i = 0; i < term->input_len && px < x + w - FONT_WIDTH; i++) {
            fb_draw_char(px, py, term->input[i], RGB(240, 244, 252));
            px += FONT_WIDTH;
        }

        /* Block cursor at the insertion point. */
        if (win->focused && term->cursor_visible) {
            int cursor_px = x + padding +
                            (term->cursor_col + term->input_cursor) * FONT_WIDTH;
            if (cursor_px < x + w - 2) {
                fb_fill_rect(cursor_px, py, FONT_WIDTH, FONT_HEIGHT,
                             RGB(120, 190, 250));
                if (term->input_cursor < term->input_len) {
                    fb_draw_char(cursor_px, py, term->input[term->input_cursor],
                                 RGB(16, 18, 26));
                }
            }
        }
    }

    /* Scrollback indicator. */
    if (term->scroll_offset > 0) {
        char label[48];
        snprintf(label, sizeof(label), " scrolled back %d lines ",
                 term->scroll_offset);
        int label_w = fb_text_width(label);
        fb_fill_rect(x + w - label_w - 8, y + 4, label_w + 4, FONT_HEIGHT + 2,
                     theme->accent);
        fb_draw_string(x + w - label_w - 6, y + 5, label, RGB(255, 255, 255));
    }
}

static bool_t terminal_on_key(struct window *win, uint32_t key, uint32_t modifiers)
{
    struct terminal_state *term = (struct terminal_state *)win->app_state;

    /* Scrollback. */
    if (key == KEY_PAGEUP) {
        term->scroll_offset = MIN(term->scroll_offset + 5, MAX(term->total_rows - 5, 0));
        return true;
    }
    if (key == KEY_PAGEDOWN) {
        term->scroll_offset = MAX(term->scroll_offset - 5, 0);
        return true;
    }

    /* Ctrl shortcuts. */
    if (modifiers & MOD_CTRL) {
        if (key == 'c' || key == 'C' || key == 3) {
            term_printf(term, "%.*s^C\n", term->input_len, term->input);
            term->input_len    = 0;
            term->input_cursor = 0;
            term->input[0]     = '\0';
            term_prompt(term);
            return true;
        }
        if (key == 'l' || key == 'L' || key == 12) {
            for (int r = 0; r < TERM_ROWS; r++) {
                for (int c = 0; c < TERM_COLS; c++) {
                    term->cells[r][c].ch = ' ';
                }
            }
            term->cursor_row    = 0;
            term->cursor_col    = 0;
            term->total_rows    = 1;
            term->scroll_offset = 0;
            term_prompt(term);
            return true;
        }
        if (key == 'u' || key == 'U' || key == 21) {
            term->input_len    = 0;
            term->input_cursor = 0;
            term->input[0]     = '\0';
            return true;
        }
    }

    if (key == KEY_ENTER || key == '\n' || key == '\r') {
        term->input[term->input_len] = '\0';
        term_printf(term, "%s\n", term->input);

        if (term->input_len > 0) {
            term_run(term, term->input);
        }

        term->input_len    = 0;
        term->input_cursor = 0;
        term->input[0]     = '\0';
        term->scroll_offset = 0;
        term_prompt(term);
        return true;
    }

    if (key == KEY_BACKSPACE || key == '\b' || key == 127) {
        if (term->input_cursor > 0) {
            memmove(term->input + term->input_cursor - 1,
                    term->input + term->input_cursor,
                    (size_t)(term->input_len - term->input_cursor + 1));
            term->input_cursor--;
            term->input_len--;
        }
        return true;
    }

    if (key == KEY_DELETE) {
        if (term->input_cursor < term->input_len) {
            memmove(term->input + term->input_cursor,
                    term->input + term->input_cursor + 1,
                    (size_t)(term->input_len - term->input_cursor));
            term->input_len--;
        }
        return true;
    }

    if (key == KEY_LEFT) {
        if (term->input_cursor > 0) {
            term->input_cursor--;
        }
        return true;
    }
    if (key == KEY_RIGHT) {
        if (term->input_cursor < term->input_len) {
            term->input_cursor++;
        }
        return true;
    }
    if (key == KEY_HOME) {
        term->input_cursor = 0;
        return true;
    }
    if (key == KEY_END) {
        term->input_cursor = term->input_len;
        return true;
    }

    /* History. */
    if (key == KEY_UP || key == KEY_DOWN) {
        struct shell *sh = term->shell;
        int oldest = MAX(0, sh->history_count - SHELL_HISTORY_MAX);
        int target = sh->history_pos + ((key == KEY_UP) ? -1 : 1);

        target = CLAMP(target, oldest, sh->history_count);
        sh->history_pos = target;

        if (target == sh->history_count) {
            term->input[0]  = '\0';
            term->input_len = 0;
        } else {
            strlcpy(term->input, sh->history[target % SHELL_HISTORY_MAX],
                    sizeof(term->input));
            term->input_len = (int)strlen(term->input);
        }
        term->input_cursor = term->input_len;
        return true;
    }

    /* Tab completion over command names. */
    if (key == KEY_TAB) {
        struct shell *sh = term->shell;

        /* Only complete the first word for now. */
        bool_t first_word = true;
        for (int i = 0; i < term->input_cursor; i++) {
            if (term->input[i] == ' ') {
                first_word = false;
            }
        }
        if (!first_word) {
            return true;
        }

        const char *match = NULL;
        int         count = 0;
        for (int i = 0; i < sh->command_count; i++) {
            if (strncmp(sh->commands[i].name, term->input,
                        (size_t)term->input_len) == 0) {
                match = sh->commands[i].name;
                count++;
            }
        }

        if (count == 1 && match) {
            strlcpy(term->input, match, sizeof(term->input));
            term->input_len    = (int)strlen(term->input);
            term->input_cursor = term->input_len;
        } else if (count > 1) {
            term_printf(term, "\n");
            for (int i = 0; i < sh->command_count; i++) {
                if (strncmp(sh->commands[i].name, term->input,
                            (size_t)term->input_len) == 0) {
                    term_printf(term, "%-16s", sh->commands[i].name);
                }
            }
            term_printf(term, "\n");
            term_prompt(term);
        }
        return true;
    }

    /* Printable characters. */
    if (key >= 32 && key < 127 && term->input_len + 1 < TERM_INPUT_MAX) {
        memmove(term->input + term->input_cursor + 1,
                term->input + term->input_cursor,
                (size_t)(term->input_len - term->input_cursor + 1));
        term->input[term->input_cursor] = (char)key;
        term->input_cursor++;
        term->input_len++;
        term->scroll_offset = 0;
        return true;
    }

    return false;
}

static bool_t terminal_on_mouse(struct window *win, int x, int y, uint32_t buttons,
                                input_event_type_t type)
{
    struct terminal_state *term = (struct terminal_state *)win->app_state;
    UNUSED(x);
    UNUSED(y);
    UNUSED(buttons);

    if (type == INPUT_MOUSE_SCROLL) {
        /* The event carries the wheel delta in dy; positive scrolls down. */
        return true;
    }
    UNUSED(term);
    return false;
}

static bool_t terminal_tick(struct window *win)
{
    struct terminal_state *term = (struct terminal_state *)win->app_state;
    uint64_t now = time_uptime_ms();

    if (now - term->last_blink_ms >= 530) {
        term->last_blink_ms  = now;
        term->cursor_visible = !term->cursor_visible;
        return win->focused;
    }
    return false;
}

static const struct app_ops terminal_ops = {
    .draw     = terminal_draw,
    .on_key   = terminal_on_key,
    .on_mouse = terminal_on_mouse,
    .tick     = terminal_tick,
    .on_open  = terminal_on_open,
};

void app_terminal_register(void)
{
    desktop_register_app("Terminal", ">_", &terminal_ops, 720, 440,
                         RGB(120, 200, 140), sizeof(struct terminal_state), true);
}
