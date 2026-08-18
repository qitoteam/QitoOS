/*
 * Qira OS - framebuffer capture over the serial port
 *
 * Emulators running headless give no way to look at the screen, which makes
 * graphical regressions invisible to automated tests. This module streams the
 * composited back buffer out of COM1 so tools/grabframe.py can rebuild a PNG
 * from a captured serial log.
 *
 * A raw 1024x768 frame is 2.3 MB, which takes minutes over a 115200 baud line.
 * Desktop imagery is mostly flat colour, so the pixel stream is run-length
 * encoded before base64 wrapping, which typically shrinks a frame by one to
 * two orders of magnitude and makes capture practical inside a test run.
 *
 * Wire format, after base64 decoding:
 *
 *     repeat:
 *       uint8  count   1-255, how many times the pixel repeats
 *       uint8  red
 *       uint8  green
 *       uint8  blue
 *
 * Pixels are in raster order, `width * height` of them in total.
 *
 * Capture is triggered by pressing F12, or automatically by passing
 * `capture=<milliseconds>` on the kernel command line.
 */

#include <kernel/fb.h>
#include <kernel/serial.h>
#include <kernel/log.h>
#include <kernel/string.h>
#include <kernel/printf.h>
#include <kernel/time.h>

#define CAPTURE_BEGIN "\n--QIRA-FRAME-BEGIN "
#define CAPTURE_END   "\n--QIRA-FRAME-END--\n"

static const char base64_alphabet[] =
    "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";

static uint64_t captures_taken;

/* Incremental base64 encoder, so the frame never needs buffering in full. */
struct b64_state {
    uint8_t  pending[3];
    int      pending_len;
    int      column;
    uint64_t emitted;
};

static void b64_flush_triple(struct b64_state *state)
{
    uint32_t triple = ((uint32_t)state->pending[0] << 16) |
                      ((uint32_t)state->pending[1] << 8) | state->pending[2];

    serial_putc(base64_alphabet[(triple >> 18) & 0x3F]);
    serial_putc(base64_alphabet[(triple >> 12) & 0x3F]);
    serial_putc(base64_alphabet[(triple >> 6) & 0x3F]);
    serial_putc(base64_alphabet[triple & 0x3F]);

    state->emitted += 4;
    state->column += 4;
    if (state->column >= 76) {
        serial_putc('\n');
        state->column = 0;
    }
    state->pending_len = 0;
}

static void b64_push(struct b64_state *state, uint8_t byte)
{
    state->pending[state->pending_len++] = byte;
    if (state->pending_len == 3) {
        b64_flush_triple(state);
    }
}

static void b64_finish(struct b64_state *state)
{
    if (state->pending_len == 0) {
        return;
    }

    uint32_t triple = (uint32_t)state->pending[0] << 16;
    if (state->pending_len == 2) {
        triple |= (uint32_t)state->pending[1] << 8;
    }

    serial_putc(base64_alphabet[(triple >> 18) & 0x3F]);
    serial_putc(base64_alphabet[(triple >> 12) & 0x3F]);
    serial_putc(state->pending_len == 2 ? base64_alphabet[(triple >> 6) & 0x3F]
                                        : '=');
    serial_putc('=');
    state->emitted += 4;
    state->pending_len = 0;
}

/* Emit one run-length record. */
static void emit_run(struct b64_state *state, uint8_t count, uint32_t pixel)
{
    b64_push(state, count);
    b64_push(state, (uint8_t)((pixel >> 16) & 0xFF));
    b64_push(state, (uint8_t)((pixel >> 8) & 0xFF));
    b64_push(state, (uint8_t)(pixel & 0xFF));
}

void fb_capture_serial(const char *label)
{
    if (!fb_available()) {
        KLOG_WARN("capture", "no framebuffer to capture");
        return;
    }

    const struct fb_info *info = fb_get_info();

    char header[160];
    snprintf(header, sizeof(header), "%s%dx%d rle24 %s\n", CAPTURE_BEGIN,
             info->width, info->height, label ? label : "frame");
    serial_write(header);

    struct b64_state state;
    memset(&state, 0, sizeof(state));

    uint64_t runs  = 0;
    uint64_t start = time_uptime_ms();

    /*
     * Walk the whole image as one raster sequence so runs continue across
     * scanline boundaries, which matters for flat backgrounds.
     */
    uint32_t current = 0;
    uint32_t count   = 0;
    bool_t   started = false;

    for (int y = 0; y < info->height; y++) {
        const uint32_t *row = info->back + (size_t)y * info->pitch;

        for (int x = 0; x < info->width; x++) {
            uint32_t pixel = row[x] & 0x00FFFFFFu;

            if (!started) {
                current = pixel;
                count   = 1;
                started = true;
                continue;
            }
            if (pixel == current && count < 255) {
                count++;
                continue;
            }

            emit_run(&state, (uint8_t)count, current);
            runs++;
            current = pixel;
            count   = 1;
        }
    }

    if (started) {
        emit_run(&state, (uint8_t)count, current);
        runs++;
    }

    b64_finish(&state);
    serial_write(CAPTURE_END);

    captures_taken++;

    uint64_t elapsed = time_uptime_ms() - start;
    uint64_t pixels  = (uint64_t)info->width * info->height;

    KLOG_INFO("capture", "frame %llu: %dx%d, %llu runs, %llu base64 bytes in %llu ms",
              (unsigned long long)captures_taken, info->width, info->height,
              (unsigned long long)runs, (unsigned long long)state.emitted,
              (unsigned long long)elapsed);
    KLOG_INFO("capture", "compression: %llu pixels -> %llu runs (%llux)",
              (unsigned long long)pixels, (unsigned long long)runs,
              (unsigned long long)(runs ? pixels / runs : 0));
}

uint64_t fb_capture_count(void)
{
    return captures_taken;
}
