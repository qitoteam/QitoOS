/*
 * QitoOS - spinlocks
 *
 * Qito currently runs the kernel on a single CPU, but locks are still taken so
 * that interrupt handlers cannot observe half-updated state, and so the code
 * is ready for SMP.
 */
#ifndef QITO_SPINLOCK_H
#define QITO_SPINLOCK_H

#include <kernel/types.h>
#include <kernel/io.h>

typedef struct spinlock {
    volatile uint32_t locked;
    const char       *name;
    uint64_t          contentions;
} spinlock_t;

#define SPINLOCK_INIT(n) {0, (n), 0}

INLINE void spinlock_init(spinlock_t *lock, const char *name)
{
    lock->locked      = 0;
    lock->name        = name;
    lock->contentions = 0;
}

/*
 * Acquire the lock with interrupts disabled.
 * Returns the previous interrupt state, to be handed back to release().
 */
INLINE bool_t spinlock_acquire(spinlock_t *lock)
{
    bool_t enabled = irq_save();

    while (__atomic_exchange_n(&lock->locked, 1, __ATOMIC_ACQUIRE)) {
        lock->contentions++;
        while (__atomic_load_n(&lock->locked, __ATOMIC_RELAXED)) {
            cpu_pause();
        }
    }
    return enabled;
}

INLINE void spinlock_release(spinlock_t *lock, bool_t irq_state)
{
    __atomic_store_n(&lock->locked, 0, __ATOMIC_RELEASE);
    irq_restore(irq_state);
}

INLINE bool_t spinlock_try_acquire(spinlock_t *lock)
{
    return __atomic_exchange_n(&lock->locked, 1, __ATOMIC_ACQUIRE) == 0;
}

#endif /* QITO_SPINLOCK_H */
