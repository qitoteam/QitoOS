/*
 * Qira OS - preemptive round-robin scheduler with priority levels
 *
 * Tasks are kept in per-priority ready queues. On each timer tick the current
 * task's slice is decremented; when it expires the scheduler picks the highest
 * priority runnable task and switches to it by swapping the interrupt frame
 * that the ISR epilogue restores.
 *
 * Because the switch happens entirely through the interrupt frame, no separate
 * assembly context-switch routine is needed for preemption.
 */

#include <kernel/sched.h>
#include <kernel/mm.h>
#include <kernel/log.h>
#include <kernel/string.h>
#include <kernel/time.h>
#include <kernel/io.h>
#include <kernel/spinlock.h>
#include <kernel/fs.h>

/*
 * Kernel task stacks. Shell commands can nest several frames deep
 * (pipeline -> command -> formatter), so this is generous; a stack overflow
 * silently corrupts the neighbouring heap block rather than faulting, which
 * is expensive to debug.
 */
#define KERNEL_STACK_SIZE 65536

/* Written at the base of every stack and checked to detect an overflow. */
#define STACK_GUARD_MAGIC 0x5175697261537447ULL
#define SLICE_TICKS(p)    (2u + (uint64_t)(p) * 2u)

static struct task  tasks[MAX_TASKS];
static struct task *current;
static struct task *idle_task;
static int          next_pid = 1;
static bool_t       scheduler_running;
static spinlock_t   sched_lock;

static uint64_t context_switches;
static uint64_t idle_ticks;

/* Ready queues, highest priority last so the search runs downward. */
static struct task *ready_head[PRIO_COUNT];
static struct task *ready_tail[PRIO_COUNT];

const char *sched_state_name(task_state_t state)
{
    switch (state) {
    case TASK_UNUSED:   return "unused";
    case TASK_READY:    return "ready";
    case TASK_RUNNING:  return "running";
    case TASK_BLOCKED:  return "blocked";
    case TASK_SLEEPING: return "sleeping";
    case TASK_ZOMBIE:   return "zombie";
    default:            return "?";
    }
}

const char *sched_priority_name(task_priority_t priority)
{
    switch (priority) {
    case PRIO_IDLE:     return "idle";
    case PRIO_LOW:      return "low";
    case PRIO_NORMAL:   return "normal";
    case PRIO_HIGH:     return "high";
    case PRIO_REALTIME: return "realtime";
    default:            return "?";
    }
}

static void enqueue(struct task *task)
{
    if (task->priority >= PRIO_COUNT) {
        task->priority = PRIO_NORMAL;
    }
    task->next  = NULL;
    task->state = TASK_READY;

    int level = (int)task->priority;
    if (ready_tail[level]) {
        ready_tail[level]->next = task;
        ready_tail[level]       = task;
    } else {
        ready_head[level] = ready_tail[level] = task;
    }
}

static struct task *dequeue_highest(void)
{
    for (int level = PRIO_COUNT - 1; level >= 0; level--) {
        struct task *task = ready_head[level];
        if (!task) {
            continue;
        }
        ready_head[level] = task->next;
        if (!ready_head[level]) {
            ready_tail[level] = NULL;
        }
        task->next = NULL;
        return task;
    }
    return NULL;
}

/* Remove a task from whichever ready queue holds it. */
static void unlink_ready(struct task *target)
{
    for (int level = 0; level < PRIO_COUNT; level++) {
        struct task *prev = NULL;
        for (struct task *task = ready_head[level]; task; task = task->next) {
            if (task == target) {
                if (prev) {
                    prev->next = task->next;
                } else {
                    ready_head[level] = task->next;
                }
                if (ready_tail[level] == task) {
                    ready_tail[level] = prev;
                }
                task->next = NULL;
                return;
            }
            prev = task;
        }
    }
}

static struct task *alloc_task(void)
{
    for (int i = 0; i < MAX_TASKS; i++) {
        if (tasks[i].state == TASK_UNUSED) {
            memset(&tasks[i], 0, sizeof(struct task));
            return &tasks[i];
        }
    }
    return NULL;
}

/*
 * A kernel thread that returns falls through to here, which terminates it
 * cleanly instead of jumping to a garbage return address.
 */
static void task_exit_trampoline(void)
{
    sched_exit(0);
}

int sched_create_kernel_task(const char *name, void (*entry)(void *), void *arg,
                             task_priority_t priority)
{
    bool_t irq  = spinlock_acquire(&sched_lock);
    struct task *task = alloc_task();

    if (!task) {
        spinlock_release(&sched_lock, irq);
        KLOG_ERR("sched", "task table is full, cannot create '%s'", name);
        return -1;
    }

    task->pid       = next_pid++;
    task->ppid      = current ? current->pid : 0;
    task->priority  = priority;
    task->is_kernel = true;
    task->uid       = 0;
    task->gid       = 0;
    task->capabilities = ~0ull;
    task->created_at_ms = time_uptime_ms();
    task->time_slice = SLICE_TICKS(priority);
    strlcpy(task->name, name, sizeof(task->name));
    strlcpy(task->cwd, "/", sizeof(task->cwd));

    spinlock_release(&sched_lock, irq);

    /* The stack must be allocated outside the lock: kmalloc may block. */
    task->kernel_stack = kmalloc(KERNEL_STACK_SIZE);
    if (!task->kernel_stack) {
        task->state = TASK_UNUSED;
        KLOG_ERR("sched", "no memory for the '%s' kernel stack", name);
        return -1;
    }

    /* Plant a guard value at the low end so an overflow can be detected. */
    *(uint64_t *)task->kernel_stack = STACK_GUARD_MAGIC;

    uint64_t stack_top = (uint64_t)task->kernel_stack + KERNEL_STACK_SIZE;
    stack_top &= ~0xFull;
    task->kernel_stack_top = stack_top;

    /*
     * Build a synthetic interrupt frame so the normal ISR return path can
     * "resume" a task that has never actually run.
     */
    stack_top -= sizeof(struct interrupt_frame);
    struct interrupt_frame *frame = (struct interrupt_frame *)stack_top;
    memset(frame, 0, sizeof(*frame));

    frame->rip    = (uint64_t)entry;
    frame->cs     = SEL_KERNEL_CODE;
    frame->rflags = 0x202;               /* interrupts enabled            */
    frame->ss     = SEL_KERNEL_DATA;
    frame->rdi    = (uint64_t)arg;       /* first argument                */
    frame->vector = 0;

    /*
     * The task's own stack pointer, with a return address that terminates it
     * if the entry function ever returns.
     */
    uint64_t *task_stack = (uint64_t *)(stack_top & ~0xFull);
    task_stack--;
    *task_stack = (uint64_t)task_exit_trampoline;
    frame->rsp  = (uint64_t)task_stack;

    task->frame = frame;

    irq = spinlock_acquire(&sched_lock);
    enqueue(task);
    spinlock_release(&sched_lock, irq);

    KLOG_DEBUG("sched", "created task %d '%s' (%s priority)", task->pid, name,
               sched_priority_name(priority));
    return task->pid;
}

static void idle_entry(void *arg)
{
    UNUSED(arg);
    for (;;) {
        idle_ticks++;
        cpu_halt();
    }
}

void sched_init(void)
{
    spinlock_init(&sched_lock, "sched");
    memset(tasks, 0, sizeof(tasks));
    memset(ready_head, 0, sizeof(ready_head));
    memset(ready_tail, 0, sizeof(ready_tail));

    current           = NULL;
    scheduler_running = false;

    int pid = sched_create_kernel_task("idle", idle_entry, NULL, PRIO_IDLE);
    idle_task = sched_find(pid);

    KLOG_INFO("sched", "scheduler ready (%d task slots)", MAX_TASKS);
}

struct task *sched_current(void)
{
    return current;
}

int sched_current_pid(void)
{
    return current ? current->pid : 0;
}

struct task *sched_find(int pid)
{
    for (int i = 0; i < MAX_TASKS; i++) {
        if (tasks[i].state != TASK_UNUSED && tasks[i].pid == pid) {
            return &tasks[i];
        }
    }
    return NULL;
}

/* Move any sleeping tasks whose deadline has passed back to the ready queues. */
static void wake_sleepers(void)
{
    uint64_t now = time_uptime_ms();

    for (int i = 0; i < MAX_TASKS; i++) {
        struct task *task = &tasks[i];
        if (task->state == TASK_SLEEPING && now >= task->wake_at_ms) {
            task->wake_at_ms = 0;
            enqueue(task);
        }
    }
}

/*
 * Choose the next task and return the frame to restore.
 * Must be called with interrupts disabled.
 */
static struct interrupt_frame *switch_to_next(struct interrupt_frame *frame)
{
    struct task *previous = current;

    if (previous) {
        previous->frame = frame;
        if (previous->state == TASK_RUNNING) {
            enqueue(previous);
        }
    }

    struct task *next = dequeue_highest();
    if (!next) {
        next = idle_task;
        if (next->state == TASK_READY || next->state == TASK_RUNNING) {
            unlink_ready(next);
        }
    }

    current             = next;
    current->state      = TASK_RUNNING;
    current->time_slice = SLICE_TICKS(current->priority);

    if (previous != current) {
        context_switches++;

        /* Point the TSS at this task's kernel stack for ring 3 entries. */
        tss_set_kernel_stack(current->kernel_stack_top);

        if (current->space) {
            vmm_switch(current->space);
        } else {
            vmm_switch(vmm_kernel_space());
        }
    }

    return current->frame;
}

/*
 * Verify a task has not run off the bottom of its stack. A kernel stack is an
 * ordinary heap allocation, so an overflow corrupts whatever block sits below
 * it and only surfaces later as unrelated heap corruption. Checking a guard
 * value on every switch turns that into an immediate, attributable panic.
 */
static void check_stack_guard(struct task *task)
{
    if (!task || !task->kernel_stack) {
        return;
    }
    if (*(uint64_t *)task->kernel_stack != STACK_GUARD_MAGIC) {
        panic("kernel stack overflow in task %d (%s): stack guard destroyed",
              task->pid, task->name);
    }
}

struct interrupt_frame *sched_on_interrupt_return(struct interrupt_frame *frame)
{
    if (!scheduler_running) {
        return frame;
    }

    check_stack_guard(current);

    /* Only the timer interrupt drives preemption. */
    bool_t timer = (frame->vector == 32);

    wake_sleepers();

    if (current) {
        current->ticks_total += timer ? 1 : 0;

        if (timer && current->time_slice > 0) {
            current->time_slice--;
        }

        /* Keep running unless the slice expired or the task is not runnable. */
        if (current->state == TASK_RUNNING && current->time_slice > 0) {
            return frame;
        }
    }

    return switch_to_next(frame);
}

NORETURN void sched_start(void)
{
    /*
     * Announce the hand-over *before* arming the scheduler. Interrupts are
     * already enabled by this point, so flipping the flag first would let
     * the very next timer tick preempt this context in the middle of the
     * log write — the message would be interleaved with, and truncated by,
     * whatever the init task printed next. Log first, then arm with
     * interrupts masked so the flag and the idle loop are reached
     * atomically.
     */
    KLOG_INFO("sched", "starting the scheduler");

    cpu_cli();
    scheduler_running = true;

    /*
     * Wait for the first timer interrupt to perform the initial switch. The
     * bootstrap context becomes the idle loop until then.
     */
    cpu_sti();
    for (;;) {
        cpu_halt();
    }
}

void sched_yield(void)
{
    if (!scheduler_running || !current) {
        return;
    }
    current->time_slice = 0;

    /* Trigger a reschedule immediately via a software interrupt. */
    __asm__ volatile("int $32");
}

void sched_sleep_ms(uint64_t ms)
{
    if (!scheduler_running || !current) {
        time_sleep_ms((uint32_t)ms);
        return;
    }

    bool_t irq = spinlock_acquire(&sched_lock);
    current->wake_at_ms = time_uptime_ms() + ms;
    current->state      = TASK_SLEEPING;
    current->time_slice = 0;
    spinlock_release(&sched_lock, irq);

    __asm__ volatile("int $32");
}

void sched_block(const char *reason)
{
    if (!scheduler_running || !current) {
        return;
    }

    bool_t irq = spinlock_acquire(&sched_lock);
    current->state        = TASK_BLOCKED;
    current->block_reason = reason;
    current->time_slice   = 0;
    spinlock_release(&sched_lock, irq);

    __asm__ volatile("int $32");
}

void sched_unblock(struct task *task)
{
    if (!task || task->state != TASK_BLOCKED) {
        return;
    }

    bool_t irq = spinlock_acquire(&sched_lock);
    task->block_reason = NULL;
    enqueue(task);
    spinlock_release(&sched_lock, irq);
}

NORETURN void sched_exit(int code)
{
    if (current) {
        KLOG_DEBUG("sched", "task %d '%s' exited with code %d", current->pid,
                   current->name, code);
        current->exit_code = code;
        current->state     = TASK_ZOMBIE;
        current->time_slice = 0;

        /* Wake a parent that is waiting on this child. */
        struct task *parent = sched_find(current->ppid);
        if (parent && parent->state == TASK_BLOCKED) {
            sched_unblock(parent);
        }
    }

    for (;;) {
        __asm__ volatile("int $32");
        cpu_halt();
    }
}

void sched_kill_current(int code)
{
    if (!current || current == idle_task) {
        panic("attempt to kill the idle task");
    }
    current->exit_code  = code;
    current->state      = TASK_ZOMBIE;
    current->time_slice = 0;
}

int sched_kill(int pid, int signal)
{
    UNUSED(signal);

    struct task *task = sched_find(pid);
    if (!task || task == idle_task) {
        return -1;
    }

    bool_t irq = spinlock_acquire(&sched_lock);
    unlink_ready(task);
    task->state     = TASK_ZOMBIE;
    task->exit_code = 130;
    spinlock_release(&sched_lock, irq);

    KLOG_INFO("sched", "task %d '%s' killed", task->pid, task->name);
    return 0;
}

/* How many tasks are ready to run right now, for the load average. */
int sched_runnable_count(void)
{
    int count = 0;
    for (int i = 0; i < MAX_TASKS; i++) {
        if (tasks[i].state == TASK_READY || tasks[i].state == TASK_RUNNING) {
            count++;
        }
    }
    /* The idle task is always runnable but is not real work. */
    return MAX(count - 1, 0);
}

int sched_task_count(void)
{
    int count = 0;
    for (int i = 0; i < MAX_TASKS; i++) {
        if (tasks[i].state != TASK_UNUSED) {
            count++;
        }
    }
    return count;
}

int sched_snapshot(struct task *out, int max)
{
    int count = 0;
    bool_t irq = spinlock_acquire(&sched_lock);

    for (int i = 0; i < MAX_TASKS && count < max; i++) {
        if (tasks[i].state != TASK_UNUSED) {
            out[count++] = tasks[i];
        }
    }

    spinlock_release(&sched_lock, irq);
    return count;
}

uint64_t sched_context_switches(void)
{
    return context_switches;
}

uint64_t sched_idle_ticks(void)
{
    return idle_ticks;
}
