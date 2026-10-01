#pragma once

int cpu_enable_nx(void);
void cpu_enable_memory_protection(void);

/* User RFLAGS.AC survives an interrupt gate; never inherit its SMAP override.
 */
static inline void cpu_clear_access_override(void) {
    __asm__ volatile("pushfq; andq $-262145, (%%rsp); popfq"
                     :
                     :
                     : "memory", "cc");
}
