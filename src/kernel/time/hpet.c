/*
 * QitoOS - HPET high-resolution timer
 * Switch to APIC/HPET, microsecond timers, monotonic clock, frame pacing.
 * Games need stable frame times; 100 Hz PIT tick isn't enough.
 */

#include <kernel/hpet.h>
#include <kernel/pci.h>
#include <kernel/mm.h>
#include <kernel/log.h>
#include <kernel/io.h>
#include <kernel/time.h>

#define HPET_CFG_ENABLE (1u<<0)
#define HPET_CFG_LEG_RT (1u<<1)

struct hpet_regs {
    uint64_t cap;
    uint64_t reserved0;
    uint64_t config;
    uint64_t reserved1;
    uint64_t isr;
    uint64_t reserved2[25];
    uint64_t counter;
    uint64_t reserved3;
    // timers follow
} PACKED;

static volatile struct hpet_regs *hpet = NULL;
static uint64_t hpet_freq = 0;
static bool_t hpet_found = false;
static uint64_t tsc_freq = 0;

void hpet_init(void)
{
    // Try to find HPET via ACPI RSDP? For simplicity, check known QEMU address 0xFED00000
    // HPET is usually at 0xFED00000, mapped by firmware
    uint64_t phys = 0xFED00000;
    // Map if not already
    // QEMU's HPET is available at that address, identity mapped low 4GB includes it
    volatile struct hpet_regs *candidate = (volatile struct hpet_regs*)phys_to_virt((phys_addr_t)phys);
    // Check cap for signature: period should be non-zero and reasonable (e.g., 10000000 fs = 100ns)
    uint64_t cap = candidate->cap;
    uint32_t period = (uint32_t)(cap >> 32); // femtoseconds per tick
    if (period !=0 && period <= 100000000) {
        hpet = candidate;
        hpet_found = true;
        hpet_freq = 1000000000000000ULL / period; // Hz
        // Enable HPET
        uint64_t cfg = hpet->config;
        cfg |= HPET_CFG_ENABLE;
        hpet->config = cfg;
        KLOG_INFO("hpet","HPET found at 0x%llx, period %u fs, freq %llu Hz", (unsigned long long)phys, period, (unsigned long long)hpet_freq);
    } else {
        KLOG_INFO("hpet","HPET not found at 0xFED00000 (cap=0x%llx period=%u), using TSC fallback", (unsigned long long)cap, period);
        // Fallback: use TSC calibration from time.c
        hpet_found = false;
        // TSC freq already calibrated in time_init via PIT
    }

    // Calibrate TSC if needed
    // tsc_freq will be set from time module
}

bool_t hpet_available(void){ return hpet_found; }
uint64_t hpet_frequency(void){ return hpet_freq; }

uint64_t hpet_read(void)
{
    if (hpet_found && hpet) {
        return hpet->counter;
    } else {
        // Fallback to TSC
        return rdtsc();
    }
}

uint64_t hpet_ticks_to_ns(uint64_t ticks)
{
    if (hpet_found && hpet_freq) {
        return (ticks * 1000000000ULL) / hpet_freq;
    } else {
        uint64_t freq = time_cpu_khz() * 1000ULL;
        if (freq) return (ticks * 1000000000ULL) / freq;
        return ticks;
    }
}

uint64_t hpet_ticks_to_us(uint64_t ticks){ return hpet_ticks_to_ns(ticks)/1000; }

uint64_t time_monotonic_ns(void)
{
    return hpet_ticks_to_ns(hpet_read());
}
uint64_t time_monotonic_us(void){ return time_monotonic_ns()/1000; }
uint64_t time_monotonic_ms(void){ return time_monotonic_ns()/1000000; }

void time_udelay_hp(uint64_t us)
{
    uint64_t start = hpet_read();
    uint64_t target = us;
    if (hpet_found) {
        target = (us * hpet_freq)/1000000;
    } else {
        uint64_t f=time_cpu_khz()*1000ULL;
        if (f) target = (us * f)/1000000;
        else target = us*3000;
    }
    while ((hpet_read() - start) < target) {
        cpu_pause();
    }
}

void time_ndelay_hp(uint64_t ns)
{
    time_udelay_hp((ns+999)/1000);
}

void frame_pacer_init(struct frame_pacer *pacer, uint64_t target_fps)
{
    if (!pacer) return;
    pacer->target_fps = target_fps ? target_fps : 60;
    pacer->frame_time_ns = 1000000000ULL / pacer->target_fps;
    pacer->last_frame_ns = time_monotonic_ns();
}

void frame_pacer_wait(struct frame_pacer *pacer)
{
    if (!pacer) return;
    uint64_t now = time_monotonic_ns();
    uint64_t elapsed = now - pacer->last_frame_ns;
    if (elapsed < pacer->frame_time_ns) {
        uint64_t remaining = pacer->frame_time_ns - elapsed;
        time_ndelay_hp(remaining);
    }
    pacer->last_frame_ns = time_monotonic_ns();
}
