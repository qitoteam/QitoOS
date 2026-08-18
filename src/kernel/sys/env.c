/*
 * Qira OS - additional system facilities
 *
 * A group of small subsystems that are each too slight for a file of their
 * own but that a real system is expected to have:
 *
 *   - a pseudo-random number generator seeded from real entropy
 *   - uptime and load tracking
 *   - a kernel symbol table for readable backtraces
 *   - a boot-time environment block
 */

#include <kernel/types.h>
#include <kernel/string.h>
#include <kernel/log.h>
#include <kernel/time.h>
#include <kernel/io.h>
#include <kernel/sched.h>
#include <kernel/mm.h>
#include <kernel/random.h>
#include <kernel/sched.h>

/* --- random ------------------------------------------------------------ */

/*
 * xoshiro256**, which is small, fast and has good statistical properties.
 * This is not a cryptographic generator and is documented as such wherever a
 * caller could mistake it for one.
 */
static uint64_t state[4];
static bool_t   seeded;
static uint64_t generated;

static inline uint64_t rotate_left(uint64_t value, int count)
{
    return (value << count) | (value >> (64 - count));
}

static uint64_t splitmix64(uint64_t *seed)
{
    uint64_t result = (*seed += 0x9E3779B97F4A7C15ull);
    result = (result ^ (result >> 30)) * 0xBF58476D1CE4E5B9ull;
    result = (result ^ (result >> 27)) * 0x94D049BB133111EBull;
    return result ^ (result >> 31);
}

void random_seed(uint64_t seed)
{
    uint64_t mixer = seed;
    for (int i = 0; i < 4; i++) {
        state[i] = splitmix64(&mixer);
    }
    seeded = true;
}

void random_init(void)
{
    /*
     * Entropy sources available before userspace: the timestamp counter, the
     * real-time clock, and how long the boot took. None is strong on its own;
     * mixed together they are enough to make each boot differ.
     */
    uint64_t entropy = rdtsc();
    entropy ^= rtc_unix_time() * 0x100000001B3ull;
    entropy ^= time_uptime_us() << 17;

    /* The low bits of successive TSC reads vary with bus timing. */
    for (int i = 0; i < 16; i++) {
        io_wait();
        entropy = rotate_left(entropy, 7) ^ rdtsc();
    }

    random_seed(entropy);
    KLOG_INFO("random", "generator seeded (not cryptographically secure)");
}

uint64_t random_u64(void)
{
    if (!seeded) {
        random_init();
    }

    uint64_t result = rotate_left(state[1] * 5, 7) * 9;
    uint64_t t      = state[1] << 17;

    state[2] ^= state[0];
    state[3] ^= state[1];
    state[1] ^= state[2];
    state[0] ^= state[3];
    state[2] ^= t;
    state[3] = rotate_left(state[3], 45);

    generated++;
    return result;
}

uint32_t random_u32(void)
{
    return (uint32_t)(random_u64() >> 32);
}

/* Uniform in [0, bound), without the modulo bias a plain % would introduce. */
uint32_t random_below(uint32_t bound)
{
    if (bound == 0) {
        return 0;
    }

    uint32_t threshold = (uint32_t)(-(int32_t)bound) % bound;
    for (;;) {
        uint32_t value = random_u32();
        if (value >= threshold) {
            return value % bound;
        }
    }
}

void random_bytes(void *out, size_t len)
{
    uint8_t *bytes = (uint8_t *)out;
    size_t   index = 0;

    while (index < len) {
        uint64_t value = random_u64();
        size_t   chunk = MIN(len - index, sizeof(value));
        memcpy(bytes + index, &value, chunk);
        index += chunk;
    }
}

uint64_t random_generated_count(void)
{
    return generated;
}

/* --- load average ------------------------------------------------------ */

/*
 * A one-minute load figure, sampled from the run queue. It is an exponential
 * moving average in fixed point, the same shape as the familiar Unix figure,
 * although Qira samples it far less often.
 */
#define LOAD_SHIFT 11
#define LOAD_ONE   (1 << LOAD_SHIFT)
/* exp(-5/60) in fixed point: the decay for a 5-second sample over a minute. */
#define LOAD_DECAY 1884

static uint32_t load_average;
static uint64_t last_sample_ms;

void load_sample(void)
{
    uint64_t now = time_uptime_ms();
    if (now - last_sample_ms < 5000) {
        return;
    }
    last_sample_ms = now;

    uint32_t runnable = (uint32_t)sched_runnable_count();

    load_average = (uint32_t)((load_average * (uint64_t)LOAD_DECAY +
                               (uint64_t)runnable * LOAD_ONE *
                                   (LOAD_ONE - LOAD_DECAY)) /
                              (LOAD_ONE * (uint64_t)1));
    /* Keep the value in a sane range even if a sample is wild. */
    if (load_average > LOAD_ONE * 64) {
        load_average = LOAD_ONE * 64;
    }
}

/* Return the load scaled by 100, e.g. 125 means 1.25. */
uint32_t load_average_centi(void)
{
    return (load_average * 100) / LOAD_ONE;
}

/* --- kernel symbols ---------------------------------------------------- */

/*
 * A symbol table lets a panic print "fault in heap_free+0x1c" rather than a
 * bare address, which is the difference between a usable bug report and a
 * hex dump. The table is generated at build time from the linked kernel.
 */
extern const struct kernel_symbol __ksym_start[] __attribute__((weak));
extern const struct kernel_symbol __ksym_end[] __attribute__((weak));

const char *ksym_lookup(uint64_t address, uint64_t *offset)
{
    if (!__ksym_start || &__ksym_start[0] == &__ksym_end[0]) {
        return NULL;
    }

    const struct kernel_symbol *best = NULL;

    for (const struct kernel_symbol *symbol = __ksym_start;
         symbol < __ksym_end; symbol++) {
        if (symbol->address <= address &&
            (!best || symbol->address > best->address)) {
            best = symbol;
        }
    }

    if (!best) {
        return NULL;
    }
    if (offset) {
        *offset = address - best->address;
    }
    return best->name;
}

int ksym_count(void)
{
    if (!__ksym_start || &__ksym_start[0] == &__ksym_end[0]) {
        return 0;
    }
    return (int)(__ksym_end - __ksym_start);
}
