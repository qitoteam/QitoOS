/*
 * Qira OS - PS/2 mouse driver
 *
 * Initialises the auxiliary port of the 8042 controller, enables the
 * IntelliMouse scroll-wheel protocol when the device supports it, and turns
 * the 3 or 4 byte movement packets into absolute cursor positions.
 */

#include <kernel/input.h>
#include <kernel/irq.h>
#include <kernel/io.h>
#include <kernel/log.h>
#include <kernel/fb.h>
#include <kernel/time.h>
#include <kernel/string.h>

#define PS2_DATA   0x60
#define PS2_STATUS 0x64
#define PS2_CMD    0x64

#define STATUS_OUTPUT_FULL 0x01
#define STATUS_INPUT_FULL  0x02
#define STATUS_FROM_MOUSE  0x20

#define MOUSE_ACK 0xFA

static uint8_t  packet[4];
static int      packet_index;
static int      packet_size = 3;   /* 4 once the scroll wheel is enabled */
static bool_t   present;
static uint64_t packets_seen;

static int      cursor_x, cursor_y;
static uint32_t buttons;

uint64_t mouse_packet_count(void)
{
    return packets_seen;
}

bool_t mouse_available(void)
{
    return present;
}

static bool_t wait_write(void)
{
    for (int i = 0; i < 200000; i++) {
        if (!(inb(PS2_STATUS) & STATUS_INPUT_FULL)) {
            return true;
        }
    }
    return false;
}

static bool_t wait_read(void)
{
    for (int i = 0; i < 200000; i++) {
        if (inb(PS2_STATUS) & STATUS_OUTPUT_FULL) {
            return true;
        }
    }
    return false;
}

/* Send a byte to the mouse (port 2) and read back its acknowledgement. */
static bool_t mouse_command(uint8_t command)
{
    if (!wait_write()) {
        return false;
    }
    outb(PS2_CMD, 0xD4);        /* address the auxiliary device */
    if (!wait_write()) {
        return false;
    }
    outb(PS2_DATA, command);
    if (!wait_read()) {
        return false;
    }
    return inb(PS2_DATA) == MOUSE_ACK;
}

static uint8_t mouse_read(void)
{
    wait_read();
    return inb(PS2_DATA);
}

/*
 * Attempt the "magic knock" that switches an IntelliMouse-compatible device
 * into the 4-byte protocol with a scroll wheel.
 */
static bool_t enable_scroll_wheel(void)
{
    mouse_command(0xF3);
    mouse_command(200);
    mouse_command(0xF3);
    mouse_command(100);
    mouse_command(0xF3);
    mouse_command(80);

    if (!mouse_command(0xF2)) {   /* read device id */
        return false;
    }
    return mouse_read() == 0x03;
}

static void process_packet(void)
{
    uint8_t flags = packet[0];

    /* Bit 3 must always be set in a valid first byte. */
    if (!(flags & 0x08)) {
        packet_index = 0;
        return;
    }
    /* Discard packets that report an overflow. */
    if (flags & 0xC0) {
        return;
    }

    int dx = packet[1];
    int dy = packet[2];

    /* Sign-extend using the sign bits in the flags byte. */
    if (flags & 0x10) {
        dx |= ~0xFF;
    }
    if (flags & 0x20) {
        dy |= ~0xFF;
    }

    /* The mouse reports Y growing upward; the screen grows downward. */
    cursor_x += dx;
    cursor_y -= dy;

    int max_x = fb_available() ? fb_width() - 1 : 1023;
    int max_y = fb_available() ? fb_height() - 1 : 767;
    cursor_x  = CLAMP(cursor_x, 0, max_x);
    cursor_y  = CLAMP(cursor_y, 0, max_y);

    uint32_t new_buttons = 0;
    if (flags & 0x01) {
        new_buttons |= MOUSE_LEFT;
    }
    if (flags & 0x02) {
        new_buttons |= MOUSE_RIGHT;
    }
    if (flags & 0x04) {
        new_buttons |= MOUSE_MIDDLE;
    }

    struct input_event event;
    memset(&event, 0, sizeof(event));
    event.modifiers = input_modifiers();
    event.timestamp = time_uptime_ms();
    event.x         = cursor_x;
    event.y         = cursor_y;
    event.dx        = dx;
    event.dy        = -dy;

    /* Movement first, so a click always carries an up-to-date position. */
    if (dx || dy) {
        event.type = INPUT_MOUSE_MOVE;
        event.code = new_buttons;
        input_post(&event);
    }

    uint32_t pressed  = new_buttons & ~buttons;
    uint32_t released = buttons & ~new_buttons;

    if (pressed) {
        event.type = INPUT_MOUSE_DOWN;
        event.code = new_buttons;
        event.dx   = (int32_t)pressed;
        input_post(&event);
    }
    if (released) {
        event.type = INPUT_MOUSE_UP;
        event.code = new_buttons;
        event.dx   = (int32_t)released;
        input_post(&event);
    }

    /* Byte 4 carries the scroll wheel delta as a signed 4-bit value. */
    if (packet_size == 4) {
        int8_t wheel = (int8_t)(packet[3] & 0x0F);
        if (wheel & 0x08) {
            wheel |= (int8_t)0xF0;
        }
        if (wheel) {
            event.type = INPUT_MOUSE_SCROLL;
            event.code = new_buttons;
            event.dy   = -wheel;
            event.dx   = 0;
            input_post(&event);
        }
    }

    buttons = new_buttons;
    packets_seen++;
}

static void mouse_irq(struct interrupt_frame *frame, void *context)
{
    UNUSED(frame);
    UNUSED(context);

    int guard = 0;
    while ((inb(PS2_STATUS) & STATUS_OUTPUT_FULL) && guard++ < 16) {
        if (!(inb(PS2_STATUS) & STATUS_FROM_MOUSE)) {
            break;   /* keyboard data, leave it for the keyboard handler */
        }

        uint8_t byte = inb(PS2_DATA);

        /* Resynchronise if the first byte looks invalid. */
        if (packet_index == 0 && !(byte & 0x08)) {
            continue;
        }

        packet[packet_index++] = byte;
        if (packet_index >= packet_size) {
            process_packet();
            packet_index = 0;
        }
    }
}

void mouse_init(void)
{
    cursor_x = fb_available() ? fb_width() / 2 : 400;
    cursor_y = fb_available() ? fb_height() / 2 : 300;
    buttons  = 0;
    packet_index = 0;

    /* Enable the auxiliary (mouse) port. */
    if (!wait_write()) {
        KLOG_WARN("mouse", "8042 controller is not responding");
        return;
    }
    outb(PS2_CMD, 0xA8);

    /* Read the controller configuration byte, enable IRQ 12, write it back. */
    if (!wait_write()) {
        return;
    }
    outb(PS2_CMD, 0x20);
    if (!wait_read()) {
        return;
    }
    uint8_t config = inb(PS2_DATA);
    config |= 0x02;    /* enable the auxiliary device interrupt */
    config &= ~0x20;   /* clear the auxiliary clock disable bit */

    if (!wait_write()) {
        return;
    }
    outb(PS2_CMD, 0x60);
    if (!wait_write()) {
        return;
    }
    outb(PS2_DATA, config);

    /* Reset to defaults, then start streaming. */
    if (!mouse_command(0xF6)) {
        KLOG_WARN("mouse", "no PS/2 mouse detected");
        return;
    }

    if (enable_scroll_wheel()) {
        packet_size = 4;
        KLOG_INFO("mouse", "IntelliMouse scroll wheel enabled");
    } else {
        packet_size = 3;
    }

    if (!mouse_command(0xF4)) {   /* enable data reporting */
        KLOG_WARN("mouse", "mouse refused to start streaming");
        return;
    }

    present = true;
    input_set_mouse(cursor_x, cursor_y);
    irq_register(IRQ_MOUSE, mouse_irq, NULL, "ps2-mouse");

    KLOG_INFO("mouse", "PS/2 mouse ready (%d byte packets)", packet_size);
}
