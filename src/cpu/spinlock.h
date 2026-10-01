#pragma once

#include <stdint.h>

typedef struct {
    uint32_t held;
} spinlock_t;

#define SPINLOCK_INITIALIZER {0}

static inline uint64_t spin_lock_irqsave(spinlock_t *lock) {
    uint64_t flags;
    __asm__ volatile("pushfq; pop %0; cli" : "=r"(flags) : : "memory");
    while (__atomic_exchange_n(&lock->held, 1, __ATOMIC_ACQUIRE)) {
        while (__atomic_load_n(&lock->held, __ATOMIC_RELAXED))
            __asm__ volatile("pause" : : : "memory");
    }
    return flags;
}

static inline void spin_unlock_irqrestore(spinlock_t *lock, uint64_t flags) {
    __atomic_store_n(&lock->held, 0, __ATOMIC_RELEASE);
    __asm__ volatile("push %0; popfq" : : "r"(flags) : "memory", "cc");
}
