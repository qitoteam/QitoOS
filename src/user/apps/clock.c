/*
 * QitoOS - Clock
 *
 * An analogue clock face with a digital readout, plus a stopwatch. Doubles as
 * a demonstration that the compositor can animate at a steady rate.
 */

#include <kernel/desktop.h>
#include <kernel/time.h>
#include <kernel/string.h>
#include <kernel/printf.h>

struct clock_state {
    bool_t   stopwatch_running;
    uint64_t stopwatch_start_ms;
    uint64_t stopwatch_elapsed_ms;
    int      mode;   /* 0 clock, 1 stopwatch */
};

static void clock_on_open(struct window *win)
{
    struct clock_state *state = (struct clock_state *)win->app_state;
    state->mode = 0;
    window_set_title(win, "Clock");
}

/*
 * Draw a hand from the centre at the given angle.
 * `angle` is in units of 1/1000 of a full turn, measured clockwise from 12.
 */
static void draw_hand(int cx, int cy, int length, int angle, int thickness,
                      color_t color)
{
    /* Fixed-point sine/cosine via a small quarter-turn table. */
    static const int sine_table[91] = {
        0,    17,   35,   52,   70,   87,   105,  122,  139,  156,  174,
        191,  208,  225,  242,  259,  276,  292,  309,  326,  342,  358,
        375,  391,  407,  423,  438,  454,  469,  485,  500,  515,  530,
        545,  559,  574,  588,  602,  616,  629,  643,  656,  669,  682,
        695,  707,  719,  731,  743,  755,  766,  777,  788,  799,  809,
        819,  829,  839,  848,  857,  866,  875,  883,  891,  899,  906,
        914,  921,  927,  934,  940,  946,  951,  956,  961,  966,  970,
        974,  978,  982,  985,  988,  990,  993,  995,  996,  998,  999,
        999,  1000, 1000,
    };

    int degrees = (angle * 360) / 1000;
    degrees = ((degrees % 360) + 360) % 360;

    /* sin and cos from the first-quadrant table. */
    int sin_value, cos_value;
    int quadrant = degrees / 90;
    int offset   = degrees % 90;

    switch (quadrant) {
    case 0: sin_value = sine_table[offset];       cos_value = sine_table[90 - offset]; break;
    case 1: sin_value = sine_table[90 - offset];  cos_value = -sine_table[offset];     break;
    case 2: sin_value = -sine_table[offset];      cos_value = -sine_table[90 - offset];break;
    default: sin_value = -sine_table[90 - offset]; cos_value = sine_table[offset];     break;
    }

    /* 12 o'clock is up, so x uses sin and y uses -cos. */
    int ex = cx + (length * sin_value) / 1000;
    int ey = cy - (length * cos_value) / 1000;

    for (int t = -thickness / 2; t <= thickness / 2; t++) {
        fb_draw_line(cx + t, cy, ex + t, ey, color);
        if (thickness > 1) {
            fb_draw_line(cx, cy + t, ex, ey + t, color);
        }
    }
}

static void clock_draw(struct window *win, int x, int y, int w, int h)
{
    struct clock_state *state = (struct clock_state *)win->app_state;
    const struct theme *theme = desktop_theme();

    /* Mode tabs. */
    fb_fill_rect(x, y, w, 24, theme->titlebar_inactive);
    static const char *modes[] = {"Clock", "Stopwatch"};
    for (int i = 0; i < 2; i++) {
        int tx = x + i * 100;
        if (state->mode == i) {
            fb_fill_rect(tx, y, 100, 24, theme->window_bg);
            fb_fill_rect(tx, y, 100, 2, theme->accent);
        }
        fb_draw_string(tx + 18, y + 4, modes[i],
                       state->mode == i ? theme->accent
                                        : theme->titlebar_text_inactive);
    }
    fb_fill_rect(x, y + 23, w, 1, theme->border);

    int area_y = y + 28;
    int area_h = h - 28;

    if (state->mode == 0) {
        struct qito_time now;
        time_from_unix(rtc_unix_time(), &now);

        int cx     = x + w / 2;
        int cy     = area_y + area_h / 2 - 16;
        int radius = MIN(w, area_h) / 2 - 34;
        if (radius < 30) {
            radius = 30;
        }

        /* Face. */
        fb_fill_circle(cx, cy, radius + 6, theme->input_bg);
        fb_draw_circle(cx, cy, radius + 6, theme->border);
        fb_draw_circle(cx, cy, radius + 5, theme->border);

        /* Hour ticks. */
        for (int i = 0; i < 12; i++) {
            int angle = i * 1000 / 12;
            int inner = radius - (i % 3 == 0 ? 12 : 6);
            draw_hand(cx, cy, radius, angle, 1, theme->window_bg);
            draw_hand(cx, cy, inner, angle, i % 3 == 0 ? 3 : 1,
                      i % 3 == 0 ? theme->accent : theme->titlebar_text_inactive);
        }
        fb_fill_circle(cx, cy, radius - 14, theme->input_bg);

        /* Hands. */
        int hour_angle   = ((now.hour % 12) * 1000 + now.minute * 1000 / 60) / 12;
        int minute_angle = (now.minute * 1000 + now.second * 1000 / 60) / 60;
        int second_angle = now.second * 1000 / 60;

        draw_hand(cx, cy, radius / 2, hour_angle, 4, theme->window_text);
        draw_hand(cx, cy, radius * 3 / 4, minute_angle, 3, theme->window_text);
        draw_hand(cx, cy, radius - 8, second_angle, 1, RGB(240, 110, 100));
        fb_fill_circle(cx, cy, 4, theme->accent);

        /* Digital readout. */
        char digital[32];
        snprintf(digital, sizeof(digital), "%02d:%02d:%02d", now.hour, now.minute,
                 now.second);
        fb_draw_string_large(cx - fb_text_width_large(digital) / 2,
                             y + h - 56, digital, theme->window_text);

        static const char *weekdays[] = {"Sunday",   "Monday", "Tuesday",
                                         "Wednesday", "Thursday", "Friday",
                                         "Saturday"};
        char date[64];
        snprintf(date, sizeof(date), "%s, %04d-%02d-%02d UTC",
                 weekdays[now.weekday % 7], now.year, now.month, now.day);
        fb_draw_string(cx - fb_text_width(date) / 2, y + h - 22, date,
                       theme->titlebar_text_inactive);
    } else {
        uint64_t elapsed = state->stopwatch_elapsed_ms;
        if (state->stopwatch_running) {
            elapsed += time_uptime_ms() - state->stopwatch_start_ms;
        }

        char readout[32];
        snprintf(readout, sizeof(readout), "%02llu:%02llu:%02llu.%llu",
                 (unsigned long long)(elapsed / 3600000),
                 (unsigned long long)((elapsed % 3600000) / 60000),
                 (unsigned long long)((elapsed % 60000) / 1000),
                 (unsigned long long)((elapsed % 1000) / 100));

        fb_draw_string_large(x + w / 2 - fb_text_width_large(readout) / 2,
                             area_y + area_h / 2 - 40, readout,
                             state->stopwatch_running ? RGB(140, 230, 150)
                                                      : theme->window_text);

        const char *hint = state->stopwatch_running ? "Space to stop"
                                                    : "Space to start, R to reset";
        fb_draw_string(x + w / 2 - fb_text_width(hint) / 2, area_y + area_h / 2 + 20,
                       hint, theme->titlebar_text_inactive);

        /* Uptime for reference. */
        uint64_t seconds = time_uptime_ms() / 1000;
        char uptime[64];
        snprintf(uptime, sizeof(uptime), "System uptime %llu:%02llu:%02llu",
                 (unsigned long long)(seconds / 3600),
                 (unsigned long long)((seconds % 3600) / 60),
                 (unsigned long long)(seconds % 60));
        fb_draw_string(x + w / 2 - fb_text_width(uptime) / 2, y + h - 26, uptime,
                       theme->titlebar_text_inactive);
    }
}

static bool_t clock_on_key(struct window *win, uint32_t key, uint32_t modifiers)
{
    struct clock_state *state = (struct clock_state *)win->app_state;
    UNUSED(modifiers);

    if (key == KEY_TAB) {
        state->mode = (state->mode + 1) % 2;
        return true;
    }
    if (state->mode == 1) {
        if (key == ' ') {
            if (state->stopwatch_running) {
                state->stopwatch_elapsed_ms +=
                    time_uptime_ms() - state->stopwatch_start_ms;
                state->stopwatch_running = false;
            } else {
                state->stopwatch_start_ms = time_uptime_ms();
                state->stopwatch_running  = true;
            }
            return true;
        }
        if (key == 'r' || key == 'R') {
            state->stopwatch_elapsed_ms = 0;
            state->stopwatch_start_ms   = time_uptime_ms();
            return true;
        }
    }
    return false;
}

static bool_t clock_on_mouse(struct window *win, int x, int y, uint32_t buttons,
                             input_event_type_t type)
{
    struct clock_state *state = (struct clock_state *)win->app_state;

    if (type == INPUT_MOUSE_DOWN && (buttons & MOUSE_LEFT) && y < 24) {
        int tab = x / 100;
        if (tab >= 0 && tab < 2) {
            state->mode = tab;
            return true;
        }
    }
    return false;
}

/* Redraw every tick so the second hand moves. */
static bool_t clock_tick(struct window *win)
{
    UNUSED(win);
    return true;
}

static const struct app_ops clock_ops = {
    .draw     = clock_draw,
    .on_key   = clock_on_key,
    .on_mouse = clock_on_mouse,
    .tick     = clock_tick,
    .on_open  = clock_on_open,
};

void app_clock_register(void)
{
    desktop_register_app("Clock", "()", &clock_ops, 360, 400, RGB(150, 220, 250),
                         sizeof(struct clock_state), true);
}
