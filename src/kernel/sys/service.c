/*
 * QitoOS - service manager
 */

#include <kernel/service.h>
#include <kernel/sched.h>
#include <kernel/log.h>
#include <kernel/string.h>
#include <kernel/shell.h>
#include <kernel/time.h>

static struct service services[MAX_SERVICES];
static int            service_total;

void service_init(void)
{
    service_total = 0;
    memset(services, 0, sizeof(services));
    KLOG_INFO("service", "service manager ready");
}

int service_register(const char *name, const char *description,
                     void (*entry)(void *), task_priority_t priority,
                     bool_t auto_start, bool_t essential)
{
    if (service_total >= MAX_SERVICES) {
        KLOG_ERR("service", "cannot register '%s': table full", name);
        return -1;
    }

    struct service *service = &services[service_total++];

    strlcpy(service->name, name, sizeof(service->name));
    strlcpy(service->description, description, sizeof(service->description));
    service->entry      = entry;
    service->priority   = priority;
    service->auto_start = auto_start;
    service->essential  = essential;
    service->state      = SERVICE_STOPPED;
    service->pid        = 0;

    return 0;
}

static struct service *find(const char *name)
{
    for (int i = 0; i < service_total; i++) {
        if (strcmp(services[i].name, name) == 0) {
            return &services[i];
        }
    }
    return NULL;
}

static int start_service(struct service *service)
{
    if (service->state == SERVICE_RUNNING) {
        return 0;
    }

    int pid = sched_create_kernel_task(service->name, service->entry, NULL,
                                       service->priority);
    if (pid < 0) {
        service->state = SERVICE_FAILED;
        KLOG_ERR("service", "failed to start '%s'", service->name);
        return -1;
    }

    service->pid           = pid;
    service->state         = SERVICE_RUNNING;
    service->started_at_ms = time_uptime_ms();
    KLOG_INFO("service", "started '%s' as pid %d", service->name, pid);
    return 0;
}

void service_start_all(void)
{
    for (int i = 0; i < service_total; i++) {
        if (services[i].auto_start) {
            start_service(&services[i]);
        }
    }
}

int service_start(struct shell *sh, const char *name)
{
    struct service *service = find(name);

    if (!service) {
        shell_printf(sh, "service: no such service: %s\n", name);
        return 1;
    }
    if (service->state == SERVICE_RUNNING) {
        shell_printf(sh, "service: %s is already running (pid %d)\n", name,
                     service->pid);
        return 0;
    }
    if (start_service(service) != 0) {
        shell_printf(sh, "service: failed to start %s\n", name);
        return 1;
    }

    service->restart_count++;
    shell_printf(sh, "service: started %s (pid %d)\n", name, service->pid);
    return 0;
}

int service_stop(struct shell *sh, const char *name)
{
    struct service *service = find(name);

    if (!service) {
        shell_printf(sh, "service: no such service: %s\n", name);
        return 1;
    }
    if (service->essential) {
        shell_printf(sh, "service: %s is essential and cannot be stopped\n", name);
        return 1;
    }
    if (service->state != SERVICE_RUNNING) {
        shell_printf(sh, "service: %s is not running\n", name);
        return 0;
    }

    sched_kill(service->pid, 9);
    service->state = SERVICE_STOPPED;
    service->pid   = 0;

    shell_printf(sh, "service: stopped %s\n", name);
    return 0;
}

static const char *state_name(service_state_t state)
{
    switch (state) {
    case SERVICE_RUNNING: return "running";
    case SERVICE_FAILED:  return "failed";
    default:              return "stopped";
    }
}

int service_status(struct shell *sh, const char *name)
{
    struct service *service = find(name);

    if (!service) {
        shell_printf(sh, "service: no such service: %s\n", name);
        return 1;
    }

    shell_printf(sh, "%s - %s\n", service->name, service->description);
    shell_printf(sh, "  State        : %s\n", state_name(service->state));
    shell_printf(sh, "  PID          : %d\n", service->pid);
    shell_printf(sh, "  Priority     : %s\n", sched_priority_name(service->priority));
    shell_printf(sh, "  Auto start   : %s\n", service->auto_start ? "yes" : "no");
    shell_printf(sh, "  Essential    : %s\n", service->essential ? "yes" : "no");
    shell_printf(sh, "  Restarts     : %u\n", service->restart_count);

    if (service->state == SERVICE_RUNNING) {
        uint64_t uptime = (time_uptime_ms() - service->started_at_ms) / 1000;
        shell_printf(sh, "  Uptime       : %llus\n", (unsigned long long)uptime);

        struct task *task = sched_find(service->pid);
        if (task) {
            shell_printf(sh, "  Task state   : %s\n", sched_state_name(task->state));
            shell_printf(sh, "  CPU ticks    : %llu\n",
                         (unsigned long long)task->ticks_total);
        }
    }
    return 0;
}

void service_list(struct shell *sh)
{
    shell_printf(sh, "%-16s %-9s %6s  %s\n", "SERVICE", "STATE", "PID",
                 "DESCRIPTION");

    for (int i = 0; i < service_total; i++) {
        struct service *service = &services[i];

        /* Reconcile with the scheduler: a task may have exited. */
        if (service->state == SERVICE_RUNNING) {
            struct task *task = sched_find(service->pid);
            if (!task || task->state == TASK_ZOMBIE) {
                service->state = SERVICE_STOPPED;
                service->pid   = 0;
            }
        }

        shell_printf(sh, "%-16s %-9s %6d  %s\n", service->name,
                     state_name(service->state), service->pid,
                     service->description);
    }

    shell_printf(sh, "\n%d service(s) registered.\n", service_total);
}

int service_count(void)
{
    return service_total;
}

const struct service *service_at(int index)
{
    if (index < 0 || index >= service_total) {
        return NULL;
    }
    return &services[index];
}
