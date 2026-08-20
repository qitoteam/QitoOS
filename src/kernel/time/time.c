/*
 * QitoOS - timekeeping
 *
 * Combines three sources:
 *   - the 8253/8254 PIT, programmed to TIMER_HZ for the scheduler tick,
 *   - the CMOS real-time clock for wall-clock time,
 *   - the TSC, calibrated against the PIT, for microsecond resolution.
 */

#include <kernel/time.h>
#include <kernel/io.h>
#include <kernel/irq.h>
#include <kernel/log.h>
#include <kernel/string.h>

/* PIT */
#define PIT_CHANNEL0  0x40
#define PIT_CHANNEL2  0x42
#define PIT_COMMAND   0x43
#define PIT_FREQUENCY 1193182u

/* CMOS/RTC */
#define CMOS_ADDRESS 0x70
#define CMOS_DATA    0x71

static volatile uint64_t tick_count;
static uint64_t          cpu_khz;
static uint64_t          boot_unix_time;
static uint64_t          tsc_at_boot;
static uint64_t          tsc_epoch_us;   /* uptime when the TSC was calibrated */

uint64_t time_ticks(void)
{
    return tick_count;
}

void timer_tick(void)
{
    tick_count++;
}

uint64_t time_uptime_ms(void)
{
    return (tick_count * 1000u) / TIMER_HZ;
}

uint64_t time_uptime_us(void)
{
    /*
     * Prefer the TSC once it has been calibrated: it gives microsecond
     * resolution instead of the 10 ms PIT tick. `tsc_epoch_us` carries the
     * uptime that had already elapsed when calibration finished, so the clock
     * never appears to jump backwards.
     */
    if (cpu_khz) {
        uint64_t delta = rdtsc() - tsc_at_boot;
        return tsc_epoch_us + delta / cpu_khz * 1000u +
               (delta % cpu_khz) * 1000u / cpu_khz;
    }
    return (tick_count * 1000000u) / TIMER_HZ;
}

static void pit_set_frequency(uint32_t hz)
{
    uint32_t divisor = PIT_FREQUENCY / hz;

    if (divisor > 65535) {
        divisor = 65535;
    }
    if (divisor < 1) {
        divisor = 1;
    }

    /* Channel 0, lobyte/hibyte access, mode 3 (square wave), binary. */
    outb(PIT_COMMAND, 0x36);
    outb(PIT_CHANNEL0, (uint8_t)(divisor & 0xFF));
    outb(PIT_CHANNEL0, (uint8_t)((divisor >> 8) & 0xFF));
}

/*
 * Calibrate the TSC using PIT channel 2, which can be polled without
 * interrupts. Runs for roughly 50 ms.
 */
static void calibrate_tsc(void)
{
    /* Enable the channel 2 gate, disable the speaker output. */
    uint8_t port61 = inb(0x61);
    outb(0x61, (uint8_t)((port61 & ~0x02) | 0x01));

    /* One-shot mode with a ~50 ms count. */
    const uint32_t counts = PIT_FREQUENCY / 20;
    outb(PIT_COMMAND, 0xB0);   /* channel 2, lobyte/hibyte, mode 0 */
    outb(PIT_CHANNEL2, (uint8_t)(counts & 0xFF));
    outb(PIT_CHANNEL2, (uint8_t)(counts >> 8));

    uint64_t start = rdtsc();

    /* Wait for the output bit of port 0x61 to go high. */
    uint64_t guard = 0;
    while (!(inb(0x61) & 0x20)) {
        if (++guard > 100000000ull) {
            break;
        }
    }

    uint64_t elapsed = rdtsc() - start;

    outb(0x61, port61);

    /* 50 ms of TSC ticks -> kHz. */
    cpu_khz = elapsed / 50;
    if (cpu_khz < 1000 || cpu_khz > 20000000ull) {
        /* Nonsense result (emulator without a usable TSC); fall back. */
        cpu_khz = 0;
        KLOG_WARN("time", "TSC calibration failed, using PIT ticks only");
    } else {
        KLOG_INFO("time", "CPU frequency approximately %llu.%03llu MHz",
                  (unsigned long long)(cpu_khz / 1000),
                  (unsigned long long)(cpu_khz % 1000));
    }

    /* Anchor the TSC clock to the uptime accumulated so far. */
    tsc_epoch_us = (tick_count * 1000000u) / TIMER_HZ;
    tsc_at_boot  = rdtsc();
}

uint64_t time_cpu_khz(void)
{
    return cpu_khz;
}

/* --- CMOS real-time clock ------------------------------------------- */

static uint8_t cmos_read(uint8_t reg)
{
    /* Preserve the NMI-disable bit in the high position. */
    outb(CMOS_ADDRESS, (uint8_t)((inb(CMOS_ADDRESS) & 0x80) | (reg & 0x7F)));
    return inb(CMOS_DATA);
}

static bool_t rtc_updating(void)
{
    return (cmos_read(0x0A) & 0x80) != 0;
}

static uint8_t bcd_to_bin(uint8_t value)
{
    return (uint8_t)((value & 0x0F) + ((value >> 4) * 10));
}

void rtc_read(struct qito_time *out)
{
    struct qito_time first, second;
    int              guard = 0;

    memset(out, 0, sizeof(*out));

    /* Read twice and accept the value only when two reads agree. */
    do {
        while (rtc_updating() && ++guard < 1000000) {
        }

        first.second  = cmos_read(0x00);
        first.minute  = cmos_read(0x02);
        first.hour    = cmos_read(0x04);
        first.weekday = cmos_read(0x06);
        first.day     = cmos_read(0x07);
        first.month   = cmos_read(0x08);
        first.year    = cmos_read(0x09);

        while (rtc_updating() && ++guard < 2000000) {
        }

        second.second  = cmos_read(0x00);
        second.minute  = cmos_read(0x02);
        second.hour    = cmos_read(0x04);
        second.weekday = cmos_read(0x06);
        second.day     = cmos_read(0x07);
        second.month   = cmos_read(0x08);
        second.year    = cmos_read(0x09);
    } while ((first.second != second.second || first.minute != second.minute ||
              first.hour != second.hour || first.day != second.day ||
              first.month != second.month || first.year != second.year) &&
             ++guard < 3000000);

    uint8_t status_b = cmos_read(0x0B);
    bool_t  is_bcd   = !(status_b & 0x04);
    bool_t  is_12h   = !(status_b & 0x02);

    if (is_bcd) {
        first.second  = bcd_to_bin((uint8_t)first.second);
        first.minute  = bcd_to_bin((uint8_t)first.minute);
        first.day     = bcd_to_bin((uint8_t)first.day);
        first.month   = bcd_to_bin((uint8_t)first.month);
        first.year    = bcd_to_bin((uint8_t)first.year);
        first.weekday = bcd_to_bin((uint8_t)first.weekday);
        /* The 12-hour PM flag lives in bit 7 and must survive conversion. */
        bool_t pm  = (first.hour & 0x80) != 0;
        first.hour = bcd_to_bin((uint8_t)(first.hour & 0x7F));
        if (pm) {
            first.hour |= 0x80;
        }
    }

    if (is_12h && (first.hour & 0x80)) {
        first.hour = ((first.hour & 0x7F) % 12) + 12;
    }

    /* Two-digit year: assume 2000-2099. */
    first.year += (first.year < 70) ? 2000 : 1900;
    if (first.weekday >= 1) {
        first.weekday -= 1;
    }

    *out = first;
}

static const int days_in_month[12] = {31, 28, 31, 30, 31, 30,
                                      31, 31, 30, 31, 30, 31};

static bool_t is_leap(int year)
{
    return (year % 4 == 0 && year % 100 != 0) || (year % 400 == 0);
}

uint64_t time_to_unix(const struct qito_time *t)
{
    uint64_t days = 0;

    for (int year = 1970; year < t->year; year++) {
        days += is_leap(year) ? 366 : 365;
    }
    for (int month = 1; month < t->month; month++) {
        days += days_in_month[month - 1];
        if (month == 2 && is_leap(t->year)) {
            days++;
        }
    }
    days += (uint64_t)(t->day - 1);

    return ((days * 24 + (uint64_t)t->hour) * 60 + (uint64_t)t->minute) * 60 +
           (uint64_t)t->second;
}

void time_from_unix(uint64_t unix_time, struct qito_time *out)
{
    uint64_t days    = unix_time / 86400u;
    uint64_t seconds = unix_time % 86400u;

    out->hour    = (int)(seconds / 3600u);
    out->minute  = (int)((seconds % 3600u) / 60u);
    out->second  = (int)(seconds % 60u);
    out->weekday = (int)((days + 4) % 7);   /* 1970-01-01 was a Thursday */

    int year = 1970;
    for (;;) {
        uint64_t year_days = is_leap(year) ? 366u : 365u;
        if (days < year_days) {
            break;
        }
        days -= year_days;
        year++;
    }
    out->year = year;

    int month = 0;
    for (; month < 12; month++) {
        uint64_t month_days = (uint64_t)days_in_month[month];
        if (month == 1 && is_leap(year)) {
            month_days++;
        }
        if (days < month_days) {
            break;
        }
        days -= month_days;
    }
    out->month = month + 1;
    out->day   = (int)days + 1;
}

uint64_t rtc_unix_time(void)
{
    /*
     * Wall-clock time = the RTC reading taken at boot plus the monotonic
     * uptime. This avoids hammering the CMOS ports on every query.
     */
    return boot_unix_time + time_uptime_ms() / 1000u;
}

void time_format(const struct qito_time *t, char *buf, size_t size)
{
    extern int snprintf(char *, size_t, const char *, ...);
    snprintf(buf, size, "%04d-%02d-%02d %02d:%02d:%02d", t->year, t->month, t->day,
             t->hour, t->minute, t->second);
}

/* --- timer IRQ ------------------------------------------------------ */

static void timer_irq_handler(struct interrupt_frame *frame, void *context)
{
    UNUSED(frame);
    UNUSED(context);
    timer_tick();
}

void time_init(void)
{
    tick_count = 0;

    struct qito_time now;
    rtc_read(&now);
    boot_unix_time = time_to_unix(&now);

    char stamp[32];
    time_format(&now, stamp, sizeof(stamp));
    KLOG_INFO("time", "real-time clock reads %s UTC", stamp);

    calibrate_tsc();

    pit_set_frequency(TIMER_HZ);
    irq_register(IRQ_TIMER, timer_irq_handler, NULL, "pit-timer");

    KLOG_INFO("time", "PIT programmed to %d Hz", TIMER_HZ);

    // Initialize HPET for high-resolution timing and frame pacing
    extern void hpet_init(void);
    hpet_init();
}

void time_udelay(uint32_t us)
{
    if (cpu_khz) {
        uint64_t target = rdtsc() + ((uint64_t)us * cpu_khz) / 1000u;
        while (rdtsc() < target) {
            cpu_pause();
        }
        return;
    }
    /* Rough fallback: poll the I/O delay port. */
    for (uint32_t i = 0; i < us; i++) {
        io_wait();
    }
}

void time_sleep_ms(uint32_t ms)
{
    uint64_t target = time_uptime_ms() + ms;

    /* Prefer the tick counter when interrupts are running. */
    if (read_flags() & (1u << 9)) {
        while (time_uptime_ms() < target) {
            cpu_halt();
        }
        return;
    }
    for (uint32_t i = 0; i < ms; i++) {
        time_udelay(1000);
    }
}
