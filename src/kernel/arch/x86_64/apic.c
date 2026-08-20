/*
 * QitoOS - APIC driver
 * Switch to APIC/HPET, high-resolution timing, monotonic clock, frame pacing
 */

#include <kernel/apic.h>
#include <kernel/log.h>
#include <kernel/io.h>
#include <kernel/cpu.h>
#include <kernel/mm.h>

static bool_t apic_present=false;

#define LAPIC_BASE_MSR 0x1B
#define LAPIC_REG_ID 0x20
#define LAPIC_REG_EOI 0xB0
#define LAPIC_REG_SPURIOUS 0xF0
#define LAPIC_REG_TIMER_INITIAL 0x380
#define LAPIC_REG_TIMER_CURRENT 0x390
#define LAPIC_REG_TIMER_DIV 0x3E0
#define LAPIC_REG_LVT_TIMER 0x320

static uint64_t lapic_base=0xFEE00000ULL;

static inline uint32_t lapic_read(uint32_t reg)
{
    return *(volatile uint32_t*)(phys_to_virt((phys_addr_t)(lapic_base + reg)));
}
static inline void lapic_write(uint32_t reg, uint32_t val)
{
    *(volatile uint32_t*)(phys_to_virt((phys_addr_t)(lapic_base + reg))) = val;
}

void apic_init(void)
{
    const struct cpu_info *cpu=cpu_get_info();
    if (!cpu || !cpu->has_apic) {
        KLOG_INFO("apic","APIC not present, staying on PIC");
        return;
    }
    // Read LAPIC base from MSR
    uint64_t msr_low, msr_high;
    __asm__ volatile("rdmsr" : "=a"(msr_low), "=d"(msr_high) : "c"(LAPIC_BASE_MSR));
    lapic_base = (msr_low & 0xFFFFF000ULL) | ((uint64_t)msr_high << 32);
    apic_present=true;

    // Enable APIC via spurious vector
    uint32_t sv = lapic_read(LAPIC_REG_SPURIOUS);
    lapic_write(LAPIC_REG_SPURIOUS, sv | 0x100 | 0xFF); // enable + spurious vector 0xFF

    KLOG_INFO("apic","Local APIC at 0x%llx, enabled (PIC still for legacy IRQs, APIC for timing)", (unsigned long long)lapic_base);
}

bool_t apic_available(void){ return apic_present; }

void apic_eoi(void)
{
    if (apic_present) lapic_write(LAPIC_REG_EOI,0);
}

uint64_t apic_ticks(void)
{
    if (!apic_present) return 0;
    return (uint64_t)lapic_read(LAPIC_REG_TIMER_CURRENT);
}

void apic_set_timer(uint64_t us)
{
    if (!apic_present) return;
    // Set divider to 16
    lapic_write(LAPIC_REG_TIMER_DIV, 0x3);
    // Set initial count based on us (approximation)
    uint32_t count = (uint32_t)(us * 100); // heuristic
    lapic_write(LAPIC_REG_TIMER_INITIAL, count);
    KLOG_DEBUG("apic","timer set for %llu us, count %u", (unsigned long long)us, count);
}
