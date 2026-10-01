#include "protection.h"
#include "../log.h"
#include <stdint.h>

#define IA32_EFER 0xC0000080U
#define EFER_NXE (1ULL << 11)
#define CR0_WP (1ULL << 16)
#define CR4_SMEP (1ULL << 20)
#define CR4_SMAP (1ULL << 21)

static void cpuid(uint32_t leaf, uint32_t *eax, uint32_t *ebx,
                  uint32_t *ecx, uint32_t *edx) {
    __asm__ volatile("cpuid" : "=a"(*eax), "=b"(*ebx), "=c"(*ecx),
                     "=d"(*edx) : "a"(leaf), "c"(0));
}

int cpu_enable_nx(void) {
    uint32_t eax, ebx, ecx, edx;
    cpuid(0x80000000U, &eax, &ebx, &ecx, &edx);
    if (eax < 0x80000001U)
        return 0;
    cpuid(0x80000001U, &eax, &ebx, &ecx, &edx);
    if (!(edx & (1U << 20)))
        return 0;

    uint32_t low, high;
    __asm__ volatile("rdmsr" : "=a"(low), "=d"(high) : "c"(IA32_EFER));
    uint64_t efer = ((uint64_t)high << 32) | low | EFER_NXE;
    __asm__ volatile("wrmsr" : : "c"(IA32_EFER), "a"((uint32_t)efer),
                     "d"((uint32_t)(efer >> 32)) : "memory");
    LOG_INFO("CPU execute-disable enabled");
    return 1;
}

void cpu_enable_memory_protection(void) {
    cpu_clear_access_override();
    uint64_t cr0, cr4;
    __asm__ volatile("mov %%cr0, %0" : "=r"(cr0));
    cr0 |= CR0_WP;
    __asm__ volatile("mov %0, %%cr0" : : "r"(cr0) : "memory");
    LOG_INFO("CPU supervisor write protection enabled");

    uint32_t eax, ebx, ecx, edx;
    cpuid(0, &eax, &ebx, &ecx, &edx);
    if (eax < 7) {
        LOG_INFO("CPU SMEP unavailable");
        LOG_INFO("CPU SMAP unavailable");
        return;
    }
    cpuid(7, &eax, &ebx, &ecx, &edx);
    __asm__ volatile("mov %%cr4, %0" : "=r"(cr4));
    if (ebx & (1U << 7)) cr4 |= CR4_SMEP;
    if (ebx & (1U << 20)) cr4 |= CR4_SMAP;
    __asm__ volatile("mov %0, %%cr4" : : "r"(cr4) : "memory");
    LOG_INFO((ebx & (1U << 7)) ? "CPU SMEP enabled" : "CPU SMEP unavailable");
    LOG_INFO((ebx & (1U << 20)) ? "CPU SMAP enabled" : "CPU SMAP unavailable");
}
