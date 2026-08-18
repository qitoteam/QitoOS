/*
 * Qira OS - desktop environment and window manager
 *
 * The desktop owns the framebuffer once the system has booted. It composites
 * a wallpaper, a set of stacked windows, a panel, menus and notifications, and
 * routes input events to whichever window has focus.
 *
 * Applications are registered with an ops table rather than being separate
 * processes; each one draws into its window's client area and receives the
 * input events delivered to it.
 */
#ifndef QIRA_DESKTOP_H
#define QIRA_DESKTOP_H

#include <kernel/types.h>
#include <kernel/fb.h>
#include <kernel/input.h>

#define WINDOW_TITLE_MAX 64
#define MAX_WINDOWS      24
#define MAX_APPS         24
#define MAX_NOTIFICATIONS 8

#define TITLEBAR_HEIGHT 28
#define BORDER_WIDTH    1
#define PANEL_HEIGHT    32
#define RESIZE_HANDLE   14

struct window;

/* Application behaviour. Any hook may be NULL. */
struct app_ops {
    /* Draw the client area. Coordinates are window-relative; the framebuffer
     * clip has already been set to the client rectangle. */
    void (*draw)(struct window *win, int x, int y, int w, int h);

    /* Return true if the event was consumed. */
    bool_t (*on_key)(struct window *win, uint32_t key, uint32_t modifiers);
    bool_t (*on_mouse)(struct window *win, int x, int y, uint32_t buttons,
                       input_event_type_t type);

    /* Periodic update; return true to request a redraw. */
    bool_t (*tick)(struct window *win);

    void (*on_open)(struct window *win);
    void (*on_close)(struct window *win);
};

struct application {
    char                 name[32];
    char                 icon[4];        /* fallback glyphs, when no QAC icon */
    char                 icon_name[24];  /* QAC icon to look up, e.g. "files" */
    const struct app_ops *ops;
    int                  default_width;
    int                  default_height;
    bool_t               show_in_menu;
    color_t              accent;
    size_t               state_size;     /* per-window state to allocate */
};

typedef enum {
    WINDOW_NORMAL = 0,
    WINDOW_MINIMISED,
    WINDOW_MAXIMISED,
} window_state_t;

struct window {
    int    id;
    char   title[WINDOW_TITLE_MAX];

    int    x, y, w, h;
    /* Geometry remembered across maximise/restore. */
    int    restore_x, restore_y, restore_w, restore_h;

    window_state_t state;
    bool_t visible;
    bool_t focused;
    bool_t resizable;
    bool_t needs_redraw;

    const struct application *app;
    void  *app_state;

    uint64_t created_at_ms;
    int      z_order;
};

void desktop_init(void);

/* The desktop main loop; called by the init task and does not return. */
NORETURN void desktop_run(void);

/* Application registry. */
int  desktop_register_app(const char *name, const char *icon,
                          const struct app_ops *ops, int width, int height,
                          color_t accent, size_t state_size, bool_t show_in_menu);
const struct application *desktop_find_app(const char *name);
int  desktop_app_count(void);
const struct application *desktop_app_at(int index);

/* Window management. */
struct window *window_create(const char *app_name);
struct window *window_create_at(const char *app_name, int x, int y);
void window_close(struct window *win);
void window_focus(struct window *win);
void window_minimise(struct window *win);
void window_maximise(struct window *win);
void window_restore(struct window *win);
void window_invalidate(struct window *win);
void window_set_title(struct window *win, const char *title);

struct window *window_at(int index);
int  window_count(void);
struct window *window_focused(void);

/* Client area geometry, excluding the title bar and borders. */
void window_client_rect(const struct window *win, struct rect *out);

/* Notifications. */
void desktop_notify(const char *title, const char *message);

/* Redraw the whole desktop on the next frame. */
void desktop_invalidate(void);

/* Theme colours, resolved from the current configuration. */
struct theme {
    color_t desktop_top;
    color_t desktop_bottom;
    color_t panel;
    color_t panel_text;
    color_t window_bg;
    color_t window_text;
    color_t titlebar_active;
    color_t titlebar_inactive;
    color_t titlebar_text;
    color_t titlebar_text_inactive;
    color_t border;
    color_t border_focused;
    color_t accent;
    color_t shadow;
    color_t menu_bg;
    color_t menu_hover;
    color_t input_bg;
};
const struct theme *desktop_theme(void);
void desktop_set_theme(const char *name);

/* Applications provided by the desktop; each registers itself. */
void app_terminal_register(void);
void app_files_register(void);
void app_editor_register(void);
void app_sysmon_register(void);
void app_settings_register(void);
void app_logs_register(void);
void app_about_register(void);
void app_calculator_register(void);
void app_clock_register(void);
void app_help_register(void);
void app_browser_register(void);
void app_imageview_register(void);
void app_paint_register(void);
void app_network_register(void);
void app_notes_register(void);

#endif /* QIRA_DESKTOP_H */
