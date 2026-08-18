/*
 * Qira OS - PS/2 keyboard driver
 *
 * Decodes scancode set 1 (the set the 8042 controller produces by default in
 * translation mode), tracks modifier state, and posts both raw input events
 * and translated characters to the console.
 */

#include <kernel/input.h>
#include <kernel/irq.h>
#include <kernel/io.h>
#include <kernel/log.h>
#include <kernel/console.h>
#include <kernel/time.h>
#include <kernel/string.h>

#define PS2_DATA   0x60
#define PS2_STATUS 0x64
#define PS2_CMD    0x64

#define STATUS_OUTPUT_FULL 0x01
#define STATUS_INPUT_FULL  0x02

/* Scancode set 1: index by make code, value is the unshifted character. */
static const uint16_t scancode_normal[128] = {
    0,        KEY_ESCAPE, '1',  '2',  '3',  '4',  '5',  '6',
    '7',      '8',        '9',  '0',  '-',  '=',  KEY_BACKSPACE, KEY_TAB,
    'q',      'w',        'e',  'r',  't',  'y',  'u',  'i',
    'o',      'p',        '[',  ']',  KEY_ENTER, 0 /* LCtrl */, 'a', 's',
    'd',      'f',        'g',  'h',  'j',  'k',  'l',  ';',
    '\'',     '`',        0 /* LShift */, '\\', 'z', 'x', 'c', 'v',
    'b',      'n',        'm',  ',',  '.',  '/',  0 /* RShift */, '*',
    0 /*LAlt*/, ' ',      0 /* Caps */, KEY_F1, KEY_F2, KEY_F3, KEY_F4, KEY_F5,
    KEY_F6,   KEY_F7,     KEY_F8, KEY_F9, KEY_F10, 0 /* Num */, 0 /* Scroll */, KEY_HOME,
    KEY_UP,   KEY_PAGEUP, '-',  KEY_LEFT, '5', KEY_RIGHT, '+', KEY_END,
    KEY_DOWN, KEY_PAGEDOWN, KEY_INSERT, KEY_DELETE, 0, 0, 0, KEY_F11,
    KEY_F12,  0, 0, 0, 0, 0, 0, 0,
};

/* Characters produced when Shift is held. */
static const uint16_t scancode_shifted[128] = {
    0,        KEY_ESCAPE, '!',  '@',  '#',  '$',  '%',  '^',
    '&',      '*',        '(',  ')',  '_',  '+',  KEY_BACKSPACE, KEY_TAB,
    'Q',      'W',        'E',  'R',  'T',  'Y',  'U',  'I',
    'O',      'P',        '{',  '}',  KEY_ENTER, 0, 'A', 'S',
    'D',      'F',        'G',  'H',  'J',  'K',  'L',  ':',
    '"',      '~',        0,    '|',  'Z',  'X',  'C',  'V',
    'B',      'N',        'M',  '<',  '>',  '?',  0,    '*',
    0,        ' ',        0,    KEY_F1, KEY_F2, KEY_F3, KEY_F4, KEY_F5,
    KEY_F6,   KEY_F7,     KEY_F8, KEY_F9, KEY_F10, 0, 0, KEY_HOME,
    KEY_UP,   KEY_PAGEUP, '-',  KEY_LEFT, '5', KEY_RIGHT, '+', KEY_END,
    KEY_DOWN, KEY_PAGEDOWN, KEY_INSERT, KEY_DELETE, 0, 0, 0, KEY_F11,
    KEY_F12,  0, 0, 0, 0, 0, 0, 0,
};

static uint32_t modifiers;
static bool_t   extended;   /* the previous byte was the 0xE0 prefix */
static uint64_t key_events;

uint64_t keyboard_key_count(void)
{
    return key_events;
}

/* Wait until the controller can accept a command byte. */
static void ps2_wait_write(void)
{
    for (int i = 0; i < 100000; i++) {
        if (!(inb(PS2_STATUS) & STATUS_INPUT_FULL)) {
            return;
        }
    }
}

static void handle_scancode(uint8_t code)
{
    /* 0xE0 introduces an extended (grey key) sequence. */
    if (code == 0xE0) {
        extended = true;
        return;
    }

    bool_t released = (code & 0x80) != 0;
    uint8_t make    = code & 0x7F;

    /* Modifier keys update state rather than producing characters. */
    uint32_t modifier_bit = 0;
    switch (make) {
    case 0x2A:   /* left shift  */
    case 0x36:   /* right shift */
        modifier_bit = MOD_SHIFT;
        break;
    case 0x1D:   /* control     */
        modifier_bit = MOD_CTRL;
        break;
    case 0x38:   /* alt         */
        modifier_bit = MOD_ALT;
        break;
    case 0x5B:   /* left super  */
    case 0x5C:
        modifier_bit = MOD_SUPER;
        break;
    case 0x3A:   /* caps lock, toggles on press */
        if (!released) {
            modifiers ^= MOD_CAPSLOCK;
        }
        extended = false;
        return;
    case 0x45:   /* num lock */
        if (!released) {
            modifiers ^= MOD_NUMLOCK;
        }
        extended = false;
        return;
    default:
        break;
    }

    if (modifier_bit) {
        if (released) {
            modifiers &= ~modifier_bit;
        } else {
            modifiers |= modifier_bit;
        }
        extended = false;
        return;
    }

    bool_t shifted = (modifiers & MOD_SHIFT) != 0;
    uint16_t key   = shifted ? scancode_shifted[make] : scancode_normal[make];

    /* The extended set remaps the numeric keypad to navigation keys. */
    if (extended) {
        switch (make) {
        case 0x48: key = KEY_UP;       break;
        case 0x50: key = KEY_DOWN;     break;
        case 0x4B: key = KEY_LEFT;     break;
        case 0x4D: key = KEY_RIGHT;    break;
        case 0x47: key = KEY_HOME;     break;
        case 0x4F: key = KEY_END;      break;
        case 0x49: key = KEY_PAGEUP;   break;
        case 0x51: key = KEY_PAGEDOWN; break;
        case 0x52: key = KEY_INSERT;   break;
        case 0x53: key = KEY_DELETE;   break;
        case 0x1C: key = KEY_ENTER;    break;
        case 0x35: key = '/';          break;
        default:   break;
        }
    }
    extended = false;

    if (key == 0) {
        return;
    }

    /* Caps lock affects letters only. */
    if ((modifiers & MOD_CAPSLOCK) && key >= 'a' && key <= 'z') {
        key = (uint16_t)(key - 'a' + 'A');
    } else if ((modifiers & MOD_CAPSLOCK) && shifted && key >= 'A' && key <= 'Z') {
        key = (uint16_t)(key - 'A' + 'a');
    }

    struct input_event event;
    memset(&event, 0, sizeof(event));
    event.type      = released ? INPUT_KEY_UP : INPUT_KEY_DOWN;
    event.code      = key;
    event.modifiers = modifiers;
    event.timestamp = time_uptime_ms();
    input_post(&event);

    if (released) {
        return;
    }
    key_events++;

    /* Feed printable characters and the common control keys to the console. */
    if (modifiers & MOD_CTRL) {
        if (key >= 'a' && key <= 'z') {
            console_push_char(key - 'a' + 1);   /* Ctrl+A -> 0x01 */
        } else if (key >= 'A' && key <= 'Z') {
            console_push_char(key - 'A' + 1);
        }
        return;
    }
    if (key < 0x100) {
        console_push_char((int)key);
    } else {
        console_push_char((int)key);
    }
}

static void keyboard_irq(struct interrupt_frame *frame, void *context)
{
    UNUSED(frame);
    UNUSED(context);

    /* Drain the controller: several scancodes may be pending. */
    int guard = 0;
    while ((inb(PS2_STATUS) & STATUS_OUTPUT_FULL) && guard++ < 32) {
        /* Bit 5 set means the byte came from the mouse, not the keyboard. */
        if (inb(PS2_STATUS) & 0x20) {
            break;
        }
        handle_scancode(inb(PS2_DATA));
    }
}

void keyboard_init(void)
{
    modifiers = 0;
    extended  = false;

    /* Drain anything the firmware left in the output buffer. */
    for (int i = 0; i < 32 && (inb(PS2_STATUS) & STATUS_OUTPUT_FULL); i++) {
        inb(PS2_DATA);
    }

    /* Enable the first PS/2 port. */
    ps2_wait_write();
    outb(PS2_CMD, 0xAE);

    irq_register(IRQ_KEYBOARD, keyboard_irq, NULL, "ps2-keyboard");

    KLOG_INFO("keyboard", "PS/2 keyboard ready (scancode set 1)");
}
