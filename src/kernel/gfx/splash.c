/*
 * QitoOS - boot splash and loading screen
 *
 * Shown from the moment the framebuffer is up until the desktop takes over.
 * Besides looking finished, it is genuinely useful: the stage text is the
 * only feedback a user gets on a machine where a driver hangs, and it makes
 * the difference between "it froze" and "it froze bringing up PCI".
 */

#include <kernel/fb.h>
#include <kernel/font.h>
#include <kernel/string.h>
#include <kernel/printf.h>
#include <kernel/version.h>
#include <kernel/time.h>
#include <kernel/splash.h>
#include <kernel/qac.h>

static int    current_percent;
static char   current_stage[64];
static bool_t active;
static uint64_t started_ms;

/* The palette matches the desktop's default Aurora theme. */
#define SPLASH_TOP     RGB(16, 19, 34)
#define SPLASH_BOTTOM  RGB(38, 26, 56)
#define SPLASH_ACCENT  RGB(110, 170, 255)
#define SPLASH_TEXT    RGB(226, 232, 245)
#define SPLASH_DIM     RGB(120, 132, 162)
#define SPLASH_TRACK   RGB(44, 50, 72)

/*
 * The wordmark, drawn as vector-ish strokes rather than text so it does not
 * depend on the font being loaded yet, and so it looks deliberate at size.
 */
static void draw_wordmark(int cx, int cy)
{
    /*
     * A ring with a tail: the Q of Qito. Drawn from arcs so it scales
     * cleanly and costs nothing to store.
     */
    int radius = 34;

    /* Outer ring, thick. */
    for (int thickness = 0; thickness < 5; thickness++) {
        fb_draw_circle(cx, cy, radius - thickness, SPLASH_ACCENT);
    }

    /* Inner counter, punched out. */
    fb_fill_circle(cx, cy, radius - 12, SPLASH_TOP);

    /* The tail, running out to the lower right. */
    for (int offset = 0; offset < 5; offset++) {
        fb_draw_line(cx + 12 + offset, cy + 12, cx + 30 + offset, cy + 30,
                     SPLASH_ACCENT);
    }

    /* A soft highlight on the upper left of the ring. */
    for (int i = 0; i < 3; i++) {
        fb_draw_line(cx - 24 + i, cy - 16, cx - 16 + i, cy - 26,
                     RGB(170, 205, 255));
    }
}

void splash_begin(void)
{
    if (!fb_available()) {
        return;
    }
    active     = true;
    started_ms = time_uptime_ms();
    current_percent = 0;
    strlcpy(current_stage, "Starting", sizeof(current_stage));
}

void splash_update(const char *stage, int percent)
{
    if (!active || !fb_available()) {
        return;
    }

    if (stage) {
        strlcpy(current_stage, stage, sizeof(current_stage));
    }
    if (percent >= 0) {
        current_percent = CLAMP(percent, 0, 100);
    }

    int width  = fb_width();
    int height = fb_height();

    fb_fill_gradient_v(0, 0, width, height, SPLASH_TOP, SPLASH_BOTTOM);

    /* A faint vignette so the centre reads brighter than the edges. */
    for (int i = 0; i < 60; i++) {
        uint8_t alpha = (uint8_t)(40 - i * 40 / 60);
        if (!alpha) {
            break;
        }
        fb_fill_rect_alpha(0, i, width, 1, RGB(0, 0, 0), alpha);
        fb_fill_rect_alpha(0, height - i - 1, width, 1, RGB(0, 0, 0), alpha);
    }

    int centre_x = width / 2;
    int centre_y = height / 2;

    draw_wordmark(centre_x, centre_y - 96);

    /* Product name. */
    const char *title = "QitoOS";
    fb_draw_string_large(centre_x - fb_text_width_large(title) / 2, centre_y - 24,
                         title, SPLASH_TEXT);

    char subtitle[80];
    snprintf(subtitle, sizeof(subtitle), "version %s  \"%s\"", QITO_VERSION_STRING,
             QITO_CODENAME);
    fb_draw_string(centre_x - fb_text_width(subtitle) / 2, centre_y + 14, subtitle,
                   SPLASH_DIM);

    /* Progress track and fill. */
    int bar_width = MIN(460, width - 140);
    int bar_x     = centre_x - bar_width / 2;
    int bar_y     = centre_y + 62;
    int bar_h     = 8;

    fb_fill_round_rect(bar_x, bar_y, bar_width, bar_h, bar_h / 2, SPLASH_TRACK);

    int filled = bar_width * current_percent / 100;
    if (filled > 0) {
        fb_fill_round_rect(bar_x, bar_y, filled, bar_h, bar_h / 2, SPLASH_ACCENT);

        /* A brighter leading edge, which reads as motion. */
        if (filled > 8) {
            fb_fill_round_rect(bar_x + filled - 8, bar_y, 8, bar_h, bar_h / 2,
                               RGB(180, 214, 255));
        }
    }

    /* Stage text under the bar, with the percentage on the right. */
    fb_draw_string(bar_x, bar_y + 20, current_stage, SPLASH_TEXT);

    char percent_text[8];
    snprintf(percent_text, sizeof(percent_text), "%d%%", current_percent);
    fb_draw_string(bar_x + bar_width - fb_text_width(percent_text), bar_y + 20,
                   percent_text, SPLASH_DIM);

    /* Footer. */
    const char *footer = QITO_MAINTAINER "  -  " QITO_PROJECT_URL;
    fb_draw_string(centre_x - fb_text_width(footer) / 2, height - 34, footer,
                   RGB(86, 96, 124));

    fb_flush();
}

void splash_end(void)
{
    if (!active) {
        return;
    }
    active = false;
}

bool_t splash_active(void)
{
    return active;
}

uint64_t splash_elapsed_ms(void)
{
    return started_ms ? time_uptime_ms() - started_ms : 0;
}

/*
 * Shown when startup fails badly enough that the desktop will not appear.
 * A user who sees this at least learns which stage died.
 */
void splash_fail(const char *stage, const char *detail)
{
    if (!fb_available()) {
        return;
    }

    int width  = fb_width();
    int height = fb_height();

    fb_fill_gradient_v(0, 0, width, height, RGB(40, 14, 18), RGB(20, 8, 12));

    int centre_x = width / 2;
    int centre_y = height / 2;

    const char *title = "Startup failed";
    fb_draw_string_large(centre_x - fb_text_width_large(title) / 2, centre_y - 40,
                         title, RGB(255, 150, 140));

    if (stage) {
        char line[96];
        snprintf(line, sizeof(line), "while %s", stage);
        fb_draw_string(centre_x - fb_text_width(line) / 2, centre_y + 6, line,
                       RGB(240, 210, 210));
    }
    if (detail) {
        fb_draw_string(centre_x - fb_text_width(detail) / 2, centre_y + 28, detail,
                       RGB(200, 170, 170));
    }

    const char *hint = "The kernel log on the serial port has the full detail.";
    fb_draw_string(centre_x - fb_text_width(hint) / 2, centre_y + 62, hint,
                   RGB(150, 120, 120));

    fb_flush();
}
