/*
 * Qira OS - Notes
 *
 * A multi-note scratchpad. Each note is a file under /home/user/notes, so
 * anything written here is visible from the shell and the file manager too.
 * Notes are saved automatically a moment after typing stops, which is the
 * behaviour people expect from a scratchpad and avoids losing work to a
 * closed window.
 */

#include <kernel/desktop.h>
#include <kernel/fs.h>
#include <kernel/string.h>
#include <kernel/printf.h>
#include <kernel/mm.h>
#include <kernel/time.h>
#include <kernel/font.h>
#include <kernel/clipboard.h>

#define NOTES_DIR       "/home/user/notes"
#define MAX_NOTES       24
#define NOTE_TITLE_MAX  40
#define NOTE_BODY_MAX   4096
#define AUTOSAVE_MS     1200

struct note {
    char   title[NOTE_TITLE_MAX];
    char   filename[64];
    bool_t loaded;
};

struct notes_state {
    struct note notes[MAX_NOTES];
    int    note_count;
    int    selected;

    char   body[NOTE_BODY_MAX];
    int    body_length;
    int    cursor;
    int    scroll;

    bool_t dirty;
    bool_t renaming;
    char   rename_buffer[NOTE_TITLE_MAX];

    uint64_t last_edit_ms;
    char     status[128];
};

/* Turn a title into a safe filename. */
static void title_to_filename(const char *title, char *out, size_t size)
{
    size_t index = 0;

    for (const char *p = title; *p && index < size - 6; p++) {
        char c = *p;
        if (isalnum((uint8_t)c)) {
            out[index++] = (char)tolower((uint8_t)c);
        } else if (c == ' ' || c == '-' || c == '_') {
            if (index > 0 && out[index - 1] != '-') {
                out[index++] = '-';
            }
        }
    }
    if (index == 0) {
        out[index++] = 'n';
    }
    strlcpy(out + index, ".txt", size - index);
}

static void notes_scan(struct notes_state *state)
{
    state->note_count = 0;

    struct fs_node *dir = fs_lookup(NOTES_DIR);
    if (!dir) {
        fs_mkdir(NOTES_DIR, 0755);
        dir = fs_lookup(NOTES_DIR);
    }
    if (!dir) {
        strlcpy(state->status, "cannot open " NOTES_DIR, sizeof(state->status));
        return;
    }

    struct fs_dirent entry;
    for (int i = 0; fs_readdir(dir, i, &entry) == 0; i++) {
        if (state->note_count >= MAX_NOTES) {
            break;
        }
        if (entry.type == FS_DIR) {
            continue;
        }

        struct note *note = &state->notes[state->note_count++];
        strlcpy(note->filename, entry.name, sizeof(note->filename));

        /* The title is the first line of the file, or the filename. */
        char   path[FS_PATH_MAX];
        char   head[128];
        size_t got = 0;

        snprintf(path, sizeof(path), "%s/%s", NOTES_DIR, entry.name);

        if (fs_read_file(path, head, sizeof(head) - 1, &got) == 0 && got > 0) {
            head[got] = '\0';
            char *newline = strchr(head, '\n');
            if (newline) {
                *newline = '\0';
            }
            if (head[0]) {
                strlcpy(note->title, head, sizeof(note->title));
            } else {
                strlcpy(note->title, entry.name, sizeof(note->title));
            }
        } else {
            strlcpy(note->title, entry.name, sizeof(note->title));
        }
    }

    snprintf(state->status, sizeof(state->status), "%d note(s)",
             state->note_count);
}

static void notes_load(struct notes_state *state, int index)
{
    if (index < 0 || index >= state->note_count) {
        state->body[0]     = '\0';
        state->body_length = 0;
        state->cursor      = 0;
        return;
    }

    char   path[FS_PATH_MAX];
    size_t got = 0;

    snprintf(path, sizeof(path), "%s/%s", NOTES_DIR,
             state->notes[index].filename);

    if (fs_read_file(path, state->body, NOTE_BODY_MAX - 1, &got) == 0) {
        state->body[got]   = '\0';
        state->body_length = (int)got;
    } else {
        state->body[0]     = '\0';
        state->body_length = 0;
    }

    state->cursor = state->body_length;
    state->scroll = 0;
    state->dirty  = false;
    state->selected = index;
}

static void notes_save(struct notes_state *state)
{
    if (state->selected < 0 || state->selected >= state->note_count) {
        return;
    }

    char path[FS_PATH_MAX];
    snprintf(path, sizeof(path), "%s/%s", NOTES_DIR,
             state->notes[state->selected].filename);

    if (fs_write_file(path, state->body, (size_t)state->body_length) == 0) {
        state->dirty = false;

        /* The title tracks the first line. */
        char title[NOTE_TITLE_MAX];
        int  length = 0;
        while (length < state->body_length && state->body[length] != '\n' &&
               length < NOTE_TITLE_MAX - 1) {
            title[length] = state->body[length];
            length++;
        }
        title[length] = '\0';

        if (title[0]) {
            strlcpy(state->notes[state->selected].title, title,
                    sizeof(state->notes[0].title));
        }

        struct qira_time now;
        time_from_unix(rtc_unix_time(), &now);
        snprintf(state->status, sizeof(state->status), "saved at %02d:%02d:%02d",
                 now.hour, now.minute, now.second);
    } else {
        strlcpy(state->status, "save failed", sizeof(state->status));
    }
}

static void notes_create(struct notes_state *state)
{
    if (state->note_count >= MAX_NOTES) {
        strlcpy(state->status, "no room for another note",
                sizeof(state->status));
        return;
    }

    struct note *note = &state->notes[state->note_count];

    struct qira_time now;
    time_from_unix(rtc_unix_time(), &now);
    snprintf(note->title, sizeof(note->title), "Note %02d-%02d %02d:%02d",
             now.day, now.month, now.hour, now.minute);

    /* Ensure the filename is unique. */
    char base[64];
    title_to_filename(note->title, base, sizeof(base));
    strlcpy(note->filename, base, sizeof(note->filename));

    for (int attempt = 1; attempt < 100; attempt++) {
        char path[FS_PATH_MAX];
        snprintf(path, sizeof(path), "%s/%s", NOTES_DIR, note->filename);
        if (!fs_lookup(path)) {
            break;
        }
        snprintf(note->filename, sizeof(note->filename), "note-%d.txt", attempt);
    }

    state->note_count++;
    state->selected    = state->note_count - 1;
    state->body_length = (int)strlcpy(state->body, note->title, NOTE_BODY_MAX);
    state->body[state->body_length++] = '\n';
    state->body[state->body_length]   = '\0';
    state->cursor = state->body_length;
    state->dirty  = true;

    notes_save(state);
    strlcpy(state->status, "new note", sizeof(state->status));
}

static void notes_delete(struct notes_state *state)
{
    if (state->selected < 0 || state->selected >= state->note_count) {
        return;
    }

    char path[FS_PATH_MAX];
    snprintf(path, sizeof(path), "%s/%s", NOTES_DIR,
             state->notes[state->selected].filename);
    fs_unlink(path);

    for (int i = state->selected; i < state->note_count - 1; i++) {
        state->notes[i] = state->notes[i + 1];
    }
    state->note_count--;

    state->selected = MIN(state->selected, state->note_count - 1);
    notes_load(state, state->selected);
    strlcpy(state->status, "note deleted", sizeof(state->status));
}

static void notes_on_open(struct window *win)
{
    struct notes_state *state = (struct notes_state *)win->app_state;

    notes_scan(state);
    if (state->note_count > 0) {
        notes_load(state, 0);
    } else {
        notes_create(state);
    }
    window_set_title(win, "Notes");
}

static void notes_on_close(struct window *win)
{
    struct notes_state *state = (struct notes_state *)win->app_state;
    if (state->dirty) {
        notes_save(state);
    }
}

static void notes_draw(struct window *win, int x, int y, int w, int h)
{
    struct notes_state *state = (struct notes_state *)win->app_state;
    const struct theme *theme = desktop_theme();

    /* Note list. */
    int list_w = 190;
    fb_fill_rect(x, y, list_w, h - 20, theme->input_bg);
    fb_fill_rect(x + list_w, y, 1, h - 20, theme->border);

    fb_fill_rect(x, y, list_w, 24, theme->titlebar_inactive);
    fb_draw_string(x + 10, y + 4, "Notes", theme->accent);
    fb_draw_string(x + list_w - 24, y + 4, "+", theme->accent);

    for (int i = 0; i < state->note_count; i++) {
        int ry = y + 30 + i * 30;
        if (ry > y + h - 50) {
            break;
        }

        bool_t selected = (i == state->selected);
        if (selected) {
            fb_fill_rect(x + 2, ry - 2, list_w - 4, 28, theme->accent);
        }

        fb_draw_string_clipped(x + 10, ry, state->notes[i].title,
                               selected ? RGB(255, 255, 255)
                                        : theme->window_text,
                               list_w - 20);
        fb_draw_string_clipped(x + 10, ry + 13, state->notes[i].filename,
                               selected ? RGB(220, 228, 245)
                                        : theme->titlebar_text_inactive,
                               list_w - 20);
    }

    /* Editor. */
    int text_x = x + list_w + 12;
    int text_w = w - list_w - 24;
    int text_y = y + 10;
    int text_h = h - 40;
    int columns = MAX(text_w / FONT_WIDTH, 10);
    int rows    = text_h / FONT_HEIGHT;

    /* Lay the body out into wrapped display lines. */
    int line = 0;
    int column = 0;
    int cursor_row = 0;
    int cursor_col = 0;

    for (int i = 0; i <= state->body_length && line < rows + state->scroll; i++) {
        if (i == state->cursor) {
            cursor_row = line;
            cursor_col = column;
        }
        if (i == state->body_length) {
            break;
        }

        char c = state->body[i];

        if (c == '\n') {
            line++;
            column = 0;
            continue;
        }

        if (line >= state->scroll && line - state->scroll < rows &&
            column < columns) {
            /* The first line of a note is its title, so it is emphasised. */
            color_t colour = (line == 0) ? theme->accent : theme->window_text;
            fb_draw_char_in(font_terminal(), text_x + column * FONT_WIDTH,
                            text_y + (line - state->scroll) * FONT_HEIGHT, c,
                            colour, 1);
        }

        column++;
        if (column >= columns) {
            line++;
            column = 0;
        }
    }

    /* Keep the cursor visible. */
    if (cursor_row < state->scroll) {
        state->scroll = cursor_row;
    }
    if (cursor_row >= state->scroll + rows) {
        state->scroll = cursor_row - rows + 1;
    }

    if (win->focused && cursor_row >= state->scroll &&
        cursor_row < state->scroll + rows) {
        fb_fill_rect(text_x + cursor_col * FONT_WIDTH,
                     text_y + (cursor_row - state->scroll) * FONT_HEIGHT, 2,
                     FONT_HEIGHT, theme->accent);
    }

    /* Status bar. */
    int status_y = y + h - 18;
    fb_fill_rect(x, status_y, w, 18, theme->titlebar_inactive);

    char left[160];
    snprintf(left, sizeof(left), "%s%s   %d characters", state->status,
             state->dirty ? "  (unsaved)" : "", state->body_length);
    fb_draw_string_clipped(x + 8, status_y + 1, left,
                           theme->titlebar_text_inactive, w - 210);
    fb_draw_string(x + w - 200, status_y + 1,
                   "Ctrl+N new   Ctrl+D delete   autosaves",
                   theme->titlebar_text_inactive);
}

static bool_t notes_on_key(struct window *win, uint32_t key, uint32_t modifiers)
{
    struct notes_state *state = (struct notes_state *)win->app_state;

    if (modifiers & MOD_CTRL) {
        if (key == 'n' || key == 'N' || key == 14) {
            if (state->dirty) {
                notes_save(state);
            }
            notes_create(state);
            return true;
        }
        if (key == 'd' || key == 'D' || key == 4) {
            notes_delete(state);
            return true;
        }
        if (key == 's' || key == 'S' || key == 19) {
            notes_save(state);
            return true;
        }
        if (key == 'c' || key == 'C' || key == 3) {
            clipboard_set_len(state->body, (size_t)state->body_length,
                              CLIP_TEXT, "Notes");
            strlcpy(state->status, "note copied to the clipboard",
                    sizeof(state->status));
            return true;
        }
        if (key == 'v' || key == 'V' || key == 22) {
            const char *pasted = clipboard_get();
            if (!pasted) {
                strlcpy(state->status, "the clipboard is empty",
                        sizeof(state->status));
                return true;
            }

            size_t length = clipboard_length();
            if (state->body_length + (int)length < NOTE_BODY_MAX) {
                memmove(state->body + state->cursor + length,
                        state->body + state->cursor,
                        (size_t)(state->body_length - state->cursor + 1));
                memcpy(state->body + state->cursor, pasted, length);
                state->cursor += (int)length;
                state->body_length += (int)length;
                state->dirty        = true;
                state->last_edit_ms = time_uptime_ms();
                snprintf(state->status, sizeof(state->status),
                         "pasted %llu bytes", (unsigned long long)length);
            }
            return true;
        }
    }

    switch (key) {
    case KEY_UP:
        /* Move up roughly one display line. */
        for (int i = 0; i < 40 && state->cursor > 0; i++) {
            state->cursor--;
            if (state->body[state->cursor] == '\n') {
                break;
            }
        }
        return true;
    case KEY_DOWN:
        for (int i = 0; i < 40 && state->cursor < state->body_length; i++) {
            state->cursor++;
            if (state->cursor < state->body_length &&
                state->body[state->cursor] == '\n') {
                state->cursor++;
                break;
            }
        }
        return true;
    case KEY_LEFT:
        if (state->cursor > 0) {
            state->cursor--;
        }
        return true;
    case KEY_RIGHT:
        if (state->cursor < state->body_length) {
            state->cursor++;
        }
        return true;
    case KEY_HOME:
        while (state->cursor > 0 && state->body[state->cursor - 1] != '\n') {
            state->cursor--;
        }
        return true;
    case KEY_END:
        while (state->cursor < state->body_length &&
               state->body[state->cursor] != '\n') {
            state->cursor++;
        }
        return true;

    case KEY_BACKSPACE:
    case 127:
        if (state->cursor > 0) {
            memmove(state->body + state->cursor - 1, state->body + state->cursor,
                    (size_t)(state->body_length - state->cursor + 1));
            state->cursor--;
            state->body_length--;
            state->dirty        = true;
            state->last_edit_ms = time_uptime_ms();
        }
        return true;

    case KEY_DELETE:
        if (state->cursor < state->body_length) {
            memmove(state->body + state->cursor, state->body + state->cursor + 1,
                    (size_t)(state->body_length - state->cursor));
            state->body_length--;
            state->dirty        = true;
            state->last_edit_ms = time_uptime_ms();
        }
        return true;

    default:
        break;
    }

    /* Printable characters, plus Enter and Tab. */
    char inserted = 0;
    if (key == KEY_ENTER || key == '\r') {
        inserted = '\n';
    } else if (key == KEY_TAB) {
        inserted = ' ';
    } else if (key >= 32 && key < 127) {
        inserted = (char)key;
    }

    if (inserted && state->body_length + 1 < NOTE_BODY_MAX) {
        memmove(state->body + state->cursor + 1, state->body + state->cursor,
                (size_t)(state->body_length - state->cursor + 1));
        state->body[state->cursor] = inserted;
        state->cursor++;
        state->body_length++;
        state->dirty        = true;
        state->last_edit_ms = time_uptime_ms();
        return true;
    }

    return false;
}

static bool_t notes_on_mouse(struct window *win, int x, int y, uint32_t buttons,
                             input_event_type_t type)
{
    struct notes_state *state = (struct notes_state *)win->app_state;

    if (type != INPUT_MOUSE_DOWN || !(buttons & MOUSE_LEFT)) {
        return false;
    }
    if (x >= 190) {
        return false;
    }

    /* The + button creates a note. */
    if (y < 24) {
        if (x >= 190 - 30) {
            if (state->dirty) {
                notes_save(state);
            }
            notes_create(state);
            return true;
        }
        return false;
    }

    int index = (y - 28) / 30;
    if (index >= 0 && index < state->note_count && index != state->selected) {
        if (state->dirty) {
            notes_save(state);
        }
        notes_load(state, index);
        return true;
    }
    return false;
}

/* Autosave a short while after the last keystroke. */
static bool_t notes_tick(struct window *win)
{
    struct notes_state *state = (struct notes_state *)win->app_state;

    if (state->dirty && state->last_edit_ms &&
        time_uptime_ms() - state->last_edit_ms > AUTOSAVE_MS) {
        notes_save(state);
        return true;
    }
    return false;
}

static const struct app_ops notes_ops = {
    .draw     = notes_draw,
    .on_key   = notes_on_key,
    .on_mouse = notes_on_mouse,
    .tick     = notes_tick,
    .on_open  = notes_on_open,
    .on_close = notes_on_close,
};

void app_notes_register(void)
{
    desktop_register_app("Notes", "==", &notes_ops, 700, 460,
                         RGB(240, 220, 130), sizeof(struct notes_state), true);
}
