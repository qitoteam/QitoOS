/*
 * Qira OS - input event queue
 *
 * A lock-free-ish ring buffer between the interrupt-driven drivers and the
 * desktop/console consumers. The queue is small and drops the oldest event on
 * overflow, which is the right behaviour for interactive input.
 */

#include <kernel/input.h>
#include <kernel/fb.h>
#include <kernel/time.h>
#include <kernel/log.h>
#include <kernel/string.h>
#include <kernel/spinlock.h>

#define EVENT_QUEUE_SIZE 256

static struct input_event queue[EVENT_QUEUE_SIZE];
static volatile int       queue_head;
static volatile int       queue_tail;
static spinlock_t         input_lock;

static int      mouse_x, mouse_y;
static uint32_t mouse_buttons;
static uint32_t modifier_state;

static uint64_t events_posted;

void input_init(void)
{
    spinlock_init(&input_lock, "input");
    queue_head = queue_tail = 0;

    mouse_x = fb_available() ? fb_width() / 2 : 400;
    mouse_y = fb_available() ? fb_height() / 2 : 300;
    mouse_buttons  = 0;
    modifier_state = 0;

    KLOG_INFO("input", "event queue ready (%d slots)", EVENT_QUEUE_SIZE);
}

void input_post(const struct input_event *event)
{
    bool_t irq  = spinlock_acquire(&input_lock);
    int    next = (queue_head + 1) % EVENT_QUEUE_SIZE;

    if (next == queue_tail) {
        /* Full: discard the oldest event so the newest always lands. */
        queue_tail = (queue_tail + 1) % EVENT_QUEUE_SIZE;
    }

    queue[queue_head] = *event;
    queue_head        = next;
    events_posted++;

    /* Track the persistent state the drivers report. */
    if (event->type == INPUT_MOUSE_MOVE || event->type == INPUT_MOUSE_DOWN ||
        event->type == INPUT_MOUSE_UP) {
        mouse_x       = event->x;
        mouse_y       = event->y;
        mouse_buttons = event->code;
    }
    modifier_state = event->modifiers;

    spinlock_release(&input_lock, irq);
}

bool_t input_poll(struct input_event *out)
{
    bool_t irq = spinlock_acquire(&input_lock);

    if (queue_tail == queue_head) {
        spinlock_release(&input_lock, irq);
        return false;
    }

    *out       = queue[queue_tail];
    queue_tail = (queue_tail + 1) % EVENT_QUEUE_SIZE;

    spinlock_release(&input_lock, irq);
    return true;
}

bool_t input_has_events(void)
{
    return queue_head != queue_tail;
}

void input_flush(void)
{
    bool_t irq = spinlock_acquire(&input_lock);
    queue_tail = queue_head;
    spinlock_release(&input_lock, irq);
}

void input_get_mouse(int *x, int *y, uint32_t *buttons)
{
    if (x) {
        *x = mouse_x;
    }
    if (y) {
        *y = mouse_y;
    }
    if (buttons) {
        *buttons = mouse_buttons;
    }
}

void input_set_mouse(int x, int y)
{
    mouse_x = x;
    mouse_y = y;
}

uint32_t input_modifiers(void)
{
    return modifier_state;
}

uint64_t input_event_count(void)
{
    return events_posted;
}
