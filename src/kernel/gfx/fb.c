/*
 * QitoOS - framebuffer rasteriser
 *
 * Everything is drawn into a 32-bit ARGB back buffer in system RAM and then
 * copied to video memory. Reading from a VBE framebuffer is extremely slow
 * (it is uncached device memory), so keeping a shadow copy makes blending and
 * scrolling practical.
 *
 * A dirty rectangle is tracked so that a flush only transfers the part of the
 * screen that actually changed.
 */

#include <kernel/fb.h>
#include <kernel/mm.h>
#include <kernel/log.h>
#include <kernel/string.h>
#include <kernel/fs.h>

#include <kernel/font.h>

static struct fb_info fb;
static struct rect    clip;
static struct rect    dirty;
static bool_t         dirty_valid;

/* Channel shifts reported by VBE; used to repack colours if the mode is not
 * the usual 0x00RRGGBB layout. */
static uint8_t shift_r = 16, shift_g = 8, shift_b = 0;
static bool_t  direct_layout = true;

static inline uint32_t pack(color_t c)
{
    if (direct_layout) {
        return c;
    }
    return ((uint32_t)COLOR_R(c) << shift_r) | ((uint32_t)COLOR_G(c) << shift_g) |
           ((uint32_t)COLOR_B(c) << shift_b);
}

bool_t fb_init(const struct qito_boot_info *boot)
{
    if (!boot->fb_valid || boot->fb_addr == 0) {
        KLOG_WARN("fb", "no linear framebuffer supplied by the bootloader");
        fb.ready = false;
        return false;
    }

    fb.width  = boot->fb_width;
    fb.height = boot->fb_height;
    fb.bpp    = boot->fb_bpp;
    fb.pitch  = (int)(boot->fb_pitch / (boot->fb_bpp / 8));
    fb.front  = (uint32_t *)(uintptr_t)boot->fb_addr;

    if (fb.bpp != 32) {
        /*
         * Only 32bpp is supported by the rasteriser. A 24bpp mode would need
         * a packed 3-byte blit path; the bootloader prefers 32bpp and only
         * falls back to 24bpp when nothing else exists.
         */
        KLOG_WARN("fb", "%d bpp framebuffer is not supported, graphics disabled",
                  fb.bpp);
        fb.ready = false;
        return false;
    }

    shift_r = boot->fb_red_shift;
    shift_g = boot->fb_green_shift;
    shift_b = boot->fb_blue_shift;
    direct_layout = (shift_r == 16 && shift_g == 8 && shift_b == 0);

    /* Map the framebuffer as write-combining-ish (uncached) memory. */
    uint64_t fb_bytes = (uint64_t)boot->fb_pitch * fb.height;
    for (uint64_t offset = 0; offset < fb_bytes; offset += PAGE_SIZE) {
        vmm_map(vmm_kernel_space(), (uintptr_t)fb.front + offset,
                boot->fb_addr + offset,
                PTE_PRESENT | PTE_WRITE | PTE_PCD | PTE_NX);
    }

    /* Allocate the back buffer from the heap. */
    size_t back_bytes = (size_t)fb.pitch * fb.height * sizeof(uint32_t);
    fb.back           = kmalloc(back_bytes);
    if (!fb.back) {
        KLOG_ERR("fb", "cannot allocate a %llu KiB back buffer",
                 (unsigned long long)(back_bytes / 1024));
        fb.ready = false;
        return false;
    }
    memset(fb.back, 0, back_bytes);

    fb.ready  = true;
    fb.frames = 0;
    fb_reset_clip();
    fb_mark_all_dirty();

    KLOG_INFO("fb", "%dx%d at %d bpp, pitch %d px, framebuffer %p", fb.width,
              fb.height, fb.bpp, fb.pitch, (void *)fb.front);
    return true;
}

const struct fb_info *fb_get_info(void)
{
    return &fb;
}

bool_t fb_available(void)
{
    return fb.ready;
}

int fb_width(void)
{
    return fb.width;
}

int fb_height(void)
{
    return fb.height;
}

uint32_t *fb_backbuffer(void)
{
    return fb.back;
}

void fb_set_clip(int x, int y, int w, int h)
{
    /* Intersect the request with the screen. */
    int x0 = MAX(x, 0);
    int y0 = MAX(y, 0);
    int x1 = MIN(x + w, fb.width);
    int y1 = MIN(y + h, fb.height);

    clip.x = x0;
    clip.y = y0;
    clip.w = MAX(x1 - x0, 0);
    clip.h = MAX(y1 - y0, 0);
}

void fb_reset_clip(void)
{
    clip.x = 0;
    clip.y = 0;
    clip.w = fb.width;
    clip.h = fb.height;
}

void fb_get_clip(struct rect *out)
{
    *out = clip;
}

void fb_mark_dirty(int x, int y, int w, int h)
{
    if (w <= 0 || h <= 0) {
        return;
    }
    int x0 = MAX(x, 0);
    int y0 = MAX(y, 0);
    int x1 = MIN(x + w, fb.width);
    int y1 = MIN(y + h, fb.height);
    if (x1 <= x0 || y1 <= y0) {
        return;
    }

    if (!dirty_valid) {
        dirty.x     = x0;
        dirty.y     = y0;
        dirty.w     = x1 - x0;
        dirty.h     = y1 - y0;
        dirty_valid = true;
        return;
    }

    int dx1 = MAX(dirty.x + dirty.w, x1);
    int dy1 = MAX(dirty.y + dirty.h, y1);
    dirty.x = MIN(dirty.x, x0);
    dirty.y = MIN(dirty.y, y0);
    dirty.w = dx1 - dirty.x;
    dirty.h = dy1 - dirty.y;
}

void fb_mark_all_dirty(void)
{
    dirty.x     = 0;
    dirty.y     = 0;
    dirty.w     = fb.width;
    dirty.h     = fb.height;
    dirty_valid = true;
}

void fb_clear(color_t color)
{
    if (!fb.ready) {
        return;
    }
    memset32(fb.back, pack(color), (size_t)fb.pitch * fb.height);
    fb_mark_all_dirty();
}

void fb_put_pixel(int x, int y, color_t color)
{
    if (!fb.ready) {
        return;
    }
    if (x < clip.x || y < clip.y || x >= clip.x + clip.w || y >= clip.y + clip.h) {
        return;
    }
    fb.back[y * fb.pitch + x] = pack(color);
    fb_mark_dirty(x, y, 1, 1);
}

color_t fb_get_pixel(int x, int y)
{
    if (!fb.ready || x < 0 || y < 0 || x >= fb.width || y >= fb.height) {
        return 0;
    }
    return fb.back[y * fb.pitch + x];
}

/* Clip a rectangle to the active clip region. Returns false if fully hidden. */
static bool_t clip_rect(int *x, int *y, int *w, int *h)
{
    int x0 = MAX(*x, clip.x);
    int y0 = MAX(*y, clip.y);
    int x1 = MIN(*x + *w, clip.x + clip.w);
    int y1 = MIN(*y + *h, clip.y + clip.h);

    if (x1 <= x0 || y1 <= y0) {
        return false;
    }
    *x = x0;
    *y = y0;
    *w = x1 - x0;
    *h = y1 - y0;
    return true;
}

void fb_fill_rect(int x, int y, int w, int h, color_t color)
{
    if (!fb.ready || !clip_rect(&x, &y, &w, &h)) {
        return;
    }

    uint32_t  value = pack(color);
    uint32_t *row   = fb.back + (size_t)y * fb.pitch + x;

    for (int i = 0; i < h; i++) {
        memset32(row, value, (size_t)w);
        row += fb.pitch;
    }
    fb_mark_dirty(x, y, w, h);
}

void fb_draw_rect(int x, int y, int w, int h, color_t color)
{
    if (w <= 0 || h <= 0) {
        return;
    }
    fb_fill_rect(x, y, w, 1, color);
    fb_fill_rect(x, y + h - 1, w, 1, color);
    fb_fill_rect(x, y, 1, h, color);
    fb_fill_rect(x + w - 1, y, 1, h, color);
}

/* Blend `src` over `dst` with the given alpha (0-255). */
static inline uint32_t blend(uint32_t dst, uint32_t src, uint8_t alpha)
{
    if (alpha == 255) {
        return src;
    }
    if (alpha == 0) {
        return dst;
    }

    uint32_t inv = 255u - alpha;
    uint32_t r   = (((src >> 16) & 0xFF) * alpha + ((dst >> 16) & 0xFF) * inv) / 255u;
    uint32_t g   = (((src >> 8) & 0xFF) * alpha + ((dst >> 8) & 0xFF) * inv) / 255u;
    uint32_t b   = ((src & 0xFF) * alpha + (dst & 0xFF) * inv) / 255u;

    return (r << 16) | (g << 8) | b;
}

void fb_fill_rect_alpha(int x, int y, int w, int h, color_t color, uint8_t alpha)
{
    if (!fb.ready || !clip_rect(&x, &y, &w, &h)) {
        return;
    }

    uint32_t  value = pack(color);
    uint32_t *row   = fb.back + (size_t)y * fb.pitch + x;

    for (int i = 0; i < h; i++) {
        for (int j = 0; j < w; j++) {
            row[j] = blend(row[j], value, alpha);
        }
        row += fb.pitch;
    }
    fb_mark_dirty(x, y, w, h);
}

void fb_fill_gradient_v(int x, int y, int w, int h, color_t top, color_t bottom)
{
    if (!fb.ready || h <= 0) {
        return;
    }

    int r0 = (int)COLOR_R(top), g0 = (int)COLOR_G(top), b0 = (int)COLOR_B(top);
    int r1 = (int)COLOR_R(bottom), g1 = (int)COLOR_G(bottom), b1 = (int)COLOR_B(bottom);

    for (int row = 0; row < h; row++) {
        int r = r0 + (r1 - r0) * row / MAX(h - 1, 1);
        int g = g0 + (g1 - g0) * row / MAX(h - 1, 1);
        int b = b0 + (b1 - b0) * row / MAX(h - 1, 1);
        fb_fill_rect(x, y + row, w, 1, RGB(r, g, b));
    }
}

void fb_fill_gradient_h(int x, int y, int w, int h, color_t left, color_t right)
{
    if (!fb.ready || w <= 0) {
        return;
    }

    int r0 = (int)COLOR_R(left), g0 = (int)COLOR_G(left), b0 = (int)COLOR_B(left);
    int r1 = (int)COLOR_R(right), g1 = (int)COLOR_G(right), b1 = (int)COLOR_B(right);

    for (int col = 0; col < w; col++) {
        int r = r0 + (r1 - r0) * col / MAX(w - 1, 1);
        int g = g0 + (g1 - g0) * col / MAX(w - 1, 1);
        int b = b0 + (b1 - b0) * col / MAX(w - 1, 1);
        fb_fill_rect(x + col, y, 1, h, RGB(r, g, b));
    }
}

void fb_fill_round_rect(int x, int y, int w, int h, int radius, color_t color)
{
    if (w <= 0 || h <= 0) {
        return;
    }
    radius = MIN(radius, MIN(w / 2, h / 2));
    if (radius <= 0) {
        fb_fill_rect(x, y, w, h, color);
        return;
    }

    /* Middle band plus the two side bands. */
    fb_fill_rect(x, y + radius, w, h - 2 * radius, color);

    for (int row = 0; row < radius; row++) {
        /* Horizontal extent of the corner arc at this row. */
        int dy     = radius - row;
        int dx     = 0;
        int limit  = radius * radius - dy * dy;
        while ((dx + 1) * (dx + 1) <= limit) {
            dx++;
        }
        int inset = radius - dx;
        fb_fill_rect(x + inset, y + row, w - 2 * inset, 1, color);
        fb_fill_rect(x + inset, y + h - 1 - row, w - 2 * inset, 1, color);
    }
}

void fb_draw_round_rect(int x, int y, int w, int h, int radius, color_t color)
{
    if (w <= 0 || h <= 0) {
        return;
    }
    radius = MIN(radius, MIN(w / 2, h / 2));
    if (radius <= 0) {
        fb_draw_rect(x, y, w, h, color);
        return;
    }

    fb_fill_rect(x + radius, y, w - 2 * radius, 1, color);
    fb_fill_rect(x + radius, y + h - 1, w - 2 * radius, 1, color);
    fb_fill_rect(x, y + radius, 1, h - 2 * radius, color);
    fb_fill_rect(x + w - 1, y + radius, 1, h - 2 * radius, color);

    /* Four corner arcs via the midpoint circle algorithm. */
    int px = radius, py = 0, err = 1 - radius;
    while (px >= py) {
        fb_put_pixel(x + radius - px, y + radius - py, color);
        fb_put_pixel(x + radius - py, y + radius - px, color);
        fb_put_pixel(x + w - radius + px - 1, y + radius - py, color);
        fb_put_pixel(x + w - radius + py - 1, y + radius - px, color);
        fb_put_pixel(x + radius - px, y + h - radius + py - 1, color);
        fb_put_pixel(x + radius - py, y + h - radius + px - 1, color);
        fb_put_pixel(x + w - radius + px - 1, y + h - radius + py - 1, color);
        fb_put_pixel(x + w - radius + py - 1, y + h - radius + px - 1, color);
        py++;
        if (err < 0) {
            err += 2 * py + 1;
        } else {
            px--;
            err += 2 * (py - px) + 1;
        }
    }
}

void fb_draw_line(int x0, int y0, int x1, int y1, color_t color)
{
    int dx  = (x1 > x0) ? x1 - x0 : x0 - x1;
    int dy  = (y1 > y0) ? y1 - y0 : y0 - y1;
    int sx  = (x0 < x1) ? 1 : -1;
    int sy  = (y0 < y1) ? 1 : -1;
    int err = dx - dy;

    for (;;) {
        fb_put_pixel(x0, y0, color);
        if (x0 == x1 && y0 == y1) {
            break;
        }
        int e2 = err * 2;
        if (e2 > -dy) {
            err -= dy;
            x0 += sx;
        }
        if (e2 < dx) {
            err += dx;
            y0 += sy;
        }
    }
}

void fb_draw_circle(int cx, int cy, int radius, color_t color)
{
    int x = radius, y = 0, err = 1 - radius;

    while (x >= y) {
        fb_put_pixel(cx + x, cy + y, color);
        fb_put_pixel(cx + y, cy + x, color);
        fb_put_pixel(cx - y, cy + x, color);
        fb_put_pixel(cx - x, cy + y, color);
        fb_put_pixel(cx - x, cy - y, color);
        fb_put_pixel(cx - y, cy - x, color);
        fb_put_pixel(cx + y, cy - x, color);
        fb_put_pixel(cx + x, cy - y, color);
        y++;
        if (err < 0) {
            err += 2 * y + 1;
        } else {
            x--;
            err += 2 * (y - x) + 1;
        }
    }
}

void fb_fill_circle(int cx, int cy, int radius, color_t color)
{
    for (int dy = -radius; dy <= radius; dy++) {
        int span = 0;
        while ((span + 1) * (span + 1) <= radius * radius - dy * dy) {
            span++;
        }
        fb_fill_rect(cx - span, cy + dy, span * 2 + 1, 1, color);
    }
}

void fb_blit(int x, int y, int w, int h, const uint32_t *src, int src_pitch)
{
    if (!fb.ready) {
        return;
    }

    int ox = x, oy = y, ow = w, oh = h;
    if (!clip_rect(&x, &y, &w, &h)) {
        return;
    }
    UNUSED(ow);
    UNUSED(oh);

    int skip_x = x - ox;
    int skip_y = y - oy;

    for (int row = 0; row < h; row++) {
        memcpy(fb.back + (size_t)(y + row) * fb.pitch + x,
               src + (size_t)(skip_y + row) * src_pitch + skip_x,
               (size_t)w * sizeof(uint32_t));
    }
    fb_mark_dirty(x, y, w, h);
}

void fb_blit_alpha(int x, int y, int w, int h, const uint32_t *src, int src_pitch)
{
    if (!fb.ready) {
        return;
    }

    int ox = x, oy = y;
    if (!clip_rect(&x, &y, &w, &h)) {
        return;
    }

    int skip_x = x - ox;
    int skip_y = y - oy;

    for (int row = 0; row < h; row++) {
        uint32_t       *dst_row = fb.back + (size_t)(y + row) * fb.pitch + x;
        const uint32_t *src_row = src + (size_t)(skip_y + row) * src_pitch + skip_x;

        for (int col = 0; col < w; col++) {
            uint32_t pixel = src_row[col];
            uint8_t  alpha = (uint8_t)(pixel >> 24);
            if (alpha) {
                dst_row[col] = blend(dst_row[col], pixel & 0xFFFFFF, alpha);
            }
        }
    }
    fb_mark_dirty(x, y, w, h);
}

/* --- text ----------------------------------------------------------- */

/*
 * Text is drawn through the font registry rather than a fixed glyph array, so
 * the interface and terminal faces can differ and either can be changed at
 * runtime. The unqualified fb_draw_* helpers use the interface face; the
 * _in variants take an explicit font.
 */

static const uint8_t *glyph_for(const struct font *font, char c)
{
    return font_glyph(font, (uint8_t)c);
}

void fb_draw_char_in(const struct font *font, int x, int y, char c, color_t fg,
                     int scale)
{
    if (!fb.ready || !font) {
        return;
    }
    if (scale < 1) {
        scale = 1;
    }

    const uint8_t *glyph = glyph_for(font, c);
    if (!glyph) {
        return;
    }
    uint32_t value = pack(fg);

    for (int row = 0; row < font->height; row++) {
        uint8_t bits = glyph[row];
        if (!bits) {
            continue;
        }

        for (int sub = 0; sub < scale; sub++) {
            int py = y + row * scale + sub;
            if (py < clip.y || py >= clip.y + clip.h) {
                continue;
            }
            uint32_t *dst = fb.back + (size_t)py * fb.pitch;

            for (int col = 0; col < font->width; col++) {
                if (!(bits & (0x80u >> col))) {
                    continue;
                }
                for (int xsub = 0; xsub < scale; xsub++) {
                    int px = x + col * scale + xsub;
                    if (px < clip.x || px >= clip.x + clip.w) {
                        continue;
                    }
                    dst[px] = value;
                }
            }
        }
    }
    fb_mark_dirty(x, y, font->width * scale, font->height * scale);
}

void fb_draw_string_in(const struct font *font, int x, int y, const char *s,
                       color_t fg, int scale)
{
    if (!font || !s) {
        return;
    }
    if (scale < 1) {
        scale = 1;
    }

    int start = x;
    for (; *s; s++) {
        if (*s == '\n') {
            y += font->height * scale;
            x = start;
            continue;
        }
        fb_draw_char_in(font, x, y, *s, fg, scale);
        x += font->width * scale;
    }
}

int fb_text_width_in(const struct font *font, const char *s, int scale)
{
    return font_text_width(font, s, scale);
}

/* --- interface-face convenience wrappers ---------------------------- */

void fb_draw_char(int x, int y, char c, color_t fg)
{
    fb_draw_char_in(font_ui(), x, y, c, fg, 1);
}

void fb_draw_char_bg(int x, int y, char c, color_t fg, color_t bg)
{
    const struct font *font = font_ui();
    fb_fill_rect(x, y, font->width, font->height, bg);
    fb_draw_char_in(font, x, y, c, fg, 1);
}

void fb_draw_string(int x, int y, const char *s, color_t fg)
{
    fb_draw_string_in(font_ui(), x, y, s, fg, 1);
}

void fb_draw_string_bg(int x, int y, const char *s, color_t fg, color_t bg)
{
    const struct font *font = font_ui();
    fb_fill_rect(x, y, font_text_width(font, s, 1), font->height, bg);
    fb_draw_string_in(font, x, y, s, fg, 1);
}

void fb_draw_string_clipped(int x, int y, const char *s, color_t fg, int max_width)
{
    fb_draw_string_clipped_in(font_ui(), x, y, s, fg, max_width, 1);
}

void fb_draw_string_clipped_in(const struct font *font, int x, int y,
                               const char *s, color_t fg, int max_width,
                               int scale)
{
    if (!font || !s) {
        return;
    }
    if (scale < 1) {
        scale = 1;
    }

    int advance = font->width * scale;
    int drawn   = 0;

    for (; *s && drawn + advance <= max_width; s++) {
        fb_draw_char_in(font, x + drawn, y, *s, fg, scale);
        drawn += advance;
    }

    /* Show an ellipsis when the string had to be truncated. */
    if (*s && max_width >= advance * 2) {
        fb_draw_char_in(font, x + max_width - advance * 2, y, '.', fg, scale);
        fb_draw_char_in(font, x + max_width - advance, y, '.', fg, scale);
    }
}

int fb_text_width(const char *s)
{
    return font_text_width(font_ui(), s, 1);
}

/*
 * Headings are the interface face at double size. Scaling a bitmap face keeps
 * the two visually consistent without shipping a separate large glyph set.
 */
void fb_draw_string_large(int x, int y, const char *s, color_t fg)
{
    fb_draw_string_in(font_ui(), x, y, s, fg, 2);
}

int fb_text_width_large(const char *s)
{
    return font_text_width(font_ui(), s, 2);
}

int fb_font_height(void)
{
    return font_ui()->height;
}

int fb_font_width(void)
{
    return font_ui()->width;
}

/* --- presentation --------------------------------------------------- */

void fb_flush_rect(int x, int y, int w, int h)
{
    if (!fb.ready) {
        return;
    }

    int x0 = MAX(x, 0);
    int y0 = MAX(y, 0);
    int x1 = MIN(x + w, fb.width);
    int y1 = MIN(y + h, fb.height);

    if (x1 <= x0 || y1 <= y0) {
        return;
    }

    size_t bytes = (size_t)(x1 - x0) * sizeof(uint32_t);
    for (int row = y0; row < y1; row++) {
        memcpy(fb.front + (size_t)row * fb.pitch + x0,
               fb.back + (size_t)row * fb.pitch + x0, bytes);
    }
}

void fb_flush(void)
{
    if (!fb.ready || !dirty_valid) {
        return;
    }
    fb_flush_rect(dirty.x, dirty.y, dirty.w, dirty.h);
    dirty_valid = false;
    fb.frames++;
}

/*
 * Dump the back buffer to a file so it can be inspected outside the VM.
 * The format is raw 32-bit pixels, width * height, row by row with no
 * padding, which tools/screenshot.py turns into a PNG.
 */
int fb_screenshot(const char *path)
{
    if (!fb.ready) {
        return -1;
    }

    struct file *file = fs_open(path, O_WRONLY | O_CREATE | O_TRUNC);
    if (!file) {
        return -1;
    }

    /* Write one scanline at a time so pitch padding is stripped. */
    for (int row = 0; row < fb.height; row++) {
        ssize_t written = fs_write(file, fb.back + (size_t)row * fb.pitch,
                                   (size_t)fb.width * sizeof(uint32_t));
        if (written < 0) {
            fs_close(file);
            return -1;
        }
    }

    fs_close(file);
    return 0;
}

/*
 * Blend a single pixel against what is already in the back buffer. Used by
 * the icon renderer, where per-pixel alpha is the whole point.
 */
void fb_blend_pixel(int x, int y, color_t color, uint8_t alpha)
{
    if (!fb.ready) {
        return;
    }
    if (x < clip.x || x >= clip.x + clip.w || y < clip.y || y >= clip.y + clip.h) {
        return;
    }

    uint32_t *dst = fb.back + (size_t)y * fb.pitch + x;
    *dst = blend(*dst, pack(color) & 0xFFFFFF, alpha);
    fb_mark_dirty(x, y, 1, 1);
}
