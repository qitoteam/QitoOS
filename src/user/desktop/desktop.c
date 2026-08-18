/*
 * Qira OS - desktop environment
 *
 * Owns the framebuffer after boot. Each frame it composites the wallpaper,
 * the window stack (back to front), the panel, any open menu, notifications
 * and the mouse cursor, then flushes only the region that changed.
 *
 * Input events are routed to the focused window, except when the pointer is
 * interacting with window decorations, the panel or a menu.
 */

#include <kernel/desktop.h>
#include <kernel/fb.h>
#include <kernel/input.h>
#include <kernel/string.h>
#include <kernel/printf.h>
#include <kernel/mm.h>
#include <kernel/log.h>
#include <kernel/time.h>
#include <kernel/sched.h>
#include <kernel/config.h>
#include <kernel/version.h>
#include <kernel/audio.h>
#include <kernel/console.h>
#include <kernel/shell.h>
#include <kernel/qac.h>
#include <kernel/random.h>
#include <kernel/clipboard.h>

/* --- state ------------------------------------------------------------ */

static struct application applications[MAX_APPS];
static int                application_count;

static struct window  windows[MAX_WINDOWS];
static int            window_total;
static int            next_window_id = 1;
static struct window *focused_window;

static struct theme theme;
static bool_t       full_redraw = true;
static uint64_t     frame_count;

/* Drag and resize state. */
static struct window *drag_window;
static int            drag_offset_x, drag_offset_y;
static bool_t         resizing;

/* Menu state. */
static bool_t menu_open;
static int    menu_hover = -1;
static int    menu_x, menu_y, menu_w, menu_h;

/* Notifications. */
struct notification {
    char     title[48];
    char     message[96];
    uint64_t expires_ms;
    bool_t   active;
};
static struct notification notifications[MAX_NOTIFICATIONS];

/* Pointer. */
static int      cursor_x, cursor_y;
static int      prev_cursor_x, prev_cursor_y;
static uint32_t cursor_buttons;
static uint32_t previous_buttons;

/* Panel layout, recomputed each frame. */
static int panel_y;

/* --- themes ----------------------------------------------------------- */

void desktop_set_theme(const char *name)
{
    if (strcmp(name, "midnight") == 0) {
        theme.desktop_top       = RGB(8, 10, 20);
        theme.desktop_bottom    = RGB(20, 16, 34);
        theme.panel             = RGB(14, 16, 26);
        theme.window_bg         = RGB(22, 24, 34);
        theme.titlebar_active   = RGB(34, 38, 56);
        theme.accent            = RGB(120, 140, 255);
    } else if (strcmp(name, "light") == 0) {
        theme.desktop_top       = RGB(214, 224, 240);
        theme.desktop_bottom    = RGB(180, 196, 220);
        theme.panel             = RGB(236, 240, 248);
        theme.window_bg         = RGB(250, 251, 253);
        theme.titlebar_active   = RGB(222, 230, 244);
        theme.accent            = RGB(40, 110, 220);
    } else if (strcmp(name, "forest") == 0) {
        theme.desktop_top       = RGB(16, 32, 26);
        theme.desktop_bottom    = RGB(28, 52, 40);
        theme.panel             = RGB(18, 34, 28);
        theme.window_bg         = RGB(24, 40, 34);
        theme.titlebar_active   = RGB(38, 62, 50);
        theme.accent            = RGB(90, 210, 140);
    } else {
        /* "aurora" is the default. */
        theme.desktop_top       = RGB(22, 26, 48);
        theme.desktop_bottom    = RGB(48, 32, 70);
        theme.panel             = RGB(20, 22, 34);
        theme.window_bg         = RGB(28, 31, 44);
        theme.titlebar_active   = RGB(44, 50, 74);
        theme.accent            = RGB(110, 170, 255);
    }

    /* Derived colours that are the same across themes. */
    bool_t light = COLOR_R(theme.window_bg) > 128;

    theme.window_text            = light ? RGB(28, 32, 40) : RGB(224, 230, 242);
    theme.panel_text             = light ? RGB(32, 38, 48) : RGB(214, 222, 238);
    theme.titlebar_inactive      = light ? RGB(236, 238, 244) : RGB(30, 33, 46);
    theme.titlebar_text          = light ? RGB(20, 24, 32) : RGB(236, 240, 250);
    theme.titlebar_text_inactive = light ? RGB(120, 128, 142) : RGB(128, 136, 156);
    theme.border                 = light ? RGB(184, 192, 206) : RGB(48, 52, 70);
    theme.border_focused         = theme.accent;
    theme.shadow                 = RGB(0, 0, 0);
    theme.menu_bg                = light ? RGB(248, 249, 252) : RGB(30, 33, 48);
    theme.menu_hover             = theme.accent;
    theme.input_bg               = light ? RGB(255, 255, 255) : RGB(18, 20, 30);

    full_redraw = true;
}

const struct theme *desktop_theme(void)
{
    return &theme;
}

/* --- application registry --------------------------------------------- */

int desktop_register_app(const char *name, const char *icon,
                         const struct app_ops *ops, int width, int height,
                         color_t accent, size_t state_size, bool_t show_in_menu)
{
    if (application_count >= MAX_APPS) {
        KLOG_ERR("desktop", "cannot register '%s': application table full", name);
        return -1;
    }

    struct application *app = &applications[application_count++];

    strlcpy(app->name, name, sizeof(app->name));
    strlcpy(app->icon, icon, sizeof(app->icon));

    /*
     * The QAC icon is looked up by a lowercased, single-word form of the
     * application name, which is how tools/mkqac.py names the files.
     */
    size_t out = 0;
    for (const char *p = name; *p && out < sizeof(app->icon_name) - 1; p++) {
        if (*p == ' ') {
            break;
        }
        app->icon_name[out++] = (char)tolower((uint8_t)*p);
    }
    app->icon_name[out] = '\0';
    app->ops            = ops;
    app->default_width  = width;
    app->default_height = height;
    app->accent         = accent;
    app->state_size     = state_size;
    app->show_in_menu   = show_in_menu;

    return application_count - 1;
}

const struct application *desktop_find_app(const char *name)
{
    for (int i = 0; i < application_count; i++) {
        if (strcmp(applications[i].name, name) == 0) {
            return &applications[i];
        }
    }
    return NULL;
}

int desktop_app_count(void)
{
    return application_count;
}

const struct application *desktop_app_at(int index)
{
    if (index < 0 || index >= application_count) {
        return NULL;
    }
    return &applications[index];
}

/* --- windows ---------------------------------------------------------- */

void window_client_rect(const struct window *win, struct rect *out)
{
    out->x = win->x + BORDER_WIDTH;
    out->y = win->y + TITLEBAR_HEIGHT;
    out->w = win->w - BORDER_WIDTH * 2;
    out->h = win->h - TITLEBAR_HEIGHT - BORDER_WIDTH;
}

int window_count(void)
{
    return window_total;
}

struct window *window_at(int index)
{
    if (index < 0 || index >= window_total) {
        return NULL;
    }
    return &windows[index];
}

struct window *window_focused(void)
{
    return focused_window;
}

void window_invalidate(struct window *win)
{
    if (win) {
        win->needs_redraw = true;
    }
}

void desktop_invalidate(void)
{
    full_redraw = true;
}

void window_set_title(struct window *win, const char *title)
{
    if (win) {
        strlcpy(win->title, title, sizeof(win->title));
        win->needs_redraw = true;
        full_redraw       = true;
    }
}

void window_focus(struct window *win)
{
    if (!win || !win->visible) {
        return;
    }

    if (focused_window && focused_window != win) {
        focused_window->focused      = false;
        focused_window->needs_redraw = true;
    }

    /* Raise to the top of the stack. */
    int top = 0;
    for (int i = 0; i < window_total; i++) {
        if (windows[i].z_order > top) {
            top = windows[i].z_order;
        }
    }
    win->z_order     = top + 1;
    win->focused     = true;
    win->needs_redraw = true;
    focused_window   = win;
    full_redraw      = true;
}

struct window *window_create_at(const char *app_name, int x, int y)
{
    const struct application *app = desktop_find_app(app_name);
    if (!app) {
        KLOG_ERR("desktop", "no such application: %s", app_name);
        return NULL;
    }
    if (window_total >= MAX_WINDOWS) {
        desktop_notify("Desktop", "Too many windows are open");
        return NULL;
    }

    struct window *win = &windows[window_total++];
    memset(win, 0, sizeof(*win));

    win->id  = next_window_id++;
    win->app = app;
    strlcpy(win->title, app->name, sizeof(win->title));

    win->w = MIN(app->default_width, fb_width() - 40);
    win->h = MIN(app->default_height, fb_height() - PANEL_HEIGHT - 40);
    win->x = CLAMP(x, 0, MAX(fb_width() - win->w, 0));
    win->y = CLAMP(y, PANEL_HEIGHT + 4,
                   MAX(fb_height() - win->h - 4, PANEL_HEIGHT + 4));

    win->visible       = true;
    win->resizable     = true;
    win->state         = WINDOW_NORMAL;
    win->needs_redraw  = true;
    win->created_at_ms = time_uptime_ms();

    if (app->state_size) {
        win->app_state = kzalloc(app->state_size);
        if (!win->app_state) {
            window_total--;
            desktop_notify("Desktop", "Out of memory opening the application");
            return NULL;
        }
    }

    if (app->ops && app->ops->on_open) {
        app->ops->on_open(win);
    }

    window_focus(win);
    KLOG_DEBUG("desktop", "opened window %d (%s)", win->id, app->name);
    return win;
}

struct window *window_create(const char *app_name)
{
    /* Cascade new windows so they do not sit exactly on top of each other. */
    int offset = (window_total % 8) * 26;
    return window_create_at(app_name, 70 + offset, PANEL_HEIGHT + 30 + offset);
}

void window_close(struct window *win)
{
    if (!win) {
        return;
    }

    if (win->app && win->app->ops && win->app->ops->on_close) {
        win->app->ops->on_close(win);
    }
    if (win->app_state) {
        kfree(win->app_state);
        win->app_state = NULL;
    }

    int index = (int)(win - windows);
    KLOG_DEBUG("desktop", "closed window %d", win->id);

    for (int i = index; i < window_total - 1; i++) {
        windows[i] = windows[i + 1];
    }
    window_total--;

    /* Focus whatever is now on top. */
    focused_window = NULL;
    struct window *top = NULL;
    for (int i = 0; i < window_total; i++) {
        windows[i].focused = false;
        if (windows[i].visible && (!top || windows[i].z_order > top->z_order)) {
            top = &windows[i];
        }
    }
    if (top) {
        window_focus(top);
    }
    full_redraw = true;
}

void window_minimise(struct window *win)
{
    if (!win) {
        return;
    }
    win->state   = WINDOW_MINIMISED;
    win->visible = false;
    win->focused = false;

    if (focused_window == win) {
        focused_window = NULL;
        struct window *top = NULL;
        for (int i = 0; i < window_total; i++) {
            if (windows[i].visible && (!top || windows[i].z_order > top->z_order)) {
                top = &windows[i];
            }
        }
        if (top) {
            window_focus(top);
        }
    }
    full_redraw = true;
}

void window_maximise(struct window *win)
{
    if (!win) {
        return;
    }

    if (win->state == WINDOW_MAXIMISED) {
        window_restore(win);
        return;
    }

    win->restore_x = win->x;
    win->restore_y = win->y;
    win->restore_w = win->w;
    win->restore_h = win->h;

    win->x     = 0;
    win->y     = PANEL_HEIGHT;
    win->w     = fb_width();
    win->h     = fb_height() - PANEL_HEIGHT;
    win->state = WINDOW_MAXIMISED;

    win->needs_redraw = true;
    full_redraw       = true;
}

void window_restore(struct window *win)
{
    if (!win) {
        return;
    }

    if (win->state == WINDOW_MAXIMISED) {
        win->x = win->restore_x;
        win->y = win->restore_y;
        win->w = win->restore_w;
        win->h = win->restore_h;
    }
    win->state   = WINDOW_NORMAL;
    win->visible = true;
    win->needs_redraw = true;
    full_redraw  = true;
    window_focus(win);
}

/* Topmost visible window containing the point, or NULL. */
static struct window *window_hit_test(int x, int y)
{
    struct window *best = NULL;

    for (int i = 0; i < window_total; i++) {
        struct window *win = &windows[i];
        if (!win->visible) {
            continue;
        }
        if (x >= win->x && x < win->x + win->w && y >= win->y &&
            y < win->y + win->h) {
            if (!best || win->z_order > best->z_order) {
                best = win;
            }
        }
    }
    return best;
}

/* --- notifications ---------------------------------------------------- */

void desktop_notify(const char *title, const char *message)
{
    for (int i = 0; i < MAX_NOTIFICATIONS; i++) {
        if (notifications[i].active) {
            continue;
        }
        strlcpy(notifications[i].title, title, sizeof(notifications[i].title));
        strlcpy(notifications[i].message, message,
                sizeof(notifications[i].message));
        notifications[i].expires_ms = time_uptime_ms() + 4500;
        notifications[i].active     = true;
        full_redraw                 = true;

        KLOG_INFO("desktop", "notification: %s - %s", title, message);
        return;
    }

    /* All slots busy: replace the oldest. */
    notifications[0].active = false;
    desktop_notify(title, message);
}

/* --- drawing ---------------------------------------------------------- */

static void draw_wallpaper(void)
{
    const char *style = config_get_string("desktop.wallpaper", "gradient");

    if (strcmp(style, "solid") == 0) {
        fb_clear(theme.desktop_bottom);
    } else {
        fb_fill_gradient_v(0, 0, fb_width(), fb_height(), theme.desktop_top,
                           theme.desktop_bottom);
    }

    if (strcmp(style, "stars") == 0) {
        /* Deterministic pseudo-random starfield so it does not shimmer. */
        uint32_t seed = 0x51524121;
        for (int i = 0; i < 220; i++) {
            seed = seed * 1103515245u + 12345u;
            int sx = (int)((seed >> 8) % (uint32_t)fb_width());
            seed = seed * 1103515245u + 12345u;
            int sy = (int)((seed >> 8) % (uint32_t)fb_height());
            seed = seed * 1103515245u + 12345u;
            int brightness = 120 + (int)((seed >> 16) % 130);
            fb_put_pixel(sx, sy, RGB(brightness, brightness, brightness + 20));
        }
    }

    /* A subtle logo in the lower right. */
    const char *label = "Qira OS " QIRA_VERSION_STRING;
    fb_draw_string(fb_width() - fb_text_width(label) - 14, fb_height() - 20, label,
                   RGB(COLOR_R(theme.desktop_bottom) + 40,
                       COLOR_G(theme.desktop_bottom) + 40,
                       COLOR_B(theme.desktop_bottom) + 46));
}

/* Small vector-ish glyphs for the title bar buttons. */
static void draw_window_buttons(struct window *win)
{
    int size    = 14;
    int spacing = 20;
    int cy      = win->y + TITLEBAR_HEIGHT / 2;
    int right   = win->x + win->w - 12;

    /* Close. */
    int close_x = right - size / 2;
    fb_fill_circle(close_x, cy, size / 2, RGB(232, 90, 88));
    for (int i = -3; i <= 3; i++) {
        fb_put_pixel(close_x + i, cy + i, RGB(60, 20, 20));
        fb_put_pixel(close_x + i, cy - i, RGB(60, 20, 20));
    }

    /* Maximise. */
    int max_x = close_x - spacing;
    fb_fill_circle(max_x, cy, size / 2, RGB(238, 190, 82));
    fb_draw_rect(max_x - 3, cy - 3, 7, 7, RGB(70, 50, 12));

    /* Minimise. */
    int min_x = max_x - spacing;
    fb_fill_circle(min_x, cy, size / 2, RGB(120, 200, 118));
    fb_fill_rect(min_x - 3, cy + 2, 7, 2, RGB(24, 60, 24));
}

static void draw_window(struct window *win)
{
    if (!win->visible) {
        return;
    }

    /* Drop shadow. */
    if (config_get_bool("wm.window_shadows", true)) {
        for (int i = 1; i <= 4; i++) {
            fb_fill_rect_alpha(win->x + i, win->y + win->h + i - 1, win->w, 1,
                               theme.shadow, (uint8_t)(50 - i * 10));
            fb_fill_rect_alpha(win->x + win->w + i - 1, win->y + i, 1, win->h,
                               theme.shadow, (uint8_t)(50 - i * 10));
        }
    }

    /* Title bar. */
    color_t bar = win->focused ? theme.titlebar_active : theme.titlebar_inactive;
    fb_fill_rect(win->x, win->y, win->w, TITLEBAR_HEIGHT, bar);

    /* A thin accent stripe under the title bar of the focused window. */
    if (win->focused) {
        color_t accent = win->app ? win->app->accent : theme.accent;
        fb_fill_rect(win->x, win->y + TITLEBAR_HEIGHT - 2, win->w, 2, accent);
    }

    /* Application icon and title. */
    color_t title_color =
        win->focused ? theme.titlebar_text : theme.titlebar_text_inactive;

    int text_x = win->x + 10;
    if (win->app) {
        const struct qac_image *icon = qac_get(win->app->icon_name);
        if (icon) {
            qac_draw_scaled(icon, text_x, win->y + (TITLEBAR_HEIGHT - 16) / 2, 16);
            text_x += 22;
        } else if (win->app->icon[0]) {
            fb_draw_string(text_x, win->y + (TITLEBAR_HEIGHT - FONT_HEIGHT) / 2,
                           win->app->icon,
                           win->focused ? win->app->accent : title_color);
            text_x += fb_text_width(win->app->icon) + 8;
        }
    }

    int title_space = win->w - (text_x - win->x) - 76;
    if (title_space > 0) {
        fb_draw_string_clipped(text_x, win->y + (TITLEBAR_HEIGHT - FONT_HEIGHT) / 2,
                               win->title, title_color, title_space);
    }

    draw_window_buttons(win);

    /* Client background. */
    struct rect client;
    window_client_rect(win, &client);
    fb_fill_rect(client.x, client.y, client.w, client.h, theme.window_bg);

    /* Client content, clipped to the client area. */
    if (win->app && win->app->ops && win->app->ops->draw) {
        fb_set_clip(client.x, client.y, client.w, client.h);
        win->app->ops->draw(win, client.x, client.y, client.w, client.h);
        fb_reset_clip();
    }

    /* Border. */
    fb_draw_rect(win->x, win->y, win->w, win->h,
                 win->focused ? theme.border_focused : theme.border);

    /* Resize grip in the bottom-right corner. */
    if (win->resizable && win->state != WINDOW_MAXIMISED) {
        int gx = win->x + win->w - 4;
        int gy = win->y + win->h - 4;
        for (int i = 0; i < 3; i++) {
            for (int j = 0; j < 3 - i; j++) {
                fb_put_pixel(gx - i * 4, gy - j * 4, theme.border_focused);
                fb_put_pixel(gx - i * 4 + 1, gy - j * 4, theme.border_focused);
            }
        }
    }

    win->needs_redraw = false;
}

/* Panel item geometry, shared between drawing and hit testing. */
#define MENU_BUTTON_W 96

static void draw_panel(void)
{
    panel_y = 0;   /* the panel is at the top */

    fb_fill_rect(0, panel_y, fb_width(), PANEL_HEIGHT, theme.panel);
    fb_fill_rect(0, panel_y + PANEL_HEIGHT - 1, fb_width(), 1, theme.border);

    /* Application menu button. */
    bool_t hovered = cursor_y < PANEL_HEIGHT && cursor_x < MENU_BUTTON_W;
    if (menu_open || hovered) {
        fb_fill_rect(0, panel_y, MENU_BUTTON_W, PANEL_HEIGHT - 1,
                     menu_open ? theme.accent : theme.titlebar_active);
    }
    color_t menu_text = menu_open ? RGB(255, 255, 255) : theme.panel_text;
    fb_draw_string(14, panel_y + (PANEL_HEIGHT - FONT_HEIGHT) / 2, "Qira",
                   menu_open ? menu_text : theme.accent);
    fb_draw_string(14 + fb_text_width("Qira") + 8,
                   panel_y + (PANEL_HEIGHT - FONT_HEIGHT) / 2, "Menu", menu_text);

    /* Window buttons in the task list. */
    int x = MENU_BUTTON_W + 8;
    for (int i = 0; i < window_total && x < fb_width() - 220; i++) {
        struct window *win = &windows[i];

        int width = 128;
        bool_t active = win->focused && win->visible;

        fb_fill_round_rect(x, panel_y + 4, width, PANEL_HEIGHT - 9, 4,
                           active ? theme.titlebar_active
                                  : (win->visible ? theme.panel : theme.panel));
        if (active) {
            color_t accent = win->app ? win->app->accent : theme.accent;
            fb_fill_rect(x, panel_y + PANEL_HEIGHT - 6, width, 2, accent);
        }

        color_t label = win->visible ? theme.panel_text : theme.titlebar_text_inactive;
        fb_draw_string_clipped(x + 8, panel_y + (PANEL_HEIGHT - FONT_HEIGHT) / 2,
                               win->title, label, width - 16);
        x += width + 6;
    }

    /* Status area on the right: memory, tasks, clock. */
    char status[96];
    struct qira_time now;
    time_from_unix(rtc_unix_time(), &now);

    if (config_get_bool("desktop.show_clock", true)) {
        snprintf(status, sizeof(status), "%02d:%02d:%02d", now.hour, now.minute,
                 now.second);
        int clock_x = fb_width() - fb_text_width(status) - 14;
        fb_draw_string(clock_x, panel_y + (PANEL_HEIGHT - FONT_HEIGHT) / 2, status,
                       theme.panel_text);

        char resources[64];
        snprintf(resources, sizeof(resources), "%lluM free  %d tasks",
                 (unsigned long long)(pmm_free_bytes() / (1024 * 1024)),
                 sched_task_count());
        fb_draw_string(clock_x - fb_text_width(resources) - 20,
                       panel_y + (PANEL_HEIGHT - FONT_HEIGHT) / 2, resources,
                       theme.titlebar_text_inactive);
    }
}

static void draw_menu(void)
{
    if (!menu_open) {
        return;
    }

    int visible_apps = 0;
    for (int i = 0; i < application_count; i++) {
        if (applications[i].show_in_menu) {
            visible_apps++;
        }
    }

    int row_height = 26;
    menu_x = 0;
    menu_y = PANEL_HEIGHT;
    menu_w = 230;
    menu_h = visible_apps * row_height + 16;

    /* Shadow, then the panel. */
    fb_fill_rect_alpha(menu_x + 4, menu_y + 4, menu_w, menu_h, theme.shadow, 70);
    fb_fill_rect(menu_x, menu_y, menu_w, menu_h, theme.menu_bg);
    fb_draw_rect(menu_x, menu_y, menu_w, menu_h, theme.border);

    int row = 0;
    for (int i = 0; i < application_count; i++) {
        const struct application *app = &applications[i];
        if (!app->show_in_menu) {
            continue;
        }

        int y = menu_y + 8 + row * row_height;
        bool_t hovered = (menu_hover == row);

        if (hovered) {
            fb_fill_rect(menu_x + 4, y, menu_w - 8, row_height, theme.menu_hover);
        }

        color_t text = hovered ? RGB(255, 255, 255) : theme.window_text;

        /* Prefer the application's QAC icon, falling back to its glyphs. */
        const struct qac_image *icon = qac_get(app->icon_name);
        if (icon) {
            qac_draw_scaled(icon, menu_x + 12, y + (row_height - 18) / 2, 18);
        } else {
            fb_draw_string(menu_x + 14, y + (row_height - FONT_HEIGHT) / 2,
                           app->icon, hovered ? text : app->accent);
        }

        fb_draw_string(menu_x + 44, y + (row_height - FONT_HEIGHT) / 2, app->name,
                       text);
        row++;
    }
}

static void draw_notifications(void)
{
    uint64_t now = time_uptime_ms();
    int      slot = 0;

    for (int i = 0; i < MAX_NOTIFICATIONS; i++) {
        struct notification *note = &notifications[i];
        if (!note->active) {
            continue;
        }
        if (now > note->expires_ms) {
            note->active = false;
            full_redraw  = true;
            continue;
        }

        /*
         * Wrap the message on word boundaries so nothing is silently cut off,
         * and size the panel to however many lines that needs.
         */
        int width      = 340;
        int text_width = width - 28;
        int max_chars  = text_width / FONT_WIDTH;

        char   wrapped[4][64];
        int    line_count = 0;
        size_t position   = 0;
        size_t length     = strlen(note->message);

        while (position < length && line_count < 4) {
            size_t take = MIN((size_t)max_chars, length - position);

            /* Break at the last space that fits, unless the rest fits whole. */
            if (position + take < length) {
                size_t back = take;
                while (back > 0 && note->message[position + back] != ' ') {
                    back--;
                }
                if (back > 0) {
                    take = back;
                }
            }

            size_t copy = MIN(take, sizeof(wrapped[0]) - 1);
            memcpy(wrapped[line_count], note->message + position, copy);
            wrapped[line_count][copy] = '\0';
            line_count++;

            position += take;
            while (position < length && note->message[position] == ' ') {
                position++;
            }
        }
        if (line_count == 0) {
            wrapped[line_count][0] = '\0';
            line_count             = 1;
        }

        int height = 34 + line_count * (FONT_HEIGHT + 2) + 8;
        int x      = fb_width() - width - 16;
        int y      = PANEL_HEIGHT + 16 + slot * (height + 10);

        fb_fill_rect_alpha(x + 3, y + 3, width, height, theme.shadow, 80);
        fb_fill_round_rect(x, y, width, height, 6, theme.menu_bg);
        fb_draw_round_rect(x, y, width, height, 6, theme.accent);
        fb_fill_rect(x, y + 6, 3, height - 12, theme.accent);

        fb_draw_string(x + 14, y + 12, note->title, theme.accent);
        for (int line = 0; line < line_count; line++) {
            fb_draw_string(x + 14, y + 34 + line * (FONT_HEIGHT + 2), wrapped[line],
                           theme.window_text);
        }
        slot++;
    }
}

/* Draw the mouse pointer as a classic arrow. */
static void draw_cursor(int x, int y)
{
    static const char *shape[] = {
        "X         ",
        "XX        ",
        "XoX       ",
        "XooX      ",
        "XoooX     ",
        "XooooX    ",
        "XoooooX   ",
        "XooooooX  ",
        "XoooooooX ",
        "XooooXXXXX",
        "XooXoX    ",
        "XoX XoX   ",
        "XX  XoX   ",
        "X    XoX  ",
        "     XoX  ",
        "      XX  ",
    };

    for (int row = 0; row < (int)ARRAY_SIZE(shape); row++) {
        for (int col = 0; shape[row][col]; col++) {
            char pixel = shape[row][col];
            if (pixel == 'X') {
                fb_put_pixel(x + col, y + row, RGB(20, 22, 30));
            } else if (pixel == 'o') {
                fb_put_pixel(x + col, y + row, RGB(250, 251, 255));
            }
        }
    }
}

/* --- input handling --------------------------------------------------- */

/* Which decoration, if any, is under the pointer. */
typedef enum {
    HIT_NONE = 0,
    HIT_TITLEBAR,
    HIT_CLOSE,
    HIT_MAXIMISE,
    HIT_MINIMISE,
    HIT_RESIZE,
    HIT_CLIENT,
} hit_t;

static hit_t hit_test_window(struct window *win, int x, int y)
{
    if (x < win->x || x >= win->x + win->w || y < win->y || y >= win->y + win->h) {
        return HIT_NONE;
    }

    if (y < win->y + TITLEBAR_HEIGHT) {
        int right = win->x + win->w - 12;
        int cy    = win->y + TITLEBAR_HEIGHT / 2;

        if (y >= cy - 9 && y <= cy + 9) {
            if (x >= right - 14 && x <= right) {
                return HIT_CLOSE;
            }
            if (x >= right - 34 && x <= right - 20) {
                return HIT_MAXIMISE;
            }
            if (x >= right - 54 && x <= right - 40) {
                return HIT_MINIMISE;
            }
        }
        return HIT_TITLEBAR;
    }

    if (win->resizable && win->state != WINDOW_MAXIMISED &&
        x >= win->x + win->w - RESIZE_HANDLE &&
        y >= win->y + win->h - RESIZE_HANDLE) {
        return HIT_RESIZE;
    }

    return HIT_CLIENT;
}

static void open_menu_item(int row)
{
    int current = 0;
    for (int i = 0; i < application_count; i++) {
        if (!applications[i].show_in_menu) {
            continue;
        }
        if (current == row) {
            window_create(applications[i].name);
            return;
        }
        current++;
    }
}

static void handle_mouse(const struct input_event *event)
{
    cursor_x = event->x;
    cursor_y = event->y;
    cursor_buttons = event->code;

    bool_t pressed  = (cursor_buttons & MOUSE_LEFT) &&
                     !(previous_buttons & MOUSE_LEFT);
    bool_t released = !(cursor_buttons & MOUSE_LEFT) &&
                      (previous_buttons & MOUSE_LEFT);

    /* Dragging or resizing takes priority over everything else. */
    if (drag_window && (cursor_buttons & MOUSE_LEFT)) {
        if (resizing) {
            int new_w = cursor_x - drag_window->x + drag_offset_x;
            int new_h = cursor_y - drag_window->y + drag_offset_y;
            drag_window->w = CLAMP(new_w, 220, fb_width() - drag_window->x);
            drag_window->h = CLAMP(new_h, 120, fb_height() - drag_window->y);
        } else {
            drag_window->x = cursor_x - drag_offset_x;
            drag_window->y = cursor_y - drag_offset_y;

            /* Keep the title bar reachable. */
            drag_window->x = CLAMP(drag_window->x, -(drag_window->w - 80),
                                   fb_width() - 80);
            drag_window->y = CLAMP(drag_window->y, PANEL_HEIGHT,
                                   fb_height() - TITLEBAR_HEIGHT);

            /* Snap to the top edge to maximise. */
            if (config_get_bool("wm.snap_to_edges", true) &&
                cursor_y <= PANEL_HEIGHT + 2 && drag_window->state == WINDOW_NORMAL) {
                /* Show the intent by highlighting; commit on release. */
            }
        }
        drag_window->needs_redraw = true;
        full_redraw               = true;
        previous_buttons          = cursor_buttons;
        return;
    }

    if (released) {
        if (drag_window && !resizing &&
            config_get_bool("wm.snap_to_edges", true) &&
            cursor_y <= PANEL_HEIGHT + 2) {
            window_maximise(drag_window);
        }
        drag_window = NULL;
        resizing    = false;
    }

    /* Menu interaction. */
    if (menu_open) {
        if (cursor_x >= menu_x && cursor_x < menu_x + menu_w && cursor_y >= menu_y &&
            cursor_y < menu_y + menu_h) {
            int row = (cursor_y - menu_y - 8) / 26;
            if (row != menu_hover) {
                menu_hover  = row;
                full_redraw = true;
            }
            if (pressed) {
                menu_open   = false;
                menu_hover  = -1;
                full_redraw = true;
                open_menu_item(row);
                previous_buttons = cursor_buttons;
                return;
            }
        } else if (pressed) {
            menu_open   = false;
            menu_hover  = -1;
            full_redraw = true;
            /* Fall through so the click also reaches whatever is underneath. */
        }
    }

    /* Panel interaction. */
    if (cursor_y < PANEL_HEIGHT) {
        if (pressed) {
            if (cursor_x < MENU_BUTTON_W) {
                menu_open   = !menu_open;
                menu_hover  = -1;
                full_redraw = true;
            } else {
                /* Task list button: raise, or minimise if already focused. */
                int x = MENU_BUTTON_W + 8;
                for (int i = 0; i < window_total; i++) {
                    if (cursor_x >= x && cursor_x < x + 128) {
                        struct window *win = &windows[i];
                        if (win->visible && win->focused) {
                            window_minimise(win);
                        } else {
                            win->visible = true;
                            win->state   = (win->state == WINDOW_MINIMISED)
                                               ? WINDOW_NORMAL
                                               : win->state;
                            window_focus(win);
                        }
                        break;
                    }
                    x += 134;
                }
            }
        }
        previous_buttons = cursor_buttons;
        return;
    }

    /* Window interaction. */
    struct window *win = window_hit_test(cursor_x, cursor_y);

    if (pressed && win) {
        window_focus(win);

        switch (hit_test_window(win, cursor_x, cursor_y)) {
        case HIT_CLOSE:
            window_close(win);
            previous_buttons = cursor_buttons;
            return;
        case HIT_MAXIMISE:
            window_maximise(win);
            previous_buttons = cursor_buttons;
            return;
        case HIT_MINIMISE:
            window_minimise(win);
            previous_buttons = cursor_buttons;
            return;
        case HIT_TITLEBAR:
            drag_window   = win;
            resizing      = false;
            drag_offset_x = cursor_x - win->x;
            drag_offset_y = cursor_y - win->y;
            previous_buttons = cursor_buttons;
            return;
        case HIT_RESIZE:
            drag_window   = win;
            resizing      = true;
            drag_offset_x = win->x + win->w - cursor_x;
            drag_offset_y = win->y + win->h - cursor_y;
            previous_buttons = cursor_buttons;
            return;
        default:
            break;
        }
    }

    /* Deliver to the application. */
    if (win && win->app && win->app->ops && win->app->ops->on_mouse) {
        struct rect client;
        window_client_rect(win, &client);

        if (cursor_x >= client.x && cursor_y >= client.y) {
            if (win->app->ops->on_mouse(win, cursor_x - client.x,
                                        cursor_y - client.y, cursor_buttons,
                                        event->type)) {
                win->needs_redraw = true;
                full_redraw       = true;
            }
        }
    }

    previous_buttons = cursor_buttons;
}

static void handle_key(const struct input_event *event)
{
    uint32_t key = event->code;
    uint32_t mod = event->modifiers;

    /* F12 captures the screen for documentation and tests. */
    if (key == KEY_F12) {
        fb_capture_serial("manual");
        desktop_notify("Desktop", "Screen captured to the serial log");
        return;
    }

    /* Global shortcuts. */
    if (mod & MOD_SUPER) {
        if (key == ' ') {
            menu_open   = !menu_open;
            menu_hover  = -1;
            full_redraw = true;
            return;
        }
        if (key == 't' || key == 'T') {
            window_create("Terminal");
            return;
        }
        if (key == 'q' || key == 'Q') {
            if (focused_window) {
                window_close(focused_window);
            }
            return;
        }
    }

    if (mod & MOD_ALT) {
        /* Alt+Tab cycles focus. */
        if (key == KEY_TAB) {
            if (window_total > 1) {
                struct window *next = NULL;
                int lowest = 0x7FFFFFFF;
                for (int i = 0; i < window_total; i++) {
                    struct window *win = &windows[i];
                    if (!win->visible || win == focused_window) {
                        continue;
                    }
                    if (win->z_order < lowest) {
                        lowest = win->z_order;
                        next   = win;
                    }
                }
                if (next) {
                    window_focus(next);
                }
            }
            return;
        }
        /* Alt+F4 closes. */
        if (key == KEY_F4) {
            if (focused_window) {
                window_close(focused_window);
            }
            return;
        }
    }

    if (menu_open) {
        if (key == KEY_ESCAPE) {
            menu_open   = false;
            full_redraw = true;
            return;
        }
        if (key == KEY_UP || key == KEY_DOWN) {
            int visible = 0;
            for (int i = 0; i < application_count; i++) {
                if (applications[i].show_in_menu) {
                    visible++;
                }
            }
            menu_hover += (key == KEY_DOWN) ? 1 : -1;
            menu_hover  = CLAMP(menu_hover, 0, MAX(visible - 1, 0));
            full_redraw = true;
            return;
        }
        if (key == KEY_ENTER && menu_hover >= 0) {
            menu_open   = false;
            full_redraw = true;
            open_menu_item(menu_hover);
            return;
        }
    }

    /* Everything else goes to the focused window. */
    if (focused_window && focused_window->app && focused_window->app->ops &&
        focused_window->app->ops->on_key) {
        if (focused_window->app->ops->on_key(focused_window, key, mod)) {
            focused_window->needs_redraw = true;
            full_redraw                  = true;
        }
    }
}

/* --- main loop -------------------------------------------------------- */

void desktop_init(void)
{
    memset(windows, 0, sizeof(windows));
    memset(applications, 0, sizeof(applications));
    memset(notifications, 0, sizeof(notifications));

    window_total      = 0;
    application_count = 0;
    focused_window    = NULL;

    desktop_set_theme(config_get_string("desktop.theme", "aurora"));

    cursor_x = fb_width() / 2;
    cursor_y = fb_height() / 2;
    input_set_mouse(cursor_x, cursor_y);

    /* Register the built-in applications. */
    app_terminal_register();
    app_files_register();
    app_browser_register();
    app_editor_register();
    app_sysmon_register();
    app_settings_register();
    app_logs_register();
    app_calculator_register();
    app_clock_register();
    app_imageview_register();
    app_paint_register();
    app_network_register();
    app_notes_register();
    app_help_register();
    app_about_register();

    KLOG_INFO("desktop", "%d applications registered, %dx%d display",
              application_count, fb_width(), fb_height());
}

NORETURN void desktop_run(void)
{
    if (!fb_available()) {
        /*
         * Without a framebuffer there is no desktop; fall back to running
         * UltraShell directly on the serial/text console so the system is
         * still usable.
         */
        KLOG_WARN("desktop", "no framebuffer: falling back to a console shell");
        console_puts("\nQira OS - no graphics available, starting UltraShell.\n\n");
        for (;;) {
            shell_run(ultrashell_instance());
            ultrashell_instance()->running = true;
        }
    }

    desktop_init();

    /* Open a terminal so the system is immediately usable. */
    window_create("Terminal");
    desktop_notify("Welcome to Qira OS",
                   "Press Super+T for a terminal, or use the Qira menu.");

    /*
     * Automated tests cannot press a key, so `capture=<ms>[,<ms>...]` on the
     * kernel command line schedules unattended frame captures.
     */
    uint64_t capture_at[4] = {0};
    int      capture_count = 0;
    int      captures_done = 0;
    {
        extern const struct qira_boot_info *kernel_boot_info(void);
        const char *cmdline = kernel_boot_info()->cmdline;
        const char *option  = strstr(cmdline, "capture=");
        if (option) {
            option += 8;
            while (capture_count < 4) {
                uint64_t ms = 0;
                bool_t   any = false;
                while (*option >= '0' && *option <= '9') {
                    ms = ms * 10 + (uint64_t)(*option++ - '0');
                    any = true;
                }
                if (!any) {
                    break;
                }
                capture_at[capture_count++] = ms;
                if (*option != ',') {
                    break;
                }
                option++;
            }
            KLOG_INFO("desktop", "%d automatic capture(s) scheduled",
                      capture_count);
        }
    }

    uint64_t last_tick_ms = 0;

    for (;;) {
        /* 1. Drain the input queue. */
        struct input_event event;
        while (input_poll(&event)) {
            switch (event.type) {
            case INPUT_KEY_DOWN:
                handle_key(&event);
                break;
            case INPUT_MOUSE_MOVE:
            case INPUT_MOUSE_DOWN:
            case INPUT_MOUSE_UP:
            case INPUT_MOUSE_SCROLL:
                handle_mouse(&event);
                break;
            default:
                break;
            }
        }

        /* 2. Give applications a chance to update. */
        uint64_t now = time_uptime_ms();
        if (now - last_tick_ms >= 100) {
            last_tick_ms = now;
            for (int i = 0; i < window_total; i++) {
                struct window *win = &windows[i];
                if (win->visible && win->app && win->app->ops &&
                    win->app->ops->tick) {
                    if (win->app->ops->tick(win)) {
                        win->needs_redraw = true;
                        full_redraw       = true;
                    }
                }
            }
            /* Keep the load average current for the panel and top. */
            load_sample();

            /* The panel clock needs a repaint every second. */
            full_redraw = true;
        }

        /* 3. Composite, if anything changed. */
        bool_t cursor_moved = (cursor_x != prev_cursor_x || cursor_y != prev_cursor_y);

        if (full_redraw || cursor_moved) {
            draw_wallpaper();

            /*
             * Draw the windows back to front. Rather than reordering the
             * window array (which would invalidate the pointers applications
             * hold), sort a list of indices by z-order each frame.
             */
            int order[MAX_WINDOWS];
            int visible_count = 0;
            for (int i = 0; i < window_total; i++) {
                if (windows[i].visible) {
                    order[visible_count++] = i;
                }
            }
            for (int i = 1; i < visible_count; i++) {
                int key = order[i];
                int j   = i - 1;
                while (j >= 0 && windows[order[j]].z_order > windows[key].z_order) {
                    order[j + 1] = order[j];
                    j--;
                }
                order[j + 1] = key;
            }
            for (int i = 0; i < visible_count; i++) {
                draw_window(&windows[order[i]]);
            }

            draw_panel();
            draw_menu();
            draw_notifications();
            draw_cursor(cursor_x, cursor_y);

            fb_flush();

            prev_cursor_x = cursor_x;
            prev_cursor_y = cursor_y;
            full_redraw   = false;
            frame_count++;

            /* Unattended capture, once the frame is fully composited. */
            if (captures_done < capture_count &&
                time_uptime_ms() >= capture_at[captures_done]) {
                char label[32];
                snprintf(label, sizeof(label), "auto%d", captures_done);
                fb_capture_serial(label);
                captures_done++;
            }
        }

        /* 4. Yield so other tasks run; the desktop is not a busy loop. */
        sched_sleep_ms(16);
    }
}
