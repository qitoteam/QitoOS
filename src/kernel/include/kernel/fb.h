/*
 * Qira OS - framebuffer graphics
 *
 * A simple 32-bit ARGB software rasteriser over the linear framebuffer that
 * the bootloader programmed through VBE. All drawing goes to an off-screen
 * back buffer which is then flushed to video memory, so the desktop never
 * shows partially drawn frames.
 */
#ifndef QIRA_FB_H
#define QIRA_FB_H

#include <kernel/types.h>
#include "../../../boot/bootinfo.h"

typedef uint32_t color_t;

/* Pack an 8-bit-per-channel colour. */
#define RGB(r, g, b)     ((color_t)(((uint32_t)(r) << 16) | ((uint32_t)(g) << 8) | (uint32_t)(b)))
#define ARGB(a, r, g, b) ((color_t)(((uint32_t)(a) << 24) | ((uint32_t)(r) << 16) | \
                                    ((uint32_t)(g) << 8) | (uint32_t)(b)))

#define COLOR_R(c) (((c) >> 16) & 0xFF)
#define COLOR_G(c) (((c) >> 8) & 0xFF)
#define COLOR_B(c) ((c) & 0xFF)
#define COLOR_A(c) (((c) >> 24) & 0xFF)

struct rect {
    int x, y, w, h;
};

struct fb_info {
    uint32_t *back;      /* software back buffer                        */
    uint32_t *front;     /* mapped video memory                         */
    int       width;
    int       height;
    int       pitch;     /* in pixels, not bytes                        */
    int       bpp;
    bool_t    ready;
    uint64_t  frames;
};

bool_t fb_init(const struct qira_boot_info *boot);
const struct fb_info *fb_get_info(void);
bool_t fb_available(void);

int  fb_width(void);
int  fb_height(void);

/* Direct back-buffer access for compositing. */
uint32_t *fb_backbuffer(void);

void fb_clear(color_t color);
void fb_put_pixel(int x, int y, color_t color);
color_t fb_get_pixel(int x, int y);

void fb_fill_rect(int x, int y, int w, int h, color_t color);
void fb_draw_rect(int x, int y, int w, int h, color_t color);
void fb_fill_rect_alpha(int x, int y, int w, int h, color_t color, uint8_t alpha);
void fb_fill_round_rect(int x, int y, int w, int h, int radius, color_t color);
void fb_draw_round_rect(int x, int y, int w, int h, int radius, color_t color);

/* Vertical linear gradient between two colours. */
void fb_fill_gradient_v(int x, int y, int w, int h, color_t top, color_t bottom);
void fb_fill_gradient_h(int x, int y, int w, int h, color_t left, color_t right);

void fb_draw_line(int x0, int y0, int x1, int y1, color_t color);
void fb_draw_circle(int cx, int cy, int radius, color_t color);
void fb_fill_circle(int cx, int cy, int radius, color_t color);

/* Blit a 32-bit ARGB source buffer. */
void fb_blit(int x, int y, int w, int h, const uint32_t *src, int src_pitch);
void fb_blit_alpha(int x, int y, int w, int h, const uint32_t *src, int src_pitch);

/* Text rendering with the built-in bitmap fonts. */
void fb_draw_char(int x, int y, char c, color_t fg);
void fb_draw_char_bg(int x, int y, char c, color_t fg, color_t bg);
void fb_draw_string(int x, int y, const char *s, color_t fg);
void fb_draw_string_bg(int x, int y, const char *s, color_t fg, color_t bg);
void fb_draw_string_clipped(int x, int y, const char *s, color_t fg, int max_width);
int  fb_text_width(const char *s);

/* Large font used by titles. */
void fb_draw_string_large(int x, int y, const char *s, color_t fg);
int  fb_text_width_large(const char *s);

/* Metrics of the current interface face. */
void fb_blend_pixel(int x, int y, color_t color, uint8_t alpha);

int fb_font_height(void);
int fb_font_width(void);

/*
 * Draw with an explicit face and integer scale factor. The terminal uses
 * these with font_terminal() so it can be monospaced while the rest of the
 * interface uses a different typeface.
 */
struct font;
void fb_draw_char_in(const struct font *font, int x, int y, char c, color_t fg,
                     int scale);
void fb_draw_string_in(const struct font *font, int x, int y, const char *s,
                       color_t fg, int scale);
void fb_draw_string_clipped_in(const struct font *font, int x, int y,
                               const char *s, color_t fg, int max_width,
                               int scale);
int  fb_text_width_in(const struct font *font, const char *s, int scale);

#define FONT_WIDTH        8
#define FONT_HEIGHT       16
#define FONT_LARGE_WIDTH  16
#define FONT_LARGE_HEIGHT 32

/* Clipping: all drawing is confined to the current clip rectangle. */
void fb_set_clip(int x, int y, int w, int h);
void fb_reset_clip(void);
void fb_get_clip(struct rect *out);

/* Present the back buffer. */
void fb_flush(void);
void fb_flush_rect(int x, int y, int w, int h);

/* Mark a region dirty so the next fb_flush() only copies what changed. */
void fb_mark_dirty(int x, int y, int w, int h);
void fb_mark_all_dirty(void);

/*
 * Write the current back buffer to `path` as a raw 32-bit BGRX dump.
 * tools/screenshot.py converts the result to a PNG. Returns 0 on success.
 */
int fb_screenshot(const char *path);

/*
 * Stream the current frame out of the serial port as base64 RGB. Used by the
 * automated boot tests to verify what is actually on screen.
 */
void fb_capture_serial(const char *label);
uint64_t fb_capture_count(void);

#endif /* QIRA_FB_H */
