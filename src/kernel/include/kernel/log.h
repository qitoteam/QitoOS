/*
 * Qira OS - kernel logging and diagnostics
 *
 * Log records are written to a fixed-size in-memory ring buffer and mirrored
 * to any registered sinks (serial port, framebuffer console). The ring buffer
 * is what `qcsh dmesg` and the Logs application read back.
 */
#ifndef QIRA_LOG_H
#define QIRA_LOG_H

#include <kernel/types.h>

typedef enum {
    LOG_PANIC = 0,
    LOG_ERROR = 1,
    LOG_WARN  = 2,
    LOG_INFO  = 3,
    LOG_DEBUG = 4,
    LOG_TRACE = 5,
} log_level_t;

/* A sink receives fully formatted lines. */
typedef void (*log_sink_fn)(const char *text, size_t len, log_level_t level);

void log_init(void);
void log_add_sink(log_sink_fn sink);
void log_remove_sink(log_sink_fn sink);

void log_set_level(log_level_t level);
log_level_t log_get_level(void);

void klog(log_level_t level, const char *subsystem, const char *fmt, ...)
    PRINTF_FMT(3, 4);
void kvlog(log_level_t level, const char *subsystem, const char *fmt, va_list ap);

/* Raw, unformatted output straight to the sinks. */
void kputs(const char *s);
void kprintf(const char *fmt, ...) PRINTF_FMT(1, 2);

/*
 * Copy up to `size` bytes of the log ring into `buf`, starting `offset` bytes
 * from the oldest retained record. Returns the number of bytes written.
 */
size_t log_read(char *buf, size_t size, size_t offset);
size_t log_size(void);
void   log_clear(void);

/* Statistics surfaced by diagnostics tools. */
struct log_stats {
    uint64_t records;
    uint64_t dropped;
    uint64_t bytes_written;
    uint32_t counts[6];
};
void log_get_stats(struct log_stats *out);

void print_backtrace(void);
NORETURN void panic(const char *fmt, ...) PRINTF_FMT(1, 2);

#define KLOG_ERR(sub, ...)   klog(LOG_ERROR, sub, __VA_ARGS__)
#define KLOG_WARN(sub, ...)  klog(LOG_WARN,  sub, __VA_ARGS__)
#define KLOG_INFO(sub, ...)  klog(LOG_INFO,  sub, __VA_ARGS__)
#define KLOG_DEBUG(sub, ...) klog(LOG_DEBUG, sub, __VA_ARGS__)

#define ASSERT(cond)                                                        \
    do {                                                                    \
        if (!(cond)) {                                                      \
            panic("assertion failed: %s at %s:%d", #cond, __FILE__,         \
                  __LINE__);                                                \
        }                                                                   \
    } while (0)

#endif /* QIRA_LOG_H */
