/*
 * Qira OS - Settings
 *
 * Graphical front end to the system configuration store. Boolean settings
 * toggle, integers step, and string settings cycle through their known
 * values. Changes take effect immediately and can be written to disk.
 */

#include <kernel/desktop.h>
#include <kernel/config.h>
#include <kernel/string.h>
#include <kernel/printf.h>
#include <kernel/log.h>
#include <kernel/audio.h>

#define ROW_HEIGHT 24

struct settings_state {
    int  selected;
    int  scroll;
    char status[128];
};

/* Known values for string settings, so they can be cycled. */
struct string_choices {
    const char *key;
    const char *values[6];
    int         count;
};

static const struct string_choices choices[] = {
    {"desktop.theme", {"aurora", "midnight", "light", "forest"}, 4},
    {"desktop.wallpaper", {"gradient", "solid", "stars"}, 3},
    {"desktop.panel_position", {"top", "bottom"}, 2},
    {"terminal.default_shell", {"ush", "qcsh"}, 2},
};

static const struct string_choices *find_choices(const char *key)
{
    for (size_t i = 0; i < ARRAY_SIZE(choices); i++) {
        if (strcmp(choices[i].key, key) == 0) {
            return &choices[i];
        }
    }
    return NULL;
}

/* Apply a setting that has an immediate side effect. */
static void apply_setting(const char *key)
{
    if (strcmp(key, "desktop.theme") == 0) {
        desktop_set_theme(config_get_string("desktop.theme", "aurora"));
    } else if (strcmp(key, "log.level") == 0) {
        log_set_level((log_level_t)config_get_int("log.level", 3));
    } else if (strcmp(key, "audio.enabled") == 0) {
        audio_set_enabled(config_get_bool("audio.enabled", true));
    }
    desktop_invalidate();
}

static void settings_on_open(struct window *win)
{
    struct settings_state *state = (struct settings_state *)win->app_state;
    strlcpy(state->status, "Enter or click to change  |  Ctrl+S to save",
            sizeof(state->status));
    window_set_title(win, "Settings");
}

static void settings_change(struct settings_state *state, int direction)
{
    const struct config_entry *entry = config_at(state->selected);
    if (!entry) {
        return;
    }

    char key[CONFIG_KEY_MAX];
    strlcpy(key, entry->key, sizeof(key));

    if (entry->type == CONFIG_BOOL) {
        bool_t value = config_get_bool(key, false);
        config_set_bool(key, !value);
        snprintf(state->status, sizeof(state->status), "%s = %s", key,
                 !value ? "on" : "off");
    } else if (entry->type == CONFIG_INT) {
        int value = config_get_int(key, 0);
        int step  = 1;

        /* Sensible step sizes for the settings that have them. */
        if (strstr(key, "_ms") || strstr(key, "scrollback")) {
            step = 10;
        }
        if (strstr(key, "speed")) {
            step = 5;
        }

        value += direction * step;
        if (strcmp(key, "log.level") == 0) {
            value = CLAMP(value, 0, 5);
        }
        if (value < 0) {
            value = 0;
        }
        config_set_int(key, value);
        snprintf(state->status, sizeof(state->status), "%s = %d", key, value);
    } else {
        const struct string_choices *options = find_choices(key);
        if (!options) {
            snprintf(state->status, sizeof(state->status),
                     "%s is a free-form value; edit it with 'qcsh config set'", key);
            return;
        }

        const char *current = config_get_string(key, options->values[0]);
        int index = 0;
        for (int i = 0; i < options->count; i++) {
            if (strcmp(options->values[i], current) == 0) {
                index = i;
                break;
            }
        }
        index = (index + direction + options->count) % options->count;
        config_set_string(key, options->values[index]);
        snprintf(state->status, sizeof(state->status), "%s = %s", key,
                 options->values[index]);
    }

    apply_setting(key);
}

static void settings_draw(struct window *win, int x, int y, int w, int h)
{
    struct settings_state *state = (struct settings_state *)win->app_state;
    const struct theme    *theme = desktop_theme();

    /* Header. */
    fb_fill_rect(x, y, w, 30, theme->titlebar_inactive);
    fb_draw_string(x + 12, y + 7, "System Settings", theme->accent);
    char count[48];
    snprintf(count, sizeof(count), "%d settings", config_count());
    fb_draw_string(x + w - fb_text_width(count) - 12, y + 7, count,
                   theme->titlebar_text_inactive);
    fb_fill_rect(x, y + 29, w, 1, theme->border);

    int list_y  = y + 34;
    int visible = (h - (list_y - y) - 24) / ROW_HEIGHT;

    if (state->selected < state->scroll) {
        state->scroll = state->selected;
    }
    if (state->selected >= state->scroll + visible) {
        state->scroll = state->selected - visible + 1;
    }

    char last_group[CONFIG_KEY_MAX] = "";

    for (int row = 0; row < visible; row++) {
        int index = state->scroll + row;
        const struct config_entry *entry = config_at(index);
        if (!entry) {
            break;
        }

        int ry = list_y + row * ROW_HEIGHT;
        bool_t selected = (index == state->selected);

        if (selected) {
            fb_fill_rect(x + 4, ry, w - 8, ROW_HEIGHT, theme->accent);
        }

        color_t label_color = selected ? RGB(255, 255, 255) : theme->window_text;
        color_t value_color = selected ? RGB(235, 240, 250) : theme->accent;

        /* Show the key without its group prefix for readability. */
        const char *dot  = strchr(entry->key, '.');
        const char *name = dot ? dot + 1 : entry->key;

        fb_draw_string_clipped(x + 16, ry + 4, name, label_color, 220);

        /* Render booleans as an on/off pill. */
        if (entry->type == CONFIG_BOOL) {
            bool_t on = config_get_bool(entry->key, false);
            int pill_x = x + 260;
            fb_fill_round_rect(pill_x, ry + 5, 40, 14, 7,
                               on ? RGB(90, 200, 130) : RGB(90, 96, 116));
            fb_fill_circle(on ? pill_x + 31 : pill_x + 9, ry + 12, 5,
                           RGB(250, 251, 255));
            fb_draw_string(pill_x + 50, ry + 4, on ? "on" : "off", value_color);
        } else {
            fb_draw_string_clipped(x + 260, ry + 4, entry->value, value_color,
                                   w - 280);
        }

        /* Description on the right, when there is room. */
        if (w > 620 && entry->description[0]) {
            fb_draw_string_clipped(x + 420, ry + 4, entry->description,
                                   selected ? RGB(220, 226, 240)
                                            : theme->titlebar_text_inactive,
                                   w - 436);
        }

        UNUSED(last_group);
    }

    /* Status bar. */
    int status_y = y + h - 22;
    fb_fill_rect(x, status_y, w, 22, theme->titlebar_inactive);
    fb_fill_rect(x, status_y, w, 1, theme->border);
    fb_draw_string_clipped(x + 8, status_y + 3, state->status,
                           theme->titlebar_text_inactive, w - 16);
}

static bool_t settings_on_key(struct window *win, uint32_t key, uint32_t modifiers)
{
    struct settings_state *state = (struct settings_state *)win->app_state;

    if (modifiers & MOD_CTRL) {
        if (key == 's' || key == 'S' || key == 19) {
            if (config_save() == 0) {
                strlcpy(state->status, "configuration saved to " CONFIG_PATH,
                        sizeof(state->status));
                desktop_notify("Settings", "Configuration saved");
            } else {
                strlcpy(state->status, "save failed", sizeof(state->status));
            }
            return true;
        }
        if (key == 'r' || key == 'R' || key == 18) {
            config_reset_defaults();
            desktop_set_theme(config_get_string("desktop.theme", "aurora"));
            strlcpy(state->status, "reset to defaults", sizeof(state->status));
            desktop_notify("Settings", "Settings reset to defaults");
            return true;
        }
    }

    switch (key) {
    case KEY_UP:
        state->selected = MAX(state->selected - 1, 0);
        return true;
    case KEY_DOWN:
        state->selected = MIN(state->selected + 1, config_count() - 1);
        return true;
    case KEY_PAGEUP:
        state->selected = MAX(state->selected - 8, 0);
        return true;
    case KEY_PAGEDOWN:
        state->selected = MIN(state->selected + 8, config_count() - 1);
        return true;
    case KEY_HOME:
        state->selected = 0;
        return true;
    case KEY_END:
        state->selected = config_count() - 1;
        return true;
    case KEY_ENTER:   /* == '\n' */
    case '\r':
    case ' ':
    case KEY_RIGHT:
        settings_change(state, 1);
        return true;
    case KEY_LEFT:
        settings_change(state, -1);
        return true;
    default:
        break;
    }
    return false;
}

static bool_t settings_on_mouse(struct window *win, int x, int y, uint32_t buttons,
                                input_event_type_t type)
{
    struct settings_state *state = (struct settings_state *)win->app_state;
    UNUSED(x);

    if (type != INPUT_MOUSE_DOWN || !(buttons & MOUSE_LEFT)) {
        return false;
    }
    if (y < 34) {
        return false;
    }

    int index = state->scroll + (y - 34) / ROW_HEIGHT;
    if (index < config_count()) {
        if (index == state->selected) {
            settings_change(state, 1);
        } else {
            state->selected = index;
        }
        return true;
    }
    return false;
}

static const struct app_ops settings_ops = {
    .draw     = settings_draw,
    .on_key   = settings_on_key,
    .on_mouse = settings_on_mouse,
    .on_open  = settings_on_open,
};

void app_settings_register(void)
{
    desktop_register_app("Settings", "*", &settings_ops, 700, 460,
                         RGB(180, 160, 250), sizeof(struct settings_state), true);
}
