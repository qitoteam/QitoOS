/*
 * QitoOS - tasks and scheduling
 */
#ifndef QITO_SCHED_H
#define QITO_SCHED_H

#include <kernel/types.h>
#include <kernel/cpu.h>
#include <kernel/mm.h>

#define TASK_NAME_MAX 32
#define MAX_TASKS     128
#define MAX_FDS       32

typedef enum {
    TASK_UNUSED = 0,
    TASK_READY,
    TASK_RUNNING,
    TASK_BLOCKED,
    TASK_SLEEPING,
    TASK_ZOMBIE,
} task_state_t;

typedef enum {
    PRIO_IDLE       = 0,
    PRIO_LOW        = 1,
    PRIO_NORMAL     = 2,
    PRIO_HIGH       = 3,
    PRIO_REALTIME   = 4,
} task_priority_t;

#define PRIO_COUNT 5

struct task {
    int             pid;
    int             ppid;
    char            name[TASK_NAME_MAX];
    task_state_t    state;
    task_priority_t priority;

    /* Saved register state; points into the task's kernel stack. */
    struct interrupt_frame *frame;

    void      *kernel_stack;      /* base of the allocated stack        */
    uint64_t   kernel_stack_top;

    struct address_space *space;  /* NULL for kernel threads            */
    bool_t     is_kernel;

    /* Scheduling accounting. */
    uint64_t   ticks_total;
    uint64_t   time_slice;
    uint64_t   wake_at_ms;        /* for TASK_SLEEPING                  */
    uint64_t   created_at_ms;

    int        exit_code;

    /* Security context. */
    uint32_t   uid;
    uint32_t   gid;
    uint64_t   capabilities;

    /* Open file descriptors. */
    struct file *fds[MAX_FDS];
    char       cwd[256];

    /* What the task is blocked on, for diagnostics. */
    const char *block_reason;

    struct task *next;
};

void sched_init(void);
void sched_start(void) NORETURN;

/* Create a kernel thread. Returns the new pid or a negative error. */
int  sched_create_kernel_task(const char *name, void (*entry)(void *), void *arg,
                              task_priority_t priority);

int  sched_create_user_task(const char *name, uint64_t entry_va, struct address_space *space,
                            uint64_t stack_top, int argc, uint64_t argv_va,
                            task_priority_t priority);

struct task *sched_current(void);
int  sched_current_pid(void);
struct task *sched_find(int pid);

void sched_yield(void);
void sched_sleep_ms(uint64_t ms);
void sched_block(const char *reason);
void sched_unblock(struct task *task);
void sched_exit(int code) NORETURN;
void sched_kill_current(int code);
int  sched_kill(int pid, int signal);

/* Called from the interrupt return path; may return a different frame. */
struct interrupt_frame *sched_on_interrupt_return(struct interrupt_frame *frame);

/* Introspection used by the shells and the task manager. */
int  sched_task_count(void);
int  sched_runnable_count(void);
int  sched_snapshot(struct task *out, int max);
const char *sched_state_name(task_state_t state);
const char *sched_priority_name(task_priority_t priority);
uint64_t sched_context_switches(void);
uint64_t sched_idle_ticks(void);

#endif /* QITO_SCHED_H */
