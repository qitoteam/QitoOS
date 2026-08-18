/*
 * Qira OS - Paint
 *
 * A bitmap editor that works at icon scale and saves in the system's own QAC
 * format. Drawing on a 32x32 grid with a palette is exactly the workflow the
 * icon format was designed around, so this doubles as the authoring tool for
 * application icons.
 */

#include <kernel/desktop.h>
#include <kernel/qac.h>
#include <kernel/fs.h>
#include <kernel/string.h>
#include <kernel/printf.h>
#include <kernel/mm.h>

#define CANVAS_SIZE   32
#define PALETTE_COUNT 16
#define UNDO_DEPTH    16

typedef enum {
    TOOL_PENCIL = 0,
    TOOL_FILL,
    TOOL_ERASER,
    TOOL_PICKER,
    TOOL_LINE,
    TOOL_RECT,
} tool_t;

struct paint_state {
    uint32_t canvas[CANVAS_SIZE * CANVAS_SIZE];
    uint32_t undo[UNDO_DEPTH][CANVAS_SIZE * CANVAS_SIZE];
    int      undo_count;
    int      undo_position;

    int    colour_index;
    tool_t tool;
    int    scale;
    bool_t show_grid;
    bool_t dirty;

    /* Anchor for the line and rectangle tools. */
    int    anchor_x, anchor_y;
    bool_t has_anchor;

    char   path[FS_PATH_MAX];
    char   status[160];
};

static const uint32_t palette[PALETTE_COUNT] = {
    0x00000000,   /* transparent  */
    0xFF1A1D28,   /* near black   */
    0xFF3A4152,   /* dark grey    */
    0xFF8A93A8,   /* grey         */
    0xFFF2F5FA,   /* white        */
    0xFFE8635F,   /* red          */
    0xFFF08A3C,   /* orange       */
    0xFFF0C24A,   /* amber        */
    0xFF78D278,   /* green        */
    0xFF3E9B4F,   /* dark green   */
    0xFF56D6D6,   /* cyan         */
    0xFF5AA0F0,   /* blue         */
    0xFF2E6FC0,   /* dark blue    */
    0xFFB07BE8,   /* purple       */
    0xFFE87BC0,   /* pink         */
    0xFF8B5A2B,   /* brown        */
};

static const char *tool_names[] = {"pencil", "fill", "eraser",
                                   "picker", "line", "rect"};

static void push_undo(struct paint_state *state)
{
    int slot = state->undo_position % UNDO_DEPTH;
    memcpy(state->undo[slot], state->canvas, sizeof(state->canvas));
    state->undo_position++;
    if (state->undo_count < UNDO_DEPTH) {
        state->undo_count++;
    }
}

static void undo(struct paint_state *state)
{
    if (state->undo_count == 0) {
        strlcpy(state->status, "nothing to undo", sizeof(state->status));
        return;
    }
    state->undo_position--;
    state->undo_count--;

    int slot = state->undo_position % UNDO_DEPTH;
    memcpy(state->canvas, state->undo[slot], sizeof(state->canvas));
    strlcpy(state->status, "undone", sizeof(state->status));
}

/* Flood fill from a point, replacing the colour that was there. */
static void flood_fill(struct paint_state *state, int x, int y, uint32_t colour)
{
    uint32_t target = state->canvas[y * CANVAS_SIZE + x];
    if (target == colour) {
        return;
    }

    /* An explicit stack: recursion on a kernel stack is a bad idea. */
    static int16_t stack[CANVAS_SIZE * CANVAS_SIZE * 2];
    int depth = 0;

    stack[depth++] = (int16_t)x;
    stack[depth++] = (int16_t)y;

    while (depth >= 2) {
        int py = stack[--depth];
        int px = stack[--depth];

        if (px < 0 || px >= CANVAS_SIZE || py < 0 || py >= CANVAS_SIZE) {
            continue;
        }
        if (state->canvas[py * CANVAS_SIZE + px] != target) {
            continue;
        }

        state->canvas[py * CANVAS_SIZE + px] = colour;

        if (depth + 8 < (int)ARRAY_SIZE(stack)) {
            stack[depth++] = (int16_t)(px + 1);
            stack[depth++] = (int16_t)py;
            stack[depth++] = (int16_t)(px - 1);
            stack[depth++] = (int16_t)py;
            stack[depth++] = (int16_t)px;
            stack[depth++] = (int16_t)(py + 1);
            stack[depth++] = (int16_t)px;
            stack[depth++] = (int16_t)(py - 1);
        }
    }
}

static void draw_line_on_canvas(struct paint_state *state, int x0, int y0,
                                int x1, int y1, uint32_t colour)
{
    /* Bresenham, so diagonals are solid. */
    int dx = (x1 > x0) ? x1 - x0 : x0 - x1;
    int dy = (y1 > y0) ? y1 - y0 : y0 - y1;
    int sx = (x0 < x1) ? 1 : -1;
    int sy = (y0 < y1) ? 1 : -1;
    int error = dx - dy;

    for (;;) {
        if (x0 >= 0 && x0 < CANVAS_SIZE && y0 >= 0 && y0 < CANVAS_SIZE) {
            state->canvas[y0 * CANVAS_SIZE + x0] = colour;
        }
        if (x0 == x1 && y0 == y1) {
            break;
        }
        int doubled = error * 2;
        if (doubled > -dy) {
            error -= dy;
            x0 += sx;
        }
        if (doubled < dx) {
            error += dx;
            y0 += sy;
        }
    }
}

/* Save the canvas as a QAC file with three frames. */
static int save_qac(struct paint_state *state)
{
    /*
     * Encode with run-length, which is what the kernel decoder reads and is a
     * good fit for flat-shaded icon artwork.
     */
    #define QAC_HEADER_SIZE 32
    #define QAC_ENTRY_SIZE  16

    static uint8_t file[16384];
    size_t position = 0;

    struct sizes { int size; } frames[] = {{32}, {24}, {16}};
    int frame_count = 3;

    /* Reserve the header and entry table. */
    size_t entries_at = QAC_HEADER_SIZE;
    position = QAC_HEADER_SIZE + (size_t)frame_count * QAC_ENTRY_SIZE;
    size_t payload_at = position;

    struct { uint32_t offset, size; } recorded[3];

    for (int f = 0; f < frame_count; f++) {
        int size = frames[f].size;
        recorded[f].offset = (uint32_t)(position - payload_at);

        /* Scale down with nearest-neighbour, then run-length encode. */
        uint32_t previous = 0;
        int      run      = 0;
        bool_t   started  = false;

        for (int i = 0; i < size * size; i++) {
            int sx = (i % size) * CANVAS_SIZE / size;
            int sy = (i / size) * CANVAS_SIZE / size;
            uint32_t pixel = state->canvas[sy * CANVAS_SIZE + sx];

            if (!started) {
                previous = pixel;
                run      = 1;
                started  = true;
            } else if (pixel == previous && run < 255) {
                run++;
            } else {
                if (position + 5 > sizeof(file)) {
                    return -1;
                }
                file[position++] = (uint8_t)run;
                file[position++] = (uint8_t)(previous & 0xFF);
                file[position++] = (uint8_t)((previous >> 8) & 0xFF);
                file[position++] = (uint8_t)((previous >> 16) & 0xFF);
                file[position++] = (uint8_t)(previous >> 24);
                previous = pixel;
                run      = 1;
            }
        }
        if (started && position + 5 <= sizeof(file)) {
            file[position++] = (uint8_t)run;
            file[position++] = (uint8_t)(previous & 0xFF);
            file[position++] = (uint8_t)((previous >> 8) & 0xFF);
            file[position++] = (uint8_t)((previous >> 16) & 0xFF);
            file[position++] = (uint8_t)(previous >> 24);
        }

        recorded[f].size = (uint32_t)(position - payload_at - recorded[f].offset);

        /* The entry: width, height, encoding=RLE, no palette. */
        size_t at = entries_at + (size_t)f * QAC_ENTRY_SIZE;
        file[at + 0] = (uint8_t)(size & 0xFF);
        file[at + 1] = (uint8_t)(size >> 8);
        file[at + 2] = (uint8_t)(size & 0xFF);
        file[at + 3] = (uint8_t)(size >> 8);
        file[at + 4] = 1;   /* QAC_RLE */
        file[at + 5] = 0;
        file[at + 6] = 0;
        file[at + 7] = 0;
        memcpy(file + at + 8, &recorded[f].offset, 4);
        memcpy(file + at + 12, &recorded[f].size, 4);
    }

    size_t payload_size = position - payload_at;

    uint32_t checksum = 0;
    for (size_t i = payload_at; i < position; i++) {
        checksum += file[i];
    }

    memcpy(file + 0, "QACI", 4);
    uint16_t version = 1, count = (uint16_t)frame_count;
    memcpy(file + 4, &version, 2);
    memcpy(file + 6, &count, 2);
    uint32_t payload32 = (uint32_t)payload_size;
    memcpy(file + 8, &payload32, 4);
    memcpy(file + 12, &checksum, 4);
    uint32_t flags = 0;
    memcpy(file + 16, &flags, 4);
    memset(file + 20, 0, 12);
    strlcpy((char *)file + 20, "paint", 12);

    return fs_write_file(state->path, file, position);
}

static void paint_on_open(struct window *win)
{
    struct paint_state *state = (struct paint_state *)win->app_state;

    for (int i = 0; i < CANVAS_SIZE * CANVAS_SIZE; i++) {
        state->canvas[i] = 0;   /* transparent */
    }
    state->colour_index = 1;
    state->tool         = TOOL_PENCIL;
    state->scale        = 12;
    state->show_grid    = true;
    strlcpy(state->path, "/home/user/icon.qac", sizeof(state->path));
    strlcpy(state->status, "Ctrl+S saves as a .qac icon",
            sizeof(state->status));

    window_set_title(win, "Paint");
}

static void paint_draw(struct window *win, int x, int y, int w, int h)
{
    struct paint_state *state = (struct paint_state *)win->app_state;
    const struct theme *theme = desktop_theme();

    /* Toolbar. */
    fb_fill_rect(x, y, w, 26, theme->titlebar_inactive);
    int tx = x + 8;
    for (int i = 0; i < (int)ARRAY_SIZE(tool_names); i++) {
        int width = fb_text_width(tool_names[i]) + 14;
        bool_t active = (state->tool == (tool_t)i);

        if (active) {
            fb_fill_round_rect(tx, y + 3, width, 20, 3, theme->accent);
        }
        fb_draw_string(tx + 7, y + 5, tool_names[i],
                       active ? RGB(255, 255, 255) : theme->window_text);
        tx += width + 4;
    }
    fb_fill_rect(x, y + 25, w, 1, theme->border);

    /* Palette down the left. */
    int swatch = 24;
    int palette_x = x + 8;
    int palette_y = y + 34;

    for (int i = 0; i < PALETTE_COUNT; i++) {
        int row = i / 2;
        int col = i % 2;
        int sx  = palette_x + col * (swatch + 4);
        int sy  = palette_y + row * (swatch + 4);

        if (palette[i] == 0) {
            /* Transparent: a chequer, so it is distinguishable from black. */
            fb_fill_rect(sx, sy, swatch, swatch, RGB(58, 62, 78));
            fb_fill_rect(sx, sy, swatch / 2, swatch / 2, RGB(44, 48, 62));
            fb_fill_rect(sx + swatch / 2, sy + swatch / 2, swatch / 2,
                         swatch / 2, RGB(44, 48, 62));
        } else {
            fb_fill_rect(sx, sy, swatch, swatch, palette[i] & 0x00FFFFFF);
        }

        fb_draw_rect(sx, sy, swatch, swatch,
                     i == state->colour_index ? RGB(255, 255, 255)
                                              : theme->border);
        if (i == state->colour_index) {
            fb_draw_rect(sx - 1, sy - 1, swatch + 2, swatch + 2, theme->accent);
        }
    }

    /* Canvas. */
    int canvas_x = palette_x + 2 * (swatch + 4) + 16;
    int canvas_y = y + 34;
    int scale    = MIN(state->scale,
                       MIN((w - (canvas_x - x) - 20) / CANVAS_SIZE,
                           (h - 60) / CANVAS_SIZE));
    if (scale < 1) {
        scale = 1;
    }
    int canvas_px = CANVAS_SIZE * scale;

    /* Chequerboard behind the artwork. */
    for (int row = 0; row < CANVAS_SIZE; row++) {
        for (int col = 0; col < CANVAS_SIZE; col++) {
            bool_t light = ((row / 2) + (col / 2)) % 2 == 0;
            fb_fill_rect(canvas_x + col * scale, canvas_y + row * scale, scale,
                         scale, light ? RGB(58, 62, 78) : RGB(44, 48, 62));

            uint32_t pixel = state->canvas[row * CANVAS_SIZE + col];
            if (pixel >> 24) {
                fb_fill_rect(canvas_x + col * scale, canvas_y + row * scale,
                             scale, scale, pixel & 0x00FFFFFF);
            }
        }
    }

    if (state->show_grid && scale >= 6) {
        for (int i = 0; i <= CANVAS_SIZE; i++) {
            fb_fill_rect_alpha(canvas_x + i * scale, canvas_y, 1, canvas_px,
                               RGB(255, 255, 255), 25);
            fb_fill_rect_alpha(canvas_x, canvas_y + i * scale, canvas_px, 1,
                               RGB(255, 255, 255), 25);
        }
    }
    fb_draw_rect(canvas_x - 1, canvas_y - 1, canvas_px + 2, canvas_px + 2,
                 theme->border);

    /* A live preview at real size, beside the canvas. */
    int preview_x = canvas_x + canvas_px + 20;
    if (preview_x + 40 < x + w) {
        fb_draw_string(preview_x, canvas_y, "actual size",
                       theme->titlebar_text_inactive);
        for (int row = 0; row < CANVAS_SIZE; row++) {
            for (int col = 0; col < CANVAS_SIZE; col++) {
                uint32_t pixel = state->canvas[row * CANVAS_SIZE + col];
                if (pixel >> 24) {
                    fb_put_pixel(preview_x + col, canvas_y + 22 + row,
                                 pixel & 0x00FFFFFF);
                }
            }
        }
        fb_draw_rect(preview_x - 1, canvas_y + 21, CANVAS_SIZE + 2,
                     CANVAS_SIZE + 2, theme->border);
    }

    /* Status bar. */
    int status_y = y + h - 18;
    fb_fill_rect(x, status_y, w, 18, theme->titlebar_inactive);

    char left[200];
    snprintf(left, sizeof(left), "%s%s   %s", state->path,
             state->dirty ? " *" : "", state->status);
    fb_draw_string_clipped(x + 8, status_y + 1, left,
                           theme->titlebar_text_inactive, w - 200);
    fb_draw_string(x + w - 190, status_y + 1, "Ctrl+S save  Ctrl+Z undo",
                   theme->titlebar_text_inactive);
}

static bool_t paint_on_key(struct window *win, uint32_t key, uint32_t modifiers)
{
    struct paint_state *state = (struct paint_state *)win->app_state;

    if (modifiers & MOD_CTRL) {
        if (key == 's' || key == 'S' || key == 19) {
            if (save_qac(state) == 0) {
                state->dirty = false;
                snprintf(state->status, sizeof(state->status), "saved to %s",
                         state->path);
                desktop_notify("Paint", "Icon saved");
            } else {
                strlcpy(state->status, "save failed", sizeof(state->status));
            }
            return true;
        }
        if (key == 'z' || key == 'Z' || key == 26) {
            undo(state);
            return true;
        }
        if (key == 'n' || key == 'N' || key == 14) {
            push_undo(state);
            memset(state->canvas, 0, sizeof(state->canvas));
            strlcpy(state->status, "cleared", sizeof(state->status));
            state->dirty = true;
            return true;
        }
    }

    if (key >= '1' && key <= '9') {
        state->colour_index = key - '1';
        return true;
    }

    switch (key) {
    case 'p': case 'P': state->tool = TOOL_PENCIL; return true;
    case 'f': case 'F': state->tool = TOOL_FILL;   return true;
    case 'e': case 'E': state->tool = TOOL_ERASER; return true;
    case 'k': case 'K': state->tool = TOOL_PICKER; return true;
    case 'l': case 'L': state->tool = TOOL_LINE;   return true;
    case 'r': case 'R': state->tool = TOOL_RECT;   return true;
    case 'g': case 'G': state->show_grid = !state->show_grid; return true;
    case '+': case '=': state->scale = MIN(state->scale + 2, 20); return true;
    case '-': case '_': state->scale = MAX(state->scale - 2, 4);  return true;
    default: break;
    }
    return false;
}

static bool_t paint_on_mouse(struct window *win, int x, int y, uint32_t buttons,
                             input_event_type_t type)
{
    struct paint_state *state = (struct paint_state *)win->app_state;

    struct rect client;
    window_client_rect(win, &client);

    /* Toolbar. */
    if (y < 26 && type == INPUT_MOUSE_DOWN) {
        int tx = 8;
        for (int i = 0; i < (int)ARRAY_SIZE(tool_names); i++) {
            int width = fb_text_width(tool_names[i]) + 14;
            if (x >= tx && x < tx + width) {
                state->tool       = (tool_t)i;
                state->has_anchor = false;
                return true;
            }
            tx += width + 4;
        }
        return false;
    }

    /* Palette. */
    int swatch = 24;
    if (x >= 8 && x < 8 + 2 * (swatch + 4) && y >= 34) {
        if (type != INPUT_MOUSE_DOWN) {
            return false;
        }
        int col = (x - 8) / (swatch + 4);
        int row = (y - 34) / (swatch + 4);
        int index = row * 2 + col;
        if (index >= 0 && index < PALETTE_COUNT) {
            state->colour_index = index;
            return true;
        }
        return false;
    }

    /* Canvas. */
    int canvas_x = 8 + 2 * (swatch + 4) + 16;
    int canvas_y = 34;
    int scale    = MIN(state->scale,
                       MIN((client.w - canvas_x - 20) / CANVAS_SIZE,
                           (client.h - 60) / CANVAS_SIZE));
    if (scale < 1) {
        scale = 1;
    }

    int px = (x - canvas_x) / scale;
    int py = (y - canvas_y) / scale;

    if (px < 0 || px >= CANVAS_SIZE || py < 0 || py >= CANVAS_SIZE) {
        return false;
    }

    bool_t pressed = (type == INPUT_MOUSE_DOWN);
    bool_t held    = (buttons & MOUSE_LEFT) != 0;

    if (!pressed && !held) {
        return false;
    }

    uint32_t colour = palette[state->colour_index];

    switch (state->tool) {
    case TOOL_PENCIL:
        if (pressed) {
            push_undo(state);
        }
        state->canvas[py * CANVAS_SIZE + px] = colour;
        state->dirty = true;
        return true;

    case TOOL_ERASER:
        if (pressed) {
            push_undo(state);
        }
        state->canvas[py * CANVAS_SIZE + px] = 0;
        state->dirty = true;
        return true;

    case TOOL_FILL:
        if (pressed) {
            push_undo(state);
            flood_fill(state, px, py, colour);
            state->dirty = true;
        }
        return true;

    case TOOL_PICKER:
        if (pressed) {
            uint32_t picked = state->canvas[py * CANVAS_SIZE + px];
            for (int i = 0; i < PALETTE_COUNT; i++) {
                if (palette[i] == picked) {
                    state->colour_index = i;
                    break;
                }
            }
        }
        return true;

    case TOOL_LINE:
    case TOOL_RECT:
        if (pressed) {
            if (!state->has_anchor) {
                state->anchor_x   = px;
                state->anchor_y   = py;
                state->has_anchor = true;
                snprintf(state->status, sizeof(state->status),
                         "anchored at %d,%d; click again to finish", px, py);
            } else {
                push_undo(state);
                if (state->tool == TOOL_LINE) {
                    draw_line_on_canvas(state, state->anchor_x, state->anchor_y,
                                        px, py, colour);
                } else {
                    int x0 = MIN(state->anchor_x, px);
                    int x1 = MAX(state->anchor_x, px);
                    int y0 = MIN(state->anchor_y, py);
                    int y1 = MAX(state->anchor_y, py);
                    draw_line_on_canvas(state, x0, y0, x1, y0, colour);
                    draw_line_on_canvas(state, x0, y1, x1, y1, colour);
                    draw_line_on_canvas(state, x0, y0, x0, y1, colour);
                    draw_line_on_canvas(state, x1, y0, x1, y1, colour);
                }
                state->has_anchor = false;
                state->dirty      = true;
                strlcpy(state->status, "done", sizeof(state->status));
            }
        }
        return true;
    }

    return false;
}

static const struct app_ops paint_ops = {
    .draw     = paint_draw,
    .on_key   = paint_on_key,
    .on_mouse = paint_on_mouse,
    .on_open  = paint_on_open,
};

void app_paint_register(void)
{
    desktop_register_app("Paint", "/\\", &paint_ops, 720, 480,
                         RGB(240, 140, 180), sizeof(struct paint_state), true);
}
