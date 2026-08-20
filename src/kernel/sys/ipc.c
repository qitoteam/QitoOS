/*
 * QitoOS - inter-process communication
 *
 * Named message ports with bounded queues, and byte-stream pipes. Both are
 * safe to use from interrupt context for the non-blocking operations.
 */

#include <kernel/ipc.h>
#include <kernel/mm.h>
#include <kernel/string.h>
#include <kernel/log.h>
#include <kernel/sched.h>
#include <kernel/time.h>
#include <kernel/spinlock.h>
#include <kernel/syscall.h>

struct ipc_port {
    char   name[IPC_PORT_NAME_MAX];
    bool_t in_use;
    int    owner_pid;

    struct ipc_message queue[IPC_QUEUE_DEPTH];
    int    head;
    int    tail;
    int    count;
};

static struct ipc_port ports[IPC_MAX_PORTS];
static spinlock_t      ipc_lock;
static uint64_t        messages_sent;
static uint64_t        messages_dropped;

void ipc_init(void)
{
    spinlock_init(&ipc_lock, "ipc");
    memset(ports, 0, sizeof(ports));
    messages_sent    = 0;
    messages_dropped = 0;

    KLOG_INFO("ipc", "message ports ready (%d ports, %d messages each)",
              IPC_MAX_PORTS, IPC_QUEUE_DEPTH);
}

int ipc_port_create(const char *name)
{
    bool_t irq = spinlock_acquire(&ipc_lock);

    /* Reject duplicates. */
    for (int i = 0; i < IPC_MAX_PORTS; i++) {
        if (ports[i].in_use && strcmp(ports[i].name, name) == 0) {
            spinlock_release(&ipc_lock, irq);
            return -QE_EXIST;
        }
    }

    for (int i = 0; i < IPC_MAX_PORTS; i++) {
        if (ports[i].in_use) {
            continue;
        }
        memset(&ports[i], 0, sizeof(ports[i]));
        strlcpy(ports[i].name, name, sizeof(ports[i].name));
        ports[i].in_use    = true;
        ports[i].owner_pid = sched_current_pid();

        spinlock_release(&ipc_lock, irq);
        KLOG_DEBUG("ipc", "port '%s' created as %d", name, i);
        return i;
    }

    spinlock_release(&ipc_lock, irq);
    return -QE_NFILE;
}

int ipc_port_lookup(const char *name)
{
    for (int i = 0; i < IPC_MAX_PORTS; i++) {
        if (ports[i].in_use && strcmp(ports[i].name, name) == 0) {
            return i;
        }
    }
    return -QE_NOENT;
}

void ipc_port_destroy(int port)
{
    if (port < 0 || port >= IPC_MAX_PORTS) {
        return;
    }
    bool_t irq = spinlock_acquire(&ipc_lock);
    ports[port].in_use = false;
    spinlock_release(&ipc_lock, irq);
}

int ipc_send(int port, uint32_t type, const void *data, size_t len)
{
    if (port < 0 || port >= IPC_MAX_PORTS || len > IPC_MSG_MAX) {
        return -QE_INVAL;
    }

    bool_t irq = spinlock_acquire(&ipc_lock);

    struct ipc_port *p = &ports[port];
    if (!p->in_use) {
        spinlock_release(&ipc_lock, irq);
        return -QE_NOENT;
    }
    if (p->count >= IPC_QUEUE_DEPTH) {
        messages_dropped++;
        spinlock_release(&ipc_lock, irq);
        return -QE_AGAIN;
    }

    struct ipc_message *message = &p->queue[p->head];
    message->sender_pid = sched_current_pid();
    message->type       = type;
    message->length     = (uint32_t)len;
    message->timestamp  = time_uptime_ms();
    if (data && len) {
        memcpy(message->data, data, len);
    }

    p->head = (p->head + 1) % IPC_QUEUE_DEPTH;
    p->count++;
    messages_sent++;

    spinlock_release(&ipc_lock, irq);
    return 0;
}

int ipc_receive(int port, struct ipc_message *out)
{
    if (port < 0 || port >= IPC_MAX_PORTS || !out) {
        return -QE_INVAL;
    }

    bool_t irq = spinlock_acquire(&ipc_lock);

    struct ipc_port *p = &ports[port];
    if (!p->in_use) {
        spinlock_release(&ipc_lock, irq);
        return -QE_NOENT;
    }
    if (p->count == 0) {
        spinlock_release(&ipc_lock, irq);
        return -QE_AGAIN;
    }

    *out    = p->queue[p->tail];
    p->tail = (p->tail + 1) % IPC_QUEUE_DEPTH;
    p->count--;

    spinlock_release(&ipc_lock, irq);
    return 0;
}

int ipc_receive_wait(int port, struct ipc_message *out, uint32_t timeout_ms)
{
    uint64_t deadline = time_uptime_ms() + timeout_ms;

    for (;;) {
        int result = ipc_receive(port, out);
        if (result != -QE_AGAIN) {
            return result;
        }
        if (timeout_ms && time_uptime_ms() >= deadline) {
            return -QE_AGAIN;
        }
        sched_sleep_ms(2);
    }
}

int ipc_pending(int port)
{
    if (port < 0 || port >= IPC_MAX_PORTS || !ports[port].in_use) {
        return 0;
    }
    return ports[port].count;
}

uint64_t ipc_messages_sent(void)
{
    return messages_sent;
}

uint64_t ipc_messages_dropped(void)
{
    return messages_dropped;
}

int ipc_port_count(void)
{
    int count = 0;
    for (int i = 0; i < IPC_MAX_PORTS; i++) {
        if (ports[i].in_use) {
            count++;
        }
    }
    return count;
}

/* --- pipes ------------------------------------------------------------ */

struct pipe {
    uint8_t   *buffer;
    size_t     capacity;
    size_t     head;
    size_t     tail;
    size_t     used;
    spinlock_t lock;
};

struct pipe *pipe_create(size_t capacity)
{
    struct pipe *pipe = kzalloc(sizeof(struct pipe));
    if (!pipe) {
        return NULL;
    }

    pipe->buffer = kmalloc(capacity);
    if (!pipe->buffer) {
        kfree(pipe);
        return NULL;
    }
    pipe->capacity = capacity;
    spinlock_init(&pipe->lock, "pipe");
    return pipe;
}

void pipe_destroy(struct pipe *pipe)
{
    if (!pipe) {
        return;
    }
    kfree(pipe->buffer);
    kfree(pipe);
}

ssize_t pipe_write(struct pipe *pipe, const void *data, size_t len)
{
    if (!pipe || !data) {
        return -QE_INVAL;
    }

    bool_t irq = spinlock_acquire(&pipe->lock);

    size_t space   = pipe->capacity - pipe->used;
    size_t written = MIN(len, space);
    const uint8_t *bytes = (const uint8_t *)data;

    for (size_t i = 0; i < written; i++) {
        pipe->buffer[pipe->head] = bytes[i];
        pipe->head               = (pipe->head + 1) % pipe->capacity;
    }
    pipe->used += written;

    spinlock_release(&pipe->lock, irq);
    return (ssize_t)written;
}

ssize_t pipe_read(struct pipe *pipe, void *out, size_t len)
{
    if (!pipe || !out) {
        return -QE_INVAL;
    }

    bool_t irq = spinlock_acquire(&pipe->lock);

    size_t   count = MIN(len, pipe->used);
    uint8_t *bytes = (uint8_t *)out;

    for (size_t i = 0; i < count; i++) {
        bytes[i]   = pipe->buffer[pipe->tail];
        pipe->tail = (pipe->tail + 1) % pipe->capacity;
    }
    pipe->used -= count;

    spinlock_release(&pipe->lock, irq);
    return (ssize_t)count;
}

size_t pipe_available(struct pipe *pipe)
{
    return pipe ? pipe->used : 0;
}
