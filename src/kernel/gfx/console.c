/*
 * QitoOS - system console
 *
 * Maintains a character cell grid with per-cell colours, renders it onto the
 * framebuffer, and provides a keyboard input ring for the shells.
 *
 * ANSI escape sequences for colour and cursor control are recognised so the
 * shells can produce coloured output portably.
 */

#include <kernel/console.h>
#include <kernel/fb.h>
#include <kernel/serial.h>
#include <kernel/log.h>
#include <kernel/string.h>
#include <kernel/printf.h>
#include <kernel/mm.h>
#include <kernel/sched.h>
#include <kernel/spinlock.h>

struct cell {
    char    ch;
    color_t fg;
    color_t bg;
};

#define INPUT_RING 512

static struct cell *cells;
static int          cols, rows;
static int          cursor_x, cursor_y;
static color_t      current_fg, current_bg;
static color_t      default_fg, default_bg;
static bool_t       visible = true;
static bool_t       ready;
static console_hook_fn output_hook;

static int          input_ring[INPUT_RING];
static volatile int input_head, input_tail;
static spinlock_t   console_lock;

/* Escape sequence parser state. */
static enum { ESC_NONE, ESC_START, ESC_CSI } esc_state;
static int  esc_params[8];
static int  esc_param_count;

/* The standard 16 ANSI colours. */
static const color_t ansi_colors[16] = {
    RGB(30, 32, 40),    RGB(220, 80, 80),   RGB(120, 200, 120), RGB(220, 200, 100),
    RGB(90, 150, 230),  RGB(190, 120, 220), RGB(90, 200, 210),  RGB(200, 205, 215),
    RGB(90, 95, 110),   RGB(250, 120, 120), RGB(160, 230, 160), RGB(250, 230, 140),
    RGB(130, 185, 250), RGB(220, 160, 250), RGB(130, 230, 240), RGB(245, 248, 255),
};

int console_columns(void)
{
    return cols;
}

int console_rows(void)
{
    return rows;
}

void console_init(void)
{
    spinlock_init(&console_lock, "console");

    default_fg = RGB(205, 212, 225);
    default_bg = RGB(18, 20, 28);
    current_fg = default_fg;
    current_bg = default_bg;

    if (fb_available()) {
        cols = fb_width() / FONT_WIDTH;
        rows = fb_height() / FONT_HEIGHT;
    } else {
        cols = 80;
        rows = 25;
    }
    cols = MIN(cols, CONSOLE_COLS_MAX);
    rows = MIN(rows, CONSOLE_ROWS_MAX);

    cells = kzalloc((size_t)cols * rows * sizeof(struct cell));
    if (!cells) {
        KLOG_ERR("console", "cannot allocate the %dx%d cell grid", cols, rows);
        return;
    }

    console_clear();
    ready = true;

    KLOG_INFO("console", "%dx%d character console", cols, rows);
}

void console_set_hook(console_hook_fn hook)
{
    output_hook = hook;
}

void console_set_color(color_t fg, color_t bg)
{
    current_fg = fg;
    current_bg = bg;
}

void console_reset_color(void)
{
    current_fg = default_fg;
    current_bg = default_bg;
}

void console_clear(void)
{
    if (!cells) {
        return;
    }
    for (int i = 0; i < cols * rows; i++) {
        cells[i].ch = ' ';
        cells[i].fg = current_fg;
        cells[i].bg = current_bg;
    }
    cursor_x = 0;
    cursor_y = 0;
}

static void scroll_up(void)
{
    memmove(cells, cells + cols, (size_t)(rows - 1) * cols * sizeof(struct cell));

    struct cell *last = cells + (size_t)(rows - 1) * cols;
    for (int i = 0; i < cols; i++) {
        last[i].ch = ' ';
        last[i].fg = current_fg;
        last[i].bg = current_bg;
    }
    cursor_y = rows - 1;
}

static void newline(void)
{
    cursor_x = 0;
    if (++cursor_y >= rows) {
        scroll_up();
    }
}

/* Apply a Select Graphic Rendition sequence. */
static void apply_sgr(void)
{
    if (esc_param_count == 0) {
        console_reset_color();
        return;
    }
    for (int i = 0; i < esc_param_count; i++) {
        int code = esc_params[i];
        if (code == 0) {
            console_reset_color();
        } else if (code == 1) {
            /* Bold: use the bright variant of the current colour. */
            for (int c = 0; c < 8; c++) {
                if (current_fg == ansi_colors[c]) {
                    current_fg = ansi_colors[c + 8];
                    break;
                }
            }
        } else if (code == 7) {
            color_t swap = current_fg;
            current_fg   = current_bg;
            current_bg   = swap;
        } else if (code >= 30 && code <= 37) {
            current_fg = ansi_colors[code - 30];
        } else if (code >= 90 && code <= 97) {
            current_fg = ansi_colors[code - 90 + 8];
        } else if (code >= 40 && code <= 47) {
            current_bg = ansi_colors[code - 40];
        } else if (code >= 100 && code <= 107) {
            current_bg = ansi_colors[code - 100 + 8];
        } else if (code == 39) {
            current_fg = default_fg;
        } else if (code == 49) {
            current_bg = default_bg;
        }
    }
}

/* Feed one character through the escape sequence state machine.
 * Returns true when the character was consumed as part of a sequence. */
static bool_t handle_escape(char c)
{
    switch (esc_state) {
    case ESC_NONE:
        if (c == '\033') {
            esc_state       = ESC_START;
            esc_param_count = 0;
            memset(esc_params, 0, sizeof(esc_params));
            return true;
        }
        return false;

    case ESC_START:
        if (c == '[') {
            esc_state = ESC_CSI;
        } else {
            esc_state = ESC_NONE;
        }
        return true;

    case ESC_CSI:
        if (c >= '0' && c <= '9') {
            if (esc_param_count == 0) {
                esc_param_count = 1;
            }
            esc_params[esc_param_count - 1] =
                esc_params[esc_param_count - 1] * 10 + (c - '0');
            return true;
        }
        if (c == ';') {
            if (esc_param_count < (int)ARRAY_SIZE(esc_params)) {
                esc_param_count++;
            }
            return true;
        }
        switch (c) {
        case 'm':
            apply_sgr();
            break;
        case 'H':
        case 'f':
            cursor_y = CLAMP(esc_params[0] - 1, 0, rows - 1);
            cursor_x = CLAMP((esc_param_count > 1 ? esc_params[1] : 1) - 1, 0,
                             cols - 1);
            break;
        case 'J':
            console_clear();
            break;
        case 'K':
            for (int x = cursor_x; x < cols; x++) {
                cells[cursor_y * cols + x].ch = ' ';
                cells[cursor_y * cols + x].bg = current_bg;
            }
            break;
        case 'A':
            cursor_y = MAX(cursor_y - MAX(esc_params[0], 1), 0);
            break;
        case 'B':
            cursor_y = MIN(cursor_y + MAX(esc_params[0], 1), rows - 1);
            break;
        case 'C':
            cursor_x = MIN(cursor_x + MAX(esc_params[0], 1), cols - 1);
            break;
        case 'D':
            cursor_x = MAX(cursor_x - MAX(esc_params[0], 1), 0);
            break;
        default:
            break;
        }
        esc_state = ESC_NONE;
        return true;
    }
    return false;
}

void console_putc(char c)
{
    if (!cells) {
        serial_putc(c);
        return;
    }

    if (handle_escape(c)) {
        return;
    }

    switch (c) {
    case '\n':
        newline();
        return;
    case '\r':
        cursor_x = 0;
        return;
    case '\t':
        cursor_x = (cursor_x + 8) & ~7;
        if (cursor_x >= cols) {
            newline();
        }
        return;
    case '\b':
        if (cursor_x > 0) {
            cursor_x--;
            cells[cursor_y * cols + cursor_x].ch = ' ';
        }
        return;
    case '\0':
        return;
    default:
        break;
    }

    if (cursor_x >= cols) {
        newline();
    }

    struct cell *cell = &cells[cursor_y * cols + cursor_x];
    cell->ch = c;
    cell->fg = current_fg;
    cell->bg = current_bg;
    cursor_x++;
}

void console_write(const char *text, size_t len)
{
    if (output_hook) {
        output_hook(text, len);
        return;
    }
    for (size_t i = 0; i < len; i++) {
        console_putc(text[i]);
    }
}

void console_puts(const char *text)
{
    console_write(text, strlen(text));
}

void console_printf(const char *fmt, ...)
{
    char    buf[1024];
    va_list ap;

    va_start(ap, fmt);
    int n = vsnprintf(buf, sizeof(buf), fmt, ap);
    va_end(ap);

    console_write(buf, (n > 0) ? MIN((size_t)n, sizeof(buf) - 1) : 0);
}

void console_set_visible(bool_t value)
{
    visible = value;
}

bool_t console_visible(void)
{
    return visible;
}

void console_render(void)
{
    if (!ready || !visible || !fb_available()) {
        return;
    }

    for (int y = 0; y < rows; y++) {
        for (int x = 0; x < cols; x++) {
            struct cell *cell = &cells[y * cols + x];
            fb_draw_char_bg(x * FONT_WIDTH, y * FONT_HEIGHT, cell->ch, cell->fg,
                            cell->bg);
        }
    }

    /* Block cursor. */
    fb_fill_rect(cursor_x * FONT_WIDTH, cursor_y * FONT_HEIGHT + FONT_HEIGHT - 2,
                 FONT_WIDTH, 2, RGB(120, 190, 250));
}

/* --- input ----------------------------------------------------------- */

void console_push_char(int ch)
{
    int next = (input_head + 1) % INPUT_RING;

    if (next == input_tail) {
        return;   /* ring full, drop the keystroke */
    }
    input_ring[input_head] = ch;
    input_head             = next;
}

bool_t console_has_input(void)
{
    return input_head != input_tail;
}

int console_getchar_nonblock(void)
{
    if (input_head == input_tail) {
        return -1;
    }
    int ch     = input_ring[input_tail];
    input_tail = (input_tail + 1) % INPUT_RING;
    return ch;
}

int console_getchar(void)
{
    for (;;) {
        int ch = console_getchar_nonblock();
        if (ch >= 0) {
            return ch;
        }
        sched_yield();
    }
}

ssize_t console_read(void *buf, size_t len)
{
    char  *out   = (char *)buf;
    size_t count = 0;

    while (count < len) {
        int ch = console_getchar_nonblock();
        if (ch < 0) {
            if (count > 0) {
                break;
            }
            sched_yield();
            continue;
        }
        out[count++] = (char)ch;
        if (ch == '\n') {
            break;
        }
    }
    return (ssize_t)count;
}

int console_read_line(char *buf, size_t size)
{
    size_t length = 0;

    for (;;) {
        int ch = console_getchar();

        if (ch == '\n' || ch == '\r') {
            console_putc('\n');
            break;
        }
        if (ch == '\b' || ch == 127) {
            if (length > 0) {
                length--;
                console_puts("\b \b");
            }
            continue;
        }
        if (ch < 32 || ch > 126) {
            continue;
        }
        if (length + 1 < size) {
            buf[length++] = (char)ch;
            console_putc((char)ch);
        }
    }

    buf[length] = '\0';
    return (int)length;
}
