/*
 * QitoOS - HPET and APIC high-resolution timing
 */

#ifndef QITO_HPET_H
#define QITO_HPET_H

#include <kernel/types.h>

void hpet_init(void);
uint64_t hpet_read(void); // ticks
uint64_t hpet_ticks_to_ns(uint64_t ticks);
uint64_t hpet_ticks_to_us(uint64_t ticks);
uint64_t hpet_frequency(void);
bool_t hpet_available(void);

uint64_t time_monotonic_ns(void);
uint64_t time_monotonic_us(void);
uint64_t time_monotonic_ms(void);

void time_udelay_hp(uint64_t us);
void time_ndelay_hp(uint64_t ns);

/* Frame pacing */
struct frame_pacer {
    uint64_t frame_time_ns;
    uint64_t last_frame_ns;
    uint64_t target_fps;
};

void frame_pacer_init(struct frame_pacer *pacer, uint64_t target_fps);
void frame_pacer_wait(struct frame_pacer *pacer);

#endif
