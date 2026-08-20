/*
 * QitoOS - kernel log ring buffer
 *
 * Every record is timestamped with the monotonic tick counter and tagged with
 * a level and subsystem, then appended to a circular byte buffer. Oldest data
 * is discarded when the buffer wraps, which keeps logging allocation-free and
 * safe to call from interrupt context.
 */

#include <kernel/log.h>
#include <kernel/random.h>
#include <kernel/printf.h>
#include <kernel/string.h>
#include <kernel/io.h>
#include <kernel/time.h>
#include <kernel/spinlock.h>

#define LOG_RING_SIZE  (128 * 1024)
#define LOG_MAX_SINKS  4
#define LOG_LINE_MAX   512

static char        log_ring[LOG_RING_SIZE];
static size_t      log_head;      /* next write offset                     */
static size_t      log_stored;    /* bytes currently retained              */
static uint64_t    log_dropped;   /* bytes discarded on wrap               */
static log_sink_fn log_sinks[LOG_MAX_SINKS];
static log_level_t log_level = LOG_INFO;
static struct log_stats stats;
static spinlock_t  log_lock;
static bool_t      log_ready;

static const char *level_name(log_level_t level)
{
    switch (level) {
    case LOG_PANIC: return "PANIC";
    case LOG_ERROR: return "ERROR";
    case LOG_WARN:  return "WARN ";
    case LOG_INFO:  return "INFO ";
    case LOG_DEBUG: return "DEBUG";
    default:        return "TRACE";
    }
}

void log_init(void)
{
    spinlock_init(&log_lock, "log");
    log_head    = 0;
    log_stored  = 0;
    log_dropped = 0;
    memset(&stats, 0, sizeof(stats));
    memset(log_sinks, 0, sizeof(log_sinks));
    log_ready = true;
}

void log_add_sink(log_sink_fn sink)
{
    for (int i = 0; i < LOG_MAX_SINKS; i++) {
        if (log_sinks[i] == NULL) {
            log_sinks[i] = sink;
            return;
        }
    }
}

void log_remove_sink(log_sink_fn sink)
{
    for (int i = 0; i < LOG_MAX_SINKS; i++) {
        if (log_sinks[i] == sink) {
            log_sinks[i] = NULL;
            return;
        }
    }
}

void log_set_level(log_level_t level)
{
    log_level = level;
}

log_level_t log_get_level(void)
{
    return log_level;
}

/* Append raw bytes to the ring, evicting the oldest data on overflow. */
static void ring_append(const char *text, size_t len)
{
    if (len >= LOG_RING_SIZE) {
        text += len - LOG_RING_SIZE / 2;
        len   = LOG_RING_SIZE / 2;
    }
    for (size_t i = 0; i < len; i++) {
        log_ring[log_head] = text[i];
        log_head           = (log_head + 1) % LOG_RING_SIZE;
        if (log_stored < LOG_RING_SIZE) {
            log_stored++;
        } else {
            log_dropped++;
        }
    }
    stats.bytes_written += len;
}

static void dispatch(const char *text, size_t len, log_level_t level)
{
    for (int i = 0; i < LOG_MAX_SINKS; i++) {
        if (log_sinks[i]) {
            log_sinks[i](text, len, level);
        }
    }
}

void kvlog(log_level_t level, const char *subsystem, const char *fmt, va_list ap)
{
    if (level > log_level && level != LOG_PANIC) {
        return;
    }

    char   line[LOG_LINE_MAX];
    size_t pos = 0;

    /* [    1.234567] LEVEL subsystem: message */
    uint64_t us   = time_uptime_us();
    uint64_t secs = us / 1000000u;
    uint64_t frac = us % 1000000u;

    int n = snprintf(line, sizeof(line), "[%5llu.%06llu] %s %-10s: ",
                     (unsigned long long)secs, (unsigned long long)frac,
                     level_name(level), subsystem ? subsystem : "kernel");
    pos = (n > 0 && (size_t)n < sizeof(line)) ? (size_t)n : 0;

    n = vsnprintf(line + pos, sizeof(line) - pos - 2, fmt, ap);
    if (n > 0) {
        pos += ((size_t)n < sizeof(line) - pos - 2) ? (size_t)n
                                                    : sizeof(line) - pos - 2;
    }
    line[pos++] = '\n';
    line[pos]   = '\0';

    bool_t irq = spinlock_acquire(&log_lock);
    if (log_ready) {
        ring_append(line, pos);
        stats.records++;
        if (level <= LOG_TRACE) {
            stats.counts[level]++;
        }
    }
    spinlock_release(&log_lock, irq);

    dispatch(line, pos, level);
}

void klog(log_level_t level, const char *subsystem, const char *fmt, ...)
{
    va_list ap;
    va_start(ap, fmt);
    kvlog(level, subsystem, fmt, ap);
    va_end(ap);
}

void kputs(const char *s)
{
    size_t len = strlen(s);
    bool_t irq = spinlock_acquire(&log_lock);
    if (log_ready) {
        ring_append(s, len);
    }
    spinlock_release(&log_lock, irq);
    dispatch(s, len, LOG_INFO);
}

void kprintf(const char *fmt, ...)
{
    char    buf[LOG_LINE_MAX];
    va_list ap;

    va_start(ap, fmt);
    vsnprintf(buf, sizeof(buf), fmt, ap);
    va_end(ap);
    kputs(buf);
}

size_t log_read(char *buf, size_t size, size_t offset)
{
    bool_t irq = spinlock_acquire(&log_lock);

    size_t available = log_stored;
    if (offset >= available) {
        spinlock_release(&log_lock, irq);
        return 0;
    }

    size_t start = (log_head + LOG_RING_SIZE - available) % LOG_RING_SIZE;
    start        = (start + offset) % LOG_RING_SIZE;
    size_t count = MIN(size, available - offset);

    for (size_t i = 0; i < count; i++) {
        buf[i] = log_ring[(start + i) % LOG_RING_SIZE];
    }

    spinlock_release(&log_lock, irq);
    return count;
}

size_t log_size(void)
{
    return log_stored;
}

void log_clear(void)
{
    bool_t irq = spinlock_acquire(&log_lock);
    log_head   = 0;
    log_stored = 0;
    spinlock_release(&log_lock, irq);
}

void log_get_stats(struct log_stats *out)
{
    if (!out) {
        return;
    }
    bool_t irq = spinlock_acquire(&log_lock);
    *out       = stats;
    out->dropped = log_dropped;
    spinlock_release(&log_lock, irq);
}

/*
 * Walk the frame pointer chain and print each return address, resolved to a
 * function name where the symbol table can identify it. A backtrace that
 * names functions is worth far more than one that lists addresses, since the
 * addresses change with every build.
 */
void print_backtrace(void)
{
    uint64_t *frame;
    __asm__ volatile("mov %%rbp, %0" : "=r"(frame));

    kputs("Backtrace:\n");

    for (int depth = 0; depth < 16 && frame; depth++) {
        /*
         * Anything outside the kernel's virtual range is either a corrupt
         * frame or the end of the chain; either way, stop rather than
         * faulting inside the panic handler.
         */
        if ((uint64_t)frame < 0xFFFFFFFF80000000ull ||
            (uint64_t)frame > 0xFFFFFFFFFFFF0000ull) {
            break;
        }

        uint64_t return_address = frame[1];
        if (return_address < 0xFFFFFFFF80000000ull) {
            break;
        }

        uint64_t    offset = 0;
        const char *name   = ksym_lookup(return_address, &offset);

        if (name) {
            kprintf("  #%d  %016llx  %s+0x%llx\n", depth,
                    (unsigned long long)return_address, name,
                    (unsigned long long)offset);
        } else {
            kprintf("  #%d  %016llx  (unknown)\n", depth,
                    (unsigned long long)return_address);
        }

        frame = (uint64_t *)frame[0];
    }
}

NORETURN void panic(const char *fmt, ...)
{
    va_list ap;

    cpu_cli();

    kputs("\n=======================================================\n");
    kputs("KERNEL PANIC\n");

    va_start(ap, fmt);
    kvlog(LOG_PANIC, "panic", fmt, ap);
    va_end(ap);

    print_backtrace();

    kputs("System halted.\n");
    kputs("=======================================================\n");

    for (;;) {
        cpu_cli();
        cpu_halt();
    }
}
