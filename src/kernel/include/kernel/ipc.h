/*
 * Qira OS - inter-process communication
 *
 * Provides fixed-size message queues that kernel tasks use to talk to each
 * other without sharing memory, plus byte-stream pipes.
 */
#ifndef QIRA_IPC_H
#define QIRA_IPC_H

#include <kernel/types.h>

#define IPC_MSG_MAX      256
#define IPC_QUEUE_DEPTH  16
#define IPC_MAX_PORTS    32
#define IPC_PORT_NAME_MAX 24

struct ipc_message {
    int      sender_pid;
    uint32_t type;
    uint32_t length;
    uint64_t timestamp;
    uint8_t  data[IPC_MSG_MAX];
};

void ipc_init(void);

/* Named ports. Returns a port id, or negative on failure. */
int  ipc_port_create(const char *name);
int  ipc_port_lookup(const char *name);
void ipc_port_destroy(int port);

/* Non-blocking send. Returns 0 on success, negative when the queue is full. */
int  ipc_send(int port, uint32_t type, const void *data, size_t len);

/* Receive; returns 0 on success or negative when the queue is empty. */
int  ipc_receive(int port, struct ipc_message *out);

/* Block until a message arrives, or until the timeout expires. */
int  ipc_receive_wait(int port, struct ipc_message *out, uint32_t timeout_ms);

int  ipc_pending(int port);

/* Byte-stream pipes. */
struct pipe;
struct pipe *pipe_create(size_t capacity);
void         pipe_destroy(struct pipe *pipe);
ssize_t      pipe_write(struct pipe *pipe, const void *data, size_t len);
ssize_t      pipe_read(struct pipe *pipe, void *out, size_t len);
size_t       pipe_available(struct pipe *pipe);

/* Statistics. */
uint64_t ipc_messages_sent(void);
uint64_t ipc_messages_dropped(void);
int      ipc_port_count(void);

#endif /* QIRA_IPC_H */
