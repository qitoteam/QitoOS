/*
 * QitoOS - Text Editor
 *
 * A line-oriented editor with cursor movement, insertion, deletion and saving
 * back to the filesystem. Enough to edit configuration files and notes without
 * leaving the desktop.
 */

#include <kernel/desktop.h>
#include <kernel/fs.h>
#include <kernel/string.h>
#include <kernel/printf.h>
#include <kernel/mm.h>
#include <kernel/syscall.h>
#include <kernel/clipboard.h>

#define EDITOR_MAX_LINES 512
#define EDITOR_LINE_MAX  256
#define LINE_HEIGHT      FONT_HEIGHT
#define GUTTER_WIDTH     48

struct editor_state {
    char   lines[EDITOR_MAX_LINES][EDITOR_LINE_MAX];
    int    line_count;
    int    cursor_line;
    int    cursor_col;
    int    scroll;
    char   path[FS_PATH_MAX];
    bool_t modified;
    bool_t has_file;
    char   status[128];
};

static void editor_set_status(struct editor_state *state, const char *fmt, ...)
{
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(state->status, sizeof(state->status), fmt, ap);
    va_end(ap);
}

static void editor_load(struct window *win, const char *path)
{
    struct editor_state *state = (struct editor_state *)win->app_state;

    state->line_count  = 0;
    state->cursor_line = 0;
    state->cursor_col  = 0;
    state->scroll      = 0;
    state->modified    = false;

    strlcpy(state->path, path, sizeof(state->path));

    char   buffer[32768];
    size_t got = 0;

    if (fs_read_file(path, buffer, sizeof(buffer) - 1, &got) != 0) {
        /* A new file: start with one empty line. */
        state->line_count  = 1;
        state->lines[0][0] = '\0';
        state->has_file    = false;
        editor_set_status(state, "new file");
    } else {
        buffer[got] = '\0';
        state->has_file = true;

        const char *p = buffer;
        while (*p && state->line_count < EDITOR_MAX_LINES) {
            const char *end = strchr(p, '\n');
            size_t len = end ? (size_t)(end - p) : strlen(p);
            len = MIN(len, (size_t)EDITOR_LINE_MAX - 1);

            memcpy(state->lines[state->line_count], p, len);
            state->lines[state->line_count][len] = '\0';
            state->line_count++;

            if (!end) {
                break;
            }
            p = end + 1;
        }
        if (state->line_count == 0) {
            state->line_count  = 1;
            state->lines[0][0] = '\0';
        }
        editor_set_status(state, "%d lines, %llu bytes", state->line_count,
                          (unsigned long long)got);
    }

    char title[WINDOW_TITLE_MAX];
    const char *name = strrchr(path, '/');
    snprintf(title, sizeof(title), "Editor - %s", name ? name + 1 : path);
    window_set_title(win, title);
}

/* Called by the file manager when a file is opened. */
void editor_open_path(struct window *win, const char *path)
{
    editor_load(win, path);
}

static int editor_save(struct editor_state *state)
{
    char   buffer[32768];
    size_t pos = 0;

    for (int i = 0; i < state->line_count && pos < sizeof(buffer) - 2; i++) {
        size_t len = strlen(state->lines[i]);
        len = MIN(len, sizeof(buffer) - pos - 2);
        memcpy(buffer + pos, state->lines[i], len);
        pos += len;
        buffer[pos++] = '\n';
    }

    int error = fs_write_file(state->path, buffer, pos);
    if (error == 0) {
        state->modified = false;
        state->has_file = true;
        editor_set_status(state, "saved %d lines to %s", state->line_count,
                          state->path);
    } else {
        editor_set_status(state, "save failed: %s", qito_strerror(error));
    }
    return error;
}

static void editor_on_open(struct window *win)
{
    struct editor_state *state = (struct editor_state *)win->app_state;

    state->line_count  = 1;
    state->lines[0][0] = '\0';
    strlcpy(state->path, "/home/user/untitled.txt", sizeof(state->path));
    editor_set_status(state, "Ctrl+S save  Ctrl+O reload  Ctrl+N new line");
}

static void editor_draw(struct window *win, int x, int y, int w, int h)
{
    struct editor_state *state = (struct editor_state *)win->app_state;
    const struct theme  *theme = desktop_theme();

    /* Toolbar. */
    fb_fill_rect(x, y, w, 24, theme->titlebar_inactive);
    char header[160];
    snprintf(header, sizeof(header), "%s%s", state->path,
             state->modified ? "  *modified*" : "");
    fb_draw_string_clipped(x + 8, y + 4, header,
                           state->modified ? RGB(240, 190, 90) : theme->accent,
                           w - 16);
    fb_fill_rect(x, y + 23, w, 1, theme->border);

    /* Text area. */
    int text_y       = y + 26;
    int text_height  = h - (text_y - y) - 20;
    int visible_rows = text_height / LINE_HEIGHT;

    if (state->cursor_line < state->scroll) {
        state->scroll = state->cursor_line;
    }
    if (state->cursor_line >= state->scroll + visible_rows) {
        state->scroll = state->cursor_line - visible_rows + 1;
    }

    fb_fill_rect(x, text_y, GUTTER_WIDTH, text_height, theme->input_bg);
    fb_fill_rect(x + GUTTER_WIDTH, text_y, 1, text_height, theme->border);

    int max_cols = (w - GUTTER_WIDTH - 12) / FONT_WIDTH;

    for (int row = 0; row < visible_rows; row++) {
        int index = state->scroll + row;
        if (index >= state->line_count) {
            break;
        }

        int ry = text_y + row * LINE_HEIGHT;

        /* Highlight the cursor line. */
        if (index == state->cursor_line) {
            fb_fill_rect(x + GUTTER_WIDTH + 1, ry, w - GUTTER_WIDTH - 1, LINE_HEIGHT,
                         RGB(COLOR_R(theme->window_bg) + 8,
                             COLOR_G(theme->window_bg) + 8,
                             COLOR_B(theme->window_bg) + 12));
        }

        char number[12];
        snprintf(number, sizeof(number), "%4d", index + 1);
        fb_draw_string(x + 6, ry, number,
                       (index == state->cursor_line)
                           ? theme->accent
                           : theme->titlebar_text_inactive);

        fb_draw_string_clipped(x + GUTTER_WIDTH + 6, ry, state->lines[index],
                               theme->window_text, max_cols * FONT_WIDTH);
    }

    /* Cursor. */
    if (win->focused) {
        int cursor_row = state->cursor_line - state->scroll;
        if (cursor_row >= 0 && cursor_row < visible_rows) {
            int cx = x + GUTTER_WIDTH + 6 + state->cursor_col * FONT_WIDTH;
            int cy = text_y + cursor_row * LINE_HEIGHT;
            if (cx < x + w - 2) {
                fb_fill_rect(cx, cy, 2, LINE_HEIGHT, theme->accent);
            }
        }
    }

    /* Status bar. */
    int status_y = y + h - 20;
    fb_fill_rect(x, status_y, w, 20, theme->titlebar_inactive);
    fb_fill_rect(x, status_y, w, 1, theme->border);
    fb_draw_string_clipped(x + 8, status_y + 2, state->status,
                           theme->titlebar_text_inactive, w - 130);

    char position[48];
    snprintf(position, sizeof(position), "Ln %d, Col %d", state->cursor_line + 1,
             state->cursor_col + 1);
    fb_draw_string(x + w - fb_text_width(position) - 8, status_y + 2, position,
                   theme->titlebar_text_inactive);
}

static bool_t editor_on_key(struct window *win, uint32_t key, uint32_t modifiers)
{
    struct editor_state *state = (struct editor_state *)win->app_state;

    char *line     = state->lines[state->cursor_line];
    int   line_len = (int)strlen(line);

    /* Ctrl shortcuts. */
    if (modifiers & MOD_CTRL) {
        if (key == 's' || key == 'S' || key == 19) {
            editor_save(state);
            desktop_notify("Editor", state->modified ? "Save failed" : "File saved");
            return true;
        }
        if (key == 'o' || key == 'O' || key == 15) {
            editor_load(win, state->path);
            return true;
        }
        if (key == 'c' || key == 'C' || key == 3) {
            /* Copy the current line to the system clipboard. */
            clipboard_set(line, CLIP_TEXT, "Editor");
            editor_set_status(state, "copied line %d", state->cursor_line + 1);
            return true;
        }
        if (key == 'v' || key == 'V' || key == 22) {
            const char *pasted = clipboard_get();
            if (!pasted) {
                editor_set_status(state, "the clipboard is empty");
                return true;
            }

            /* Insert each character, splitting lines on newlines. */
            for (const char *p = pasted; *p; p++) {
                if (*p == '\n') {
                    editor_on_key(win, KEY_ENTER, 0);
                } else if (*p >= 32 && *p < 127) {
                    editor_on_key(win, (uint32_t)*p, 0);
                }
            }
            editor_set_status(state, "pasted %llu bytes",
                              (unsigned long long)clipboard_length());
            return true;
        }
        if (key == 'k' || key == 11) {
            /* Delete the current line. */
            if (state->line_count > 1) {
                for (int i = state->cursor_line; i < state->line_count - 1; i++) {
                    strlcpy(state->lines[i], state->lines[i + 1], EDITOR_LINE_MAX);
                }
                state->line_count--;
                state->cursor_line = MIN(state->cursor_line, state->line_count - 1);
            } else {
                state->lines[0][0] = '\0';
            }
            state->cursor_col = 0;
            state->modified   = true;
            return true;
        }
    }

    switch (key) {
    case KEY_UP:
        if (state->cursor_line > 0) {
            state->cursor_line--;
            state->cursor_col =
                MIN(state->cursor_col, (int)strlen(state->lines[state->cursor_line]));
        }
        return true;

    case KEY_DOWN:
        if (state->cursor_line < state->line_count - 1) {
            state->cursor_line++;
            state->cursor_col =
                MIN(state->cursor_col, (int)strlen(state->lines[state->cursor_line]));
        }
        return true;

    case KEY_LEFT:
        if (state->cursor_col > 0) {
            state->cursor_col--;
        } else if (state->cursor_line > 0) {
            state->cursor_line--;
            state->cursor_col = (int)strlen(state->lines[state->cursor_line]);
        }
        return true;

    case KEY_RIGHT:
        if (state->cursor_col < line_len) {
            state->cursor_col++;
        } else if (state->cursor_line < state->line_count - 1) {
            state->cursor_line++;
            state->cursor_col = 0;
        }
        return true;

    case KEY_HOME:
        state->cursor_col = 0;
        return true;

    case KEY_END:
        state->cursor_col = line_len;
        return true;

    case KEY_PAGEUP:
        state->cursor_line = MAX(state->cursor_line - 12, 0);
        state->cursor_col  = 0;
        return true;

    case KEY_PAGEDOWN:
        state->cursor_line = MIN(state->cursor_line + 12, state->line_count - 1);
        state->cursor_col  = 0;
        return true;

    case KEY_ENTER:   /* == '\n' */
    case '\r': {
        if (state->line_count >= EDITOR_MAX_LINES) {
            editor_set_status(state, "line limit reached (%d)", EDITOR_MAX_LINES);
            return true;
        }
        /* Split the current line at the cursor. */
        for (int i = state->line_count; i > state->cursor_line + 1; i--) {
            strlcpy(state->lines[i], state->lines[i - 1], EDITOR_LINE_MAX);
        }
        strlcpy(state->lines[state->cursor_line + 1], line + state->cursor_col,
                EDITOR_LINE_MAX);
        line[state->cursor_col] = '\0';

        state->line_count++;
        state->cursor_line++;
        state->cursor_col = 0;
        state->modified   = true;
        return true;
    }

    case KEY_BACKSPACE:   /* == '\b' */
    case 127:
        if (state->cursor_col > 0) {
            memmove(line + state->cursor_col - 1, line + state->cursor_col,
                    (size_t)(line_len - state->cursor_col + 1));
            state->cursor_col--;
            state->modified = true;
        } else if (state->cursor_line > 0) {
            /* Join with the previous line. */
            char *previous = state->lines[state->cursor_line - 1];
            int   prev_len = (int)strlen(previous);
            strlcat(previous, line, EDITOR_LINE_MAX);

            for (int i = state->cursor_line; i < state->line_count - 1; i++) {
                strlcpy(state->lines[i], state->lines[i + 1], EDITOR_LINE_MAX);
            }
            state->line_count--;
            state->cursor_line--;
            state->cursor_col = prev_len;
            state->modified   = true;
        }
        return true;

    case KEY_DELETE:
        if (state->cursor_col < line_len) {
            memmove(line + state->cursor_col, line + state->cursor_col + 1,
                    (size_t)(line_len - state->cursor_col));
            state->modified = true;
        }
        return true;

    case KEY_TAB:
        for (int i = 0; i < 4 && line_len + 1 < EDITOR_LINE_MAX; i++) {
            memmove(line + state->cursor_col + 1, line + state->cursor_col,
                    (size_t)(line_len - state->cursor_col + 1));
            line[state->cursor_col] = ' ';
            state->cursor_col++;
            line_len++;
        }
        state->modified = true;
        return true;

    default:
        break;
    }

    /* Printable characters. */
    if (key >= 32 && key < 127 && line_len + 1 < EDITOR_LINE_MAX) {
        memmove(line + state->cursor_col + 1, line + state->cursor_col,
                (size_t)(line_len - state->cursor_col + 1));
        line[state->cursor_col] = (char)key;
        state->cursor_col++;
        state->modified = true;
        return true;
    }

    return false;
}

static bool_t editor_on_mouse(struct window *win, int x, int y, uint32_t buttons,
                              input_event_type_t type)
{
    struct editor_state *state = (struct editor_state *)win->app_state;

    if (type != INPUT_MOUSE_DOWN || !(buttons & MOUSE_LEFT)) {
        return false;
    }
    if (y < 26) {
        return false;
    }

    int row = state->scroll + (y - 26) / LINE_HEIGHT;
    if (row < state->line_count) {
        state->cursor_line = row;
        int col = (x - GUTTER_WIDTH - 6) / FONT_WIDTH;
        state->cursor_col =
            CLAMP(col, 0, (int)strlen(state->lines[state->cursor_line]));
        return true;
    }
    return false;
}

static const struct app_ops editor_ops = {
    .draw     = editor_draw,
    .on_key   = editor_on_key,
    .on_mouse = editor_on_mouse,
    .on_open  = editor_on_open,
};

void app_editor_register(void)
{
    desktop_register_app("Editor", "Ed", &editor_ops, 660, 440, RGB(150, 180, 250),
                         sizeof(struct editor_state), true);
}
