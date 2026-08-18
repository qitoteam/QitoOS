/*
 * Qira OS - Icon Viewer
 *
 * Browses the QAC icons the system has loaded and inspects .qac files on
 * disk. Besides being useful, it is the practical test that the icon decoder
 * handles all three encodings correctly: anything it gets wrong is visible
 * immediately.
 */

#include <kernel/desktop.h>
#include <kernel/qac.h>
#include <kernel/fs.h>
#include <kernel/string.h>
#include <kernel/printf.h>
#include <kernel/mm.h>

#define VIEW_MAX_FILES 48

struct imageview_state {
    char   files[VIEW_MAX_FILES][64];
    int    file_count;
    int    selected;
    int    zoom;              /* rendered size in pixels */
    bool_t show_grid;

    struct qac_image  loaded;
    bool_t            has_loaded;
    struct qac_header header;
    bool_t            has_header;
    char              status[160];
};

static void scan_icons(struct imageview_state *state)
{
    state->file_count = 0;

    struct fs_node *dir = fs_lookup("/usr/share/icons");
    if (!dir) {
        strlcpy(state->status, "/usr/share/icons does not exist",
                sizeof(state->status));
        return;
    }

    struct fs_dirent entry;
    for (int i = 0; fs_readdir(dir, i, &entry) == 0; i++) {
        if (state->file_count >= VIEW_MAX_FILES) {
            break;
        }
        size_t len = strlen(entry.name);
        if (len > 4 && strcmp(entry.name + len - 4, ".qac") == 0) {
            strlcpy(state->files[state->file_count++], entry.name,
                    sizeof(state->files[0]));
        }
    }

    snprintf(state->status, sizeof(state->status), "%d icon file(s) found",
             state->file_count);
}

static void load_selected(struct imageview_state *state)
{
    if (state->has_loaded) {
        qac_free(&state->loaded);
        state->has_loaded = false;
    }
    state->has_header = false;

    if (state->selected < 0 || state->selected >= state->file_count) {
        return;
    }

    char path[FS_PATH_MAX];
    snprintf(path, sizeof(path), "/usr/share/icons/%s",
             state->files[state->selected]);

    /* Read the header separately so its details can be shown. */
    char   buffer[1024];
    size_t got = 0;
    if (fs_read_file(path, buffer, sizeof(buffer), &got) == 0) {
        if (qac_probe(buffer, got, &state->header) == 0) {
            state->has_header = true;
        }
    }

    if (qac_load(path, 32, &state->loaded) == 0) {
        state->has_loaded = true;
        snprintf(state->status, sizeof(state->status), "%s   %dx%d",
                 state->files[state->selected], state->loaded.width,
                 state->loaded.height);
    } else {
        snprintf(state->status, sizeof(state->status), "%s: could not decode",
                 state->files[state->selected]);
    }
}

static void imageview_on_open(struct window *win)
{
    struct imageview_state *state = (struct imageview_state *)win->app_state;

    state->zoom      = 128;
    state->show_grid = true;
    scan_icons(state);
    load_selected(state);
    window_set_title(win, "Icon Viewer");
}

static void imageview_on_close(struct window *win)
{
    struct imageview_state *state = (struct imageview_state *)win->app_state;
    if (state->has_loaded) {
        qac_free(&state->loaded);
    }
}

static void imageview_draw(struct window *win, int x, int y, int w, int h)
{
    struct imageview_state *state = (struct imageview_state *)win->app_state;
    const struct theme     *theme = desktop_theme();

    /* File list down the left. */
    int list_w = 170;
    fb_fill_rect(x, y, list_w, h - 20, theme->input_bg);
    fb_fill_rect(x + list_w, y, 1, h - 20, theme->border);

    for (int i = 0; i < state->file_count; i++) {
        int ry = y + 6 + i * 18;
        if (ry > y + h - 40) {
            break;
        }

        bool_t selected = (i == state->selected);
        if (selected) {
            fb_fill_rect(x + 2, ry - 2, list_w - 4, 18, theme->accent);
        }

        /* A small preview beside each name. */
        const struct qac_image *thumbnail = NULL;
        char name[64];
        strlcpy(name, state->files[i], sizeof(name));
        size_t len = strlen(name);
        if (len > 4) {
            name[len - 4] = '\0';
        }
        thumbnail = qac_get(name);

        if (thumbnail) {
            qac_draw_scaled(thumbnail, x + 6, ry - 1, 14);
        }

        fb_draw_string_clipped(x + 26, ry, name,
                               selected ? RGB(255, 255, 255)
                                        : theme->window_text,
                               list_w - 34);
    }

    /* Preview area. */
    int panel_x = x + list_w + 1;
    int panel_w = w - list_w - 1;

    if (state->has_loaded) {
        int size = MIN(state->zoom, MIN(panel_w - 40, h - 160));
        int ix   = panel_x + panel_w / 2 - size / 2;
        int iy   = y + 30;

        /*
         * A chequerboard behind the icon, so transparent pixels are visibly
         * transparent rather than looking like the window background.
         */
        int cell = 8;
        for (int row = 0; row < size; row += cell) {
            for (int col = 0; col < size; col += cell) {
                bool_t light = ((row / cell) + (col / cell)) % 2 == 0;
                fb_fill_rect(ix + col, iy + row, MIN(cell, size - col),
                             MIN(cell, size - row),
                             light ? RGB(58, 62, 78) : RGB(44, 48, 62));
            }
        }

        qac_draw_scaled(&state->loaded, ix, iy, size);
        fb_draw_rect(ix - 1, iy - 1, size + 2, size + 2, theme->border);

        /* A pixel grid, useful when inspecting the artwork closely. */
        if (state->show_grid && size >= 128) {
            int step = size / state->loaded.width;
            if (step >= 4) {
                for (int i = 1; i < state->loaded.width; i++) {
                    fb_fill_rect_alpha(ix + i * step, iy, 1, size,
                                       RGB(255, 255, 255), 30);
                    fb_fill_rect_alpha(ix, iy + i * step, size, 1,
                                       RGB(255, 255, 255), 30);
                }
            }
        }

        /* Header details. */
        int info_y = iy + size + 16;
        if (state->has_header) {
            char line[128];

            snprintf(line, sizeof(line), "Format      QACI version %u",
                     state->header.version);
            fb_draw_string(panel_x + 20, info_y, line,
                           theme->titlebar_text_inactive);

            snprintf(line, sizeof(line), "Frames      %u",
                     state->header.frame_count);
            fb_draw_string(panel_x + 20, info_y + 18, line,
                           theme->titlebar_text_inactive);

            snprintf(line, sizeof(line), "Payload     %u bytes",
                     state->header.payload_size);
            fb_draw_string(panel_x + 20, info_y + 36, line,
                           theme->titlebar_text_inactive);

            snprintf(line, sizeof(line), "Decoded     %dx%d, %d bytes in memory",
                     state->loaded.width, state->loaded.height,
                     state->loaded.width * state->loaded.height * 4);
            fb_draw_string(panel_x + 20, info_y + 54, line,
                           theme->titlebar_text_inactive);

            int ratio = state->header.payload_size
                            ? (state->loaded.width * state->loaded.height * 4) /
                                  (int)state->header.payload_size
                            : 0;
            snprintf(line, sizeof(line), "Compression %dx smaller than raw",
                     MAX(ratio, 1));
            fb_draw_string(panel_x + 20, info_y + 72, line, theme->accent);
        }
    } else {
        const char *message = "Select an icon on the left";
        fb_draw_string(panel_x + panel_w / 2 - fb_text_width(message) / 2,
                       y + h / 2, message, theme->titlebar_text_inactive);
    }

    /* Status bar. */
    int status_y = y + h - 18;
    fb_fill_rect(x, status_y, w, 18, theme->titlebar_inactive);
    fb_draw_string_clipped(x + 8, status_y + 1, state->status,
                           theme->titlebar_text_inactive, w - 180);
    fb_draw_string(x + w - 170, status_y + 1, "+/- zoom   G grid",
                   theme->titlebar_text_inactive);
}

static bool_t imageview_on_key(struct window *win, uint32_t key,
                               uint32_t modifiers)
{
    struct imageview_state *state = (struct imageview_state *)win->app_state;
    UNUSED(modifiers);

    switch (key) {
    case KEY_UP:
        if (state->selected > 0) {
            state->selected--;
            load_selected(state);
        }
        return true;
    case KEY_DOWN:
        if (state->selected < state->file_count - 1) {
            state->selected++;
            load_selected(state);
        }
        return true;
    case '+':
    case '=':
        state->zoom = MIN(state->zoom * 2, 512);
        return true;
    case '-':
    case '_':
        state->zoom = MAX(state->zoom / 2, 32);
        return true;
    case 'g':
    case 'G':
        state->show_grid = !state->show_grid;
        return true;
    case KEY_F5:
        scan_icons(state);
        load_selected(state);
        return true;
    default:
        break;
    }
    return false;
}

static bool_t imageview_on_mouse(struct window *win, int x, int y,
                                 uint32_t buttons, input_event_type_t type)
{
    struct imageview_state *state = (struct imageview_state *)win->app_state;

    if (type != INPUT_MOUSE_DOWN || !(buttons & MOUSE_LEFT)) {
        return false;
    }
    if (x >= 170) {
        return false;
    }

    int index = (y - 4) / 18;
    if (index >= 0 && index < state->file_count) {
        state->selected = index;
        load_selected(state);
        return true;
    }
    return false;
}

static const struct app_ops imageview_ops = {
    .draw     = imageview_draw,
    .on_key   = imageview_on_key,
    .on_mouse = imageview_on_mouse,
    .on_open  = imageview_on_open,
    .on_close = imageview_on_close,
};

void app_imageview_register(void)
{
    desktop_register_app("Icon Viewer", "[]", &imageview_ops, 660, 500,
                         RGB(200, 160, 240), sizeof(struct imageview_state),
                         true);
}
