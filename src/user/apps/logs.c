/*
 * QitoOS - Log Viewer
 *
 * Reads the kernel log ring buffer, colours entries by severity, and supports
 * scrolling, filtering and following new output as it arrives.
 */

#include <kernel/desktop.h>
#include <kernel/log.h>
#include <kernel/string.h>
#include <kernel/printf.h>
#include <kernel/mm.h>
#include <kernel/time.h>

#define LOG_MAX_LINES 600
#define LOG_LINE_MAX  200
#define ROW_HEIGHT    FONT_HEIGHT

struct logs_state {
    char   lines[LOG_MAX_LINES][LOG_LINE_MAX];
    int    line_count;
    int    scroll;
    bool_t follow;
    char   filter[48];
    bool_t editing_filter;
    uint64_t last_refresh_ms;
    size_t last_log_size;
};

static void logs_refresh(struct logs_state *state)
{
    size_t total = log_size();
    char  *buffer = kmalloc(total + 1);
    if (!buffer) {
        return;
    }

    size_t got = log_read(buffer, total, 0);
    buffer[got] = '\0';

    state->line_count = 0;

    const char *p = buffer;
    while (*p && state->line_count < LOG_MAX_LINES) {
        const char *end = strchr(p, '\n');
        size_t      len = end ? (size_t)(end - p) : strlen(p);
        len = MIN(len, (size_t)LOG_LINE_MAX - 1);

        char line[LOG_LINE_MAX];
        memcpy(line, p, len);
        line[len] = '\0';

        /* Apply the filter, if any. */
        if (!state->filter[0] || strstr(line, state->filter)) {
            strlcpy(state->lines[state->line_count++], line, LOG_LINE_MAX);
        }

        if (!end) {
            break;
        }
        p = end + 1;
    }

    kfree(buffer);
    state->last_log_size = total;
}

static void logs_on_open(struct window *win)
{
    struct logs_state *state = (struct logs_state *)win->app_state;
    state->follow = true;
    logs_refresh(state);
    window_set_title(win, "Kernel Log");
}

/* Pick a colour based on the severity token in the line. */
static color_t line_color(const char *line, const struct theme *theme)
{
    if (strstr(line, " ERROR ") || strstr(line, "PANIC")) {
        return RGB(255, 120, 110);
    }
    if (strstr(line, " WARN ")) {
        return RGB(245, 200, 100);
    }
    if (strstr(line, " DEBUG ")) {
        return RGB(140, 150, 175);
    }
    if (strstr(line, " TRACE ")) {
        return RGB(120, 128, 150);
    }
    return theme->window_text;
}

static void logs_draw(struct window *win, int x, int y, int w, int h)
{
    struct logs_state  *state = (struct logs_state *)win->app_state;
    const struct theme *theme = desktop_theme();

    /* Toolbar. */
    fb_fill_rect(x, y, w, 26, theme->titlebar_inactive);

    char header[96];
    snprintf(header, sizeof(header), "%d lines   %s", state->line_count,
             state->follow ? "following" : "paused");
    fb_draw_string(x + 8, y + 5, header,
                   state->follow ? RGB(140, 220, 150)
                                 : theme->titlebar_text_inactive);

    /* Filter box. */
    int filter_x = x + 220;
    int filter_w = MIN(240, w - filter_x + x - 16);
    fb_fill_rect(filter_x, y + 3, filter_w, 20,
                 state->editing_filter ? theme->input_bg : theme->window_bg);
    fb_draw_rect(filter_x, y + 3, filter_w, 20,
                 state->editing_filter ? theme->accent : theme->border);

    if (state->filter[0]) {
        fb_draw_string_clipped(filter_x + 6, y + 5, state->filter,
                               theme->window_text, filter_w - 12);
    } else {
        fb_draw_string(filter_x + 6, y + 5, "filter (press /)",
                       theme->titlebar_text_inactive);
    }
    if (state->editing_filter) {
        fb_fill_rect(filter_x + 6 + fb_text_width(state->filter), y + 5, 2,
                     FONT_HEIGHT, theme->accent);
    }

    fb_fill_rect(x, y + 25, w, 1, theme->border);

    /* Log lines. */
    int text_y  = y + 28;
    int visible = (h - (text_y - y) - 20) / ROW_HEIGHT;

    if (state->follow) {
        state->scroll = MAX(state->line_count - visible, 0);
    }
    state->scroll = CLAMP(state->scroll, 0, MAX(state->line_count - visible, 0));

    int max_chars = (w - 12) / FONT_WIDTH;

    for (int row = 0; row < visible; row++) {
        int index = state->scroll + row;
        if (index >= state->line_count) {
            break;
        }
        fb_draw_string_clipped(x + 6, text_y + row * ROW_HEIGHT,
                               state->lines[index],
                               line_color(state->lines[index], theme),
                               max_chars * FONT_WIDTH);
    }

    /* Scrollbar. */
    if (state->line_count > visible) {
        int track_h = h - (text_y - y) - 20;
        int thumb_h = MAX(track_h * visible / state->line_count, 20);
        int thumb_y = text_y + (track_h - thumb_h) * state->scroll /
                                  MAX(state->line_count - visible, 1);
        fb_fill_rect(x + w - 6, text_y, 4, track_h, theme->input_bg);
        fb_fill_round_rect(x + w - 6, thumb_y, 4, thumb_h, 2, theme->accent);
    }

    /* Status bar. */
    int status_y = y + h - 18;
    fb_fill_rect(x, status_y, w, 18, theme->titlebar_inactive);
    fb_draw_string(x + 8, status_y + 1,
                   "F follow  / filter  C clear  PgUp/PgDn scroll",
                   theme->titlebar_text_inactive);

    struct log_stats stats;
    log_get_stats(&stats);
    char counts[64];
    snprintf(counts, sizeof(counts), "%llu records  %llu errors",
             (unsigned long long)stats.records,
             (unsigned long long)stats.counts[LOG_ERROR]);
    fb_draw_string(x + w - fb_text_width(counts) - 8, status_y + 1, counts,
                   theme->titlebar_text_inactive);
}

static bool_t logs_on_key(struct window *win, uint32_t key, uint32_t modifiers)
{
    struct logs_state *state = (struct logs_state *)win->app_state;
    UNUSED(modifiers);

    /* Filter editing captures most keys. */
    if (state->editing_filter) {
        if (key == KEY_ENTER || key == '\n' || key == '\r' || key == KEY_ESCAPE) {
            state->editing_filter = false;
            logs_refresh(state);
            return true;
        }
        if (key == KEY_BACKSPACE || key == '\b' || key == 127) {
            size_t len = strlen(state->filter);
            if (len > 0) {
                state->filter[len - 1] = '\0';
                logs_refresh(state);
            }
            return true;
        }
        if (key >= 32 && key < 127) {
            size_t len = strlen(state->filter);
            if (len + 1 < sizeof(state->filter)) {
                state->filter[len]     = (char)key;
                state->filter[len + 1] = '\0';
                logs_refresh(state);
            }
            return true;
        }
        return true;
    }

    switch (key) {
    case '/':
        state->editing_filter = true;
        return true;
    case 'f':
    case 'F':
        state->follow = !state->follow;
        return true;
    case 'c':
    case 'C':
        log_clear();
        logs_refresh(state);
        desktop_notify("Kernel Log", "Log cleared");
        return true;
    case KEY_ESCAPE:
        if (state->filter[0]) {
            state->filter[0] = '\0';
            logs_refresh(state);
        }
        return true;
    case KEY_UP:
        state->follow = false;
        state->scroll = MAX(state->scroll - 1, 0);
        return true;
    case KEY_DOWN:
        state->scroll++;
        return true;
    case KEY_PAGEUP:
        state->follow = false;
        state->scroll = MAX(state->scroll - 15, 0);
        return true;
    case KEY_PAGEDOWN:
        state->scroll += 15;
        return true;
    case KEY_HOME:
        state->follow = false;
        state->scroll = 0;
        return true;
    case KEY_END:
        state->follow = true;
        return true;
    default:
        break;
    }
    return false;
}

static bool_t logs_tick(struct window *win)
{
    struct logs_state *state = (struct logs_state *)win->app_state;
    uint64_t now = time_uptime_ms();

    if (now - state->last_refresh_ms >= 700) {
        state->last_refresh_ms = now;
        if (log_size() != state->last_log_size) {
            logs_refresh(state);
            return true;
        }
    }
    return false;
}

static const struct app_ops logs_ops = {
    .draw    = logs_draw,
    .on_key  = logs_on_key,
    .tick    = logs_tick,
    .on_open = logs_on_open,
};

void app_logs_register(void)
{
    desktop_register_app("Kernel Log", "Lg", &logs_ops, 760, 460,
                         RGB(130, 210, 220), sizeof(struct logs_state), true);
}
