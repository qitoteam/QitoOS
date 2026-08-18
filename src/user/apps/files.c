/*
 * Qira OS - File Manager
 *
 * Browses the virtual filesystem: navigate directories, see sizes and
 * permissions, open text files in the editor and delete entries.
 */

#include <kernel/desktop.h>
#include <kernel/fs.h>
#include <kernel/string.h>
#include <kernel/printf.h>
#include <kernel/mm.h>
#include <kernel/time.h>

#define FILES_MAX_ENTRIES 256
#define ROW_HEIGHT        20

struct files_state {
    char path[FS_PATH_MAX];
    struct fs_dirent entries[FILES_MAX_ENTRIES];
    int  entry_count;
    int  selected;
    int  scroll;
    bool_t needs_refresh;
    char status[128];
};

static void files_refresh(struct files_state *state)
{
    state->entry_count = 0;

    struct fs_node *dir = fs_lookup(state->path);
    if (!dir || dir->type != FS_DIR) {
        snprintf(state->status, sizeof(state->status), "cannot open %s",
                 state->path);
        return;
    }

    for (int i = 0; i < FILES_MAX_ENTRIES; i++) {
        if (fs_readdir(dir, i, &state->entries[state->entry_count]) != 0) {
            break;
        }
        state->entry_count++;
    }

    /* Directories first, then alphabetical. */
    for (int i = 1; i < state->entry_count; i++) {
        struct fs_dirent key = state->entries[i];
        int j = i - 1;
        while (j >= 0) {
            bool_t key_is_dir = (key.type == FS_DIR);
            bool_t cmp_is_dir = (state->entries[j].type == FS_DIR);
            bool_t swap = (!cmp_is_dir && key_is_dir) ||
                          (cmp_is_dir == key_is_dir &&
                           strcmp(state->entries[j].name, key.name) > 0);
            if (!swap) {
                break;
            }
            state->entries[j + 1] = state->entries[j];
            j--;
        }
        state->entries[j + 1] = key;
    }

    uint64_t total = 0;
    int      files = 0, dirs = 0;
    for (int i = 0; i < state->entry_count; i++) {
        if (state->entries[i].type == FS_DIR) {
            dirs++;
        } else {
            files++;
            total += state->entries[i].size;
        }
    }
    snprintf(state->status, sizeof(state->status),
             "%d directories, %d files, %llu bytes", dirs, files,
             (unsigned long long)total);

    state->selected      = CLAMP(state->selected, 0, MAX(state->entry_count - 1, 0));
    state->needs_refresh = false;
}

static void files_navigate(struct window *win, const char *target)
{
    struct files_state *state = (struct files_state *)win->app_state;

    char resolved[FS_PATH_MAX];
    fs_resolve_path(state->path, target, resolved, sizeof(resolved));

    struct fs_node *node = fs_lookup(resolved);
    if (node && node->type == FS_DIR) {
        strlcpy(state->path, resolved, sizeof(state->path));
        state->selected = 0;
        state->scroll   = 0;
        files_refresh(state);

        char title[WINDOW_TITLE_MAX];
        snprintf(title, sizeof(title), "Files - %s", state->path);
        window_set_title(win, title);
    }
}

static void files_on_open(struct window *win)
{
    struct files_state *state = (struct files_state *)win->app_state;
    strlcpy(state->path, "/", sizeof(state->path));
    files_refresh(state);
    window_set_title(win, "Files - /");
}

static void files_draw(struct window *win, int x, int y, int w, int h)
{
    struct files_state *state = (struct files_state *)win->app_state;
    const struct theme *theme = desktop_theme();

    if (state->needs_refresh) {
        files_refresh(state);
    }

    /* Path bar. */
    fb_fill_rect(x, y, w, 26, theme->titlebar_inactive);
    fb_draw_string(x + 8, y + 5, "Path:", theme->titlebar_text_inactive);
    fb_draw_string_clipped(x + 8 + fb_text_width("Path: "), y + 5, state->path,
                           theme->accent, w - 80);
    fb_fill_rect(x, y + 25, w, 1, theme->border);

    /* Column headings. */
    int list_y = y + 26;
    fb_fill_rect(x, list_y, w, 18, theme->window_bg);
    fb_draw_string(x + 30, list_y + 1, "NAME", theme->titlebar_text_inactive);
    fb_draw_string(x + w - 210, list_y + 1, "TYPE", theme->titlebar_text_inactive);
    fb_draw_string(x + w - 110, list_y + 1, "SIZE", theme->titlebar_text_inactive);
    fb_fill_rect(x, list_y + 17, w, 1, theme->border);

    list_y += 18;
    int list_height  = h - (list_y - y) - 22;
    int visible_rows = list_height / ROW_HEIGHT;

    /* Keep the selection on screen. */
    if (state->selected < state->scroll) {
        state->scroll = state->selected;
    }
    if (state->selected >= state->scroll + visible_rows) {
        state->scroll = state->selected - visible_rows + 1;
    }

    for (int row = 0; row < visible_rows; row++) {
        int index = state->scroll + row;
        if (index >= state->entry_count) {
            break;
        }

        struct fs_dirent *entry = &state->entries[index];
        int ry = list_y + row * ROW_HEIGHT;

        bool_t selected = (index == state->selected);
        if (selected) {
            fb_fill_rect(x + 2, ry, w - 4, ROW_HEIGHT, theme->accent);
        }

        color_t text = selected ? RGB(255, 255, 255)
                                : (entry->type == FS_DIR ? theme->accent
                                                         : theme->window_text);

        /* Type glyph. */
        const char *glyph = (entry->type == FS_DIR)   ? "[D]"
                            : (entry->type == FS_DEV) ? "[C]"
                                                      : " . ";
        fb_draw_string(x + 6, ry + 2, glyph, text);
        fb_draw_string_clipped(x + 34, ry + 2, entry->name, text, w - 250);

        const char *type_name = (entry->type == FS_DIR)   ? "directory"
                                : (entry->type == FS_DEV) ? "device"
                                                          : "file";
        fb_draw_string(x + w - 210, ry + 2, type_name,
                       selected ? RGB(230, 235, 245) : theme->titlebar_text_inactive);

        if (entry->type != FS_DIR) {
            char size[24];
            if (entry->size >= 1024 * 1024) {
                snprintf(size, sizeof(size), "%lluM",
                         (unsigned long long)(entry->size / (1024 * 1024)));
            } else if (entry->size >= 1024) {
                snprintf(size, sizeof(size), "%lluK",
                         (unsigned long long)(entry->size / 1024));
            } else {
                snprintf(size, sizeof(size), "%llu",
                         (unsigned long long)entry->size);
            }
            fb_draw_string(x + w - 110, ry + 2, size,
                           selected ? RGB(230, 235, 245) : theme->window_text);
        }
    }

    if (state->entry_count == 0) {
        fb_draw_string(x + 34, list_y + 8, "(empty directory)",
                       theme->titlebar_text_inactive);
    }

    /* Status bar. */
    int status_y = y + h - 20;
    fb_fill_rect(x, status_y, w, 20, theme->titlebar_inactive);
    fb_fill_rect(x, status_y, w, 1, theme->border);
    fb_draw_string_clipped(x + 8, status_y + 2, state->status,
                           theme->titlebar_text_inactive, w - 200);
    fb_draw_string(x + w - 190, status_y + 2,
                   "Enter open  Bksp up  Del remove",
                   theme->titlebar_text_inactive);
}

static bool_t files_on_key(struct window *win, uint32_t key, uint32_t modifiers)
{
    struct files_state *state = (struct files_state *)win->app_state;
    UNUSED(modifiers);

    switch (key) {
    case KEY_UP:
        state->selected = MAX(state->selected - 1, 0);
        return true;
    case KEY_DOWN:
        state->selected = MIN(state->selected + 1, MAX(state->entry_count - 1, 0));
        return true;
    case KEY_PAGEUP:
        state->selected = MAX(state->selected - 10, 0);
        return true;
    case KEY_PAGEDOWN:
        state->selected = MIN(state->selected + 10, MAX(state->entry_count - 1, 0));
        return true;
    case KEY_HOME:
        state->selected = 0;
        return true;
    case KEY_END:
        state->selected = MAX(state->entry_count - 1, 0);
        return true;

    case KEY_BACKSPACE:   /* == '\b' */
        files_navigate(win, "..");
        return true;

    case KEY_ENTER:
    case '\r': {
        if (state->selected >= state->entry_count) {
            return true;
        }
        struct fs_dirent *entry = &state->entries[state->selected];
        if (entry->type == FS_DIR) {
            files_navigate(win, entry->name);
        } else {
            /* Open the file in the editor. */
            char full[FS_PATH_MAX];
            fs_resolve_path(state->path, entry->name, full, sizeof(full));

            struct window *editor = window_create("Editor");
            if (editor) {
                extern void editor_open_path(struct window *win, const char *path);
                editor_open_path(editor, full);
            }
        }
        return true;
    }

    case KEY_DELETE: {
        if (state->selected >= state->entry_count) {
            return true;
        }
        char full[FS_PATH_MAX];
        fs_resolve_path(state->path, state->entries[state->selected].name, full,
                        sizeof(full));

        int error = fs_unlink(full);
        if (error == 0) {
            desktop_notify("Files", "Deleted");
            files_refresh(state);
        } else {
            desktop_notify("Files", "Could not delete that entry");
        }
        return true;
    }

    case KEY_F5:
        files_refresh(state);
        return true;

    default:
        break;
    }
    return false;
}

static bool_t files_on_mouse(struct window *win, int x, int y, uint32_t buttons,
                             input_event_type_t type)
{
    struct files_state *state = (struct files_state *)win->app_state;

    if (type != INPUT_MOUSE_DOWN || !(buttons & MOUSE_LEFT)) {
        return false;
    }

    int list_top = 44;   /* path bar + heading */
    if (y < list_top) {
        return false;
    }

    int index = state->scroll + (y - list_top) / ROW_HEIGHT;
    if (index < state->entry_count) {
        UNUSED(x);
        state->selected = index;
        return true;
    }
    return false;
}

static const struct app_ops files_ops = {
    .draw     = files_draw,
    .on_key   = files_on_key,
    .on_mouse = files_on_mouse,
    .on_open  = files_on_open,
};

void app_files_register(void)
{
    desktop_register_app("Files", "[]", &files_ops, 640, 420, RGB(240, 190, 90),
                         sizeof(struct files_state), true);
}
