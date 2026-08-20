/*
 * QitoOS - system information structure shared with userspace
 */
#ifndef QITO_SYSINFO_H
#define QITO_SYSINFO_H

#include <kernel/types.h>

struct qito_sysinfo {
    char     version[16];
    char     codename[24];
    char     arch[16];
    char     cpu_model[64];
    char     cpu_vendor[16];
    uint64_t uptime_ms;
    uint64_t total_memory;
    uint64_t free_memory;
    uint64_t used_memory;
    uint64_t cpu_khz;
    uint64_t unix_time;
    uint32_t task_count;
    uint32_t reserved;
};

#endif /* QITO_SYSINFO_H */
