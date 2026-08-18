/*
 * Qira OS - pseudo-random numbers and assorted small facilities
 *
 * The generator is xoshiro256**, seeded from the timestamp counter and the
 * real-time clock. It is fast and statistically sound, but it is *not*
 * cryptographically secure and must not be used for keys or nonces.
 */
#ifndef QIRA_RANDOM_H
#define QIRA_RANDOM_H

#include <kernel/types.h>

void     random_init(void);
void     random_seed(uint64_t seed);
uint64_t random_u64(void);
uint32_t random_u32(void);

/* Uniform in [0, bound), free of modulo bias. */
uint32_t random_below(uint32_t bound);
void     random_bytes(void *out, size_t len);
uint64_t random_generated_count(void);

/* --- load average ------------------------------------------------------ */

void     load_sample(void);
uint32_t load_average_centi(void);   /* the load times 100 */

/* --- kernel symbols ---------------------------------------------------- */

struct kernel_symbol {
    uint64_t    address;
    const char *name;
};

/* Resolve an address to the symbol containing it, or NULL. */
const char *ksym_lookup(uint64_t address, uint64_t *offset);
int         ksym_count(void);

#endif /* QIRA_RANDOM_H */
