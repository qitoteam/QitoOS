/*
 * Qira OS - service manager
 *
 * Long-running kernel tasks are registered as services so they can be listed,
 * started and stopped from QCSH, and so their state is visible in one place.
 */
#ifndef QIRA_SERVICE_H
#define QIRA_SERVICE_H

#include <kernel/types.h>
#include <kernel/sched.h>

struct shell;

#define SERVICE_NAME_MAX 24
#define MAX_SERVICES     16

typedef enum {
    SERVICE_STOPPED = 0,
    SERVICE_RUNNING,
    SERVICE_FAILED,
} service_state_t;

struct service {
    char             name[SERVICE_NAME_MAX];
    char             description[64];
    service_state_t  state;
    int              pid;
    bool_t           auto_start;
    bool_t           essential;   /* cannot be stopped */
    task_priority_t  priority;
    void           (*entry)(void *);
    uint64_t         started_at_ms;
    uint32_t         restart_count;
};

void service_init(void);

int  service_register(const char *name, const char *description,
                      void (*entry)(void *), task_priority_t priority,
                      bool_t auto_start, bool_t essential);

/* Start every service marked auto_start. */
void service_start_all(void);

int  service_start(struct shell *sh, const char *name);
int  service_stop(struct shell *sh, const char *name);
int  service_status(struct shell *sh, const char *name);
void service_list(struct shell *sh);

int  service_count(void);
const struct service *service_at(int index);

#endif /* QIRA_SERVICE_H */
