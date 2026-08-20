/*
 * QitoOS - timekeeping
 */
#ifndef QITO_TIME_H
#define QITO_TIME_H

#include <kernel/types.h>

/* Programmable Interval Timer frequency used for the scheduler tick. */
#define TIMER_HZ 100

struct qito_time {
    int year;
    int month;   /* 1-12 */
    int day;     /* 1-31 */
    int hour;
    int minute;
    int second;
    int weekday; /* 0 = Sunday */
};

void     time_init(void);
uint64_t time_ticks(void);
uint64_t time_uptime_ms(void);
uint64_t time_uptime_us(void);

/* Busy-wait helpers; safe before the scheduler is running. */
void time_sleep_ms(uint32_t ms);
void time_udelay(uint32_t us);

/* CMOS real-time clock. */
void     rtc_read(struct qito_time *out);
uint64_t rtc_unix_time(void);

/* Convert a Unix timestamp to broken-down UTC. */
void time_from_unix(uint64_t unix_time, struct qito_time *out);
uint64_t time_to_unix(const struct qito_time *t);

/* Formatted "YYYY-MM-DD HH:MM:SS" into buf. */
void time_format(const struct qito_time *t, char *buf, size_t size);

/* Estimated CPU frequency in kHz, measured against the PIT at boot. */
uint64_t time_cpu_khz(void);

void timer_tick(void);

#endif /* QITO_TIME_H */
