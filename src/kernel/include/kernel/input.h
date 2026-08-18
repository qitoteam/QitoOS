/*
 * Qira OS - input subsystem
 *
 * The PS/2 keyboard and mouse drivers translate hardware scancodes into a
 * unified event queue that the desktop and the console both read from.
 */
#ifndef QIRA_INPUT_H
#define QIRA_INPUT_H

#include <kernel/types.h>

typedef enum {
    INPUT_KEY_DOWN = 1,
    INPUT_KEY_UP,
    INPUT_MOUSE_MOVE,
    INPUT_MOUSE_DOWN,
    INPUT_MOUSE_UP,
    INPUT_MOUSE_SCROLL,
} input_event_type_t;

/* Modifier bitmask. */
#define MOD_SHIFT    0x01
#define MOD_CTRL     0x02
#define MOD_ALT      0x04
#define MOD_SUPER    0x08
#define MOD_CAPSLOCK 0x10
#define MOD_NUMLOCK  0x20

/* Non-printable keys are reported with these codes. */
#define KEY_ESCAPE    0x1B
#define KEY_BACKSPACE 0x08
#define KEY_TAB       0x09
#define KEY_ENTER     0x0A
#define KEY_UP        0x100
#define KEY_DOWN      0x101
#define KEY_LEFT      0x102
#define KEY_RIGHT     0x103
#define KEY_HOME      0x104
#define KEY_END       0x105
#define KEY_PAGEUP    0x106
#define KEY_PAGEDOWN  0x107
#define KEY_INSERT    0x108
#define KEY_DELETE    0x109
#define KEY_F1        0x110
#define KEY_F2        0x111
#define KEY_F3        0x112
#define KEY_F4        0x113
#define KEY_F5        0x114
#define KEY_F6        0x115
#define KEY_F7        0x116
#define KEY_F8        0x117
#define KEY_F9        0x118
#define KEY_F10       0x119
#define KEY_F11       0x11A
#define KEY_F12       0x11B

#define MOUSE_LEFT   0x01
#define MOUSE_RIGHT  0x02
#define MOUSE_MIDDLE 0x04

struct input_event {
    input_event_type_t type;
    uint32_t           code;      /* key code or mouse button mask       */
    uint32_t           modifiers;
    int32_t            x, y;      /* absolute cursor position            */
    int32_t            dx, dy;    /* relative motion / scroll delta      */
    uint64_t           timestamp; /* milliseconds since boot             */
};

void input_init(void);

/* Producers (called by the drivers). */
void input_post(const struct input_event *event);

/* Consumers. */
bool_t input_poll(struct input_event *out);
bool_t input_has_events(void);
void   input_flush(void);

/* Current pointer state. */
void input_get_mouse(int *x, int *y, uint32_t *buttons);
void input_set_mouse(int x, int y);
uint32_t input_modifiers(void);

/* Drivers. */
void keyboard_init(void);
void mouse_init(void);
bool_t mouse_available(void);

/* Statistics. */
uint64_t input_event_count(void);
uint64_t keyboard_key_count(void);
uint64_t mouse_packet_count(void);

#endif /* QIRA_INPUT_H */
