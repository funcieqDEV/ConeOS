#include "smp.h"
#include "../../limine/limine.h"
#include "../mm/pmm.h"
#include "../mm/stack.h"
#include "../mm/vmm.h"
#include "../drivers/pit.h"
#include "../log.h"
#include "apic.h"
#include "gdt.h"
#include "idt.h"
#include "irq.h"
#include <stddef.h>
#include <stdint.h>

#define SMP_START_SPIN_LIMIT 100000000ULL
#define SMP_KERNEL_STACK_PAGES 8

struct smp_cpu {
    uint32_t processor_id;
    uint32_t lapic_id;
    int requested;
    int online;
    int release;
    int initialized;
    int timer_command;
    int timer_state;
    uint64_t timer_ticks;
    struct kernel_stack stack;
    struct gdt_context *gdt;
};

static struct smp_cpu *cpus;
static size_t cpu_count;

__attribute__((used, section(".limine_requests"))) static volatile struct
    limine_smp_request smp_request = {
        .id = LIMINE_SMP_REQUEST,
        .revision = 0,
        .flags = 0,
};

__attribute__((noreturn)) void smp_secondary_main(struct smp_cpu *cpu) {
    gdt_load_secondary(cpu->gdt);
    idt_load_current();
    __atomic_store_n(&cpu->initialized, 1, __ATOMIC_RELEASE);
    int command;
    while ((command = __atomic_load_n(&cpu->timer_command,
                                      __ATOMIC_ACQUIRE)) == 0)
        __asm__ volatile("pause");
    if (command == 1) {
        int started = apic_timer_start();
        __atomic_store_n(&cpu->timer_state, started ? 1 : -1,
                         __ATOMIC_RELEASE);
        if (started)
            __asm__ volatile("sti" : : : "memory");
    }
    for (;;)
        __asm__ volatile("hlt");
}

extern void smp_switch_stack(uint64_t stack_top, struct smp_cpu *cpu)
    __attribute__((noreturn));

__asm__(".global smp_switch_stack\n"
        ".type smp_switch_stack, @function\n"
        "smp_switch_stack:\n"
        "    movq %rdi, %rsp\n"
        "    movq %rsi, %rdi\n"
        "    call smp_secondary_main\n"
        "1:  cli\n"
        "    hlt\n"
        "    jmp 1b\n"
        ".size smp_switch_stack, .-smp_switch_stack\n");

__attribute__((noreturn)) static void
smp_secondary_entry(struct limine_smp_info *info) {
    __asm__ volatile("cli" : : : "memory");
    struct smp_cpu *cpu = (struct smp_cpu *)(uintptr_t)info->extra_argument;
    if (cpu == NULL)
        for (;;)
            __asm__ volatile("hlt");
    __atomic_store_n(&cpu->online, 1, __ATOMIC_RELEASE);
    int release;
    while ((release = __atomic_load_n(&cpu->release, __ATOMIC_ACQUIRE)) == 0)
        __asm__ volatile("pause");
    if (release == 1) {
        vmm_space_activate(vmm_kernel_space());
        smp_switch_stack(cpu->stack.top, cpu);
    }
    for (;;)
        __asm__ volatile("hlt");
}

static char *append_number(char *out, uint32_t value) {
    char digits[10];
    size_t length = 0;
    do {
        digits[length++] = '0' + value % 10;
        value /= 10;
    } while (value);
    while (length)
        *out++ = digits[--length];
    return out;
}

static void log_cpu(const struct smp_cpu *cpu, const char *state,
                    log_level_t level) {
    char message[80];
    char *out = message;
    const char *text = "SMP CPU ";
    while (*text)
        *out++ = *text++;
    out = append_number(out, cpu->processor_id);
    text = " (LAPIC ";
    while (*text)
        *out++ = *text++;
    out = append_number(out, cpu->lapic_id);
    text = ") ";
    while (*text)
        *out++ = *text++;
    while (*state)
        *out++ = *state++;
    *out = '\0';
    log_msg(level, message);
}

int smp_init(void) {
    struct limine_smp_response *response = smp_request.response;
    if (response == NULL) {
        LOG_WARN("SMP response unavailable");
        return 1;
    }
    if (response->cpus == NULL || response->cpu_count == 0) {
        LOG_WARN("SMP response invalid");
        return 0;
    }
    if (response->flags & LIMINE_SMP_X2APIC) {
        LOG_WARN("SMP x2APIC mode unsupported");
        return 0;
    }

    if (response->cpu_count >
        (SIZE_MAX - (PMM_PAGE_SIZE - 1)) / sizeof(*cpus)) {
        LOG_WARN("SMP CPU count exceeds address space");
        return 0;
    }
    size_t count = (size_t)response->cpu_count;
    size_t bytes = count * sizeof(*cpus);
    size_t pages = (bytes + PMM_PAGE_SIZE - 1) / PMM_PAGE_SIZE;
    for (size_t i = 0; i < count; i++) {
        if (response->cpus[i] == NULL) {
            LOG_WARN("SMP CPU entry missing");
            return 0;
        }
    }
    uint64_t physical = pmm_alloc_contiguous(pages);
    if (physical == PMM_INVALID_ADDRESS) {
        LOG_WARN("SMP CPU data allocation failed");
        return 0;
    }
    cpus = pmm_physical_to_virtual(physical);
    cpu_count = count;
    for (size_t i = 0; i < count; i++)
        cpus[i] = (struct smp_cpu){0};

    size_t requested = 0;
    for (size_t i = 0; i < count; i++) {
        struct limine_smp_info *info = response->cpus[i];
        struct smp_cpu *cpu = &cpus[i];
        cpu->processor_id = info->processor_id;
        cpu->lapic_id = info->lapic_id;
        if (info->lapic_id == response->bsp_lapic_id) {
            cpu->online = 1;
            log_cpu(cpu, "boot processor online", LOG_INFO);
            continue;
        }
        cpu->requested = 1;
        info->extra_argument = (uint64_t)(uintptr_t)cpu;
        __atomic_store_n(&info->goto_address, smp_secondary_entry,
                         __ATOMIC_RELEASE);
        requested++;
    }

    size_t online = 0;
    for (uint64_t spin = 0; spin < SMP_START_SPIN_LIMIT; spin++) {
        online = 0;
        for (size_t i = 0; i < count; i++)
            if (cpus[i].requested &&
                __atomic_load_n(&cpus[i].online, __ATOMIC_ACQUIRE))
                online++;
        if (online == requested)
            break;
        __asm__ volatile("pause");
    }

    for (size_t i = 0; i < count; i++) {
        if (!cpus[i].requested)
            continue;
        if (__atomic_load_n(&cpus[i].online, __ATOMIC_ACQUIRE))
            log_cpu(&cpus[i], "online, idle", LOG_INFO);
        else
            log_cpu(&cpus[i], "did not start", LOG_WARN);
    }
    return online == requested;
}

int smp_prepare_aps(void) {
    if (cpu_count <= 1)
        return 1;
    for (size_t i = 0; i < cpu_count; i++) {
        struct smp_cpu *cpu = &cpus[i];
        if (!cpu->requested)
            continue;
        if (!kernel_stack_alloc(&cpu->stack, SMP_KERNEL_STACK_PAGES))
            goto fail;
        cpu->gdt = gdt_prepare_secondary(cpu->stack.top);
        if (cpu->gdt == NULL)
            goto fail;
    }

    size_t requested = 0;
    for (size_t i = 0; i < cpu_count; i++) {
        if (!cpus[i].requested)
            continue;
        __atomic_store_n(&cpus[i].release, 1, __ATOMIC_RELEASE);
        requested++;
    }
    size_t initialized = 0;
    for (uint64_t spin = 0; spin < SMP_START_SPIN_LIMIT; spin++) {
        initialized = 0;
        for (size_t i = 0; i < cpu_count; i++)
            if (cpus[i].requested &&
                __atomic_load_n(&cpus[i].initialized, __ATOMIC_ACQUIRE))
                initialized++;
        if (initialized == requested)
            break;
        __asm__ volatile("pause");
    }
    if (initialized != requested) {
        LOG_WARN("SMP CPU context initialization timed out");
        return 0;
    }
    LOG_INFO("SMP secondary stacks, GDT/TSS and IDT initialized");
    return 1;

fail:
    for (size_t i = 0; i < cpu_count; i++)
        if (cpus[i].requested)
            __atomic_store_n(&cpus[i].release, 2, __ATOMIC_RELEASE);
    LOG_WARN("SMP secondary CPU resource allocation failed");
    return 0;
}

void smp_timer_tick_current(void) {
    if (cpus == NULL)
        return;
    uint32_t lapic_id = apic_current_id();
    for (size_t i = 0; i < cpu_count; i++) {
        if (cpus[i].lapic_id != lapic_id)
            continue;
        __atomic_add_fetch(&cpus[i].timer_ticks, 1, __ATOMIC_RELAXED);
        return;
    }
}

void smp_start_timers(void) {
    if (cpu_count == 0)
        return;
    if (!irq_uses_apic()) {
        for (size_t i = 0; i < cpu_count; i++)
            if (cpus[i].requested)
                __atomic_store_n(&cpus[i].timer_command, 2,
                                 __ATOMIC_RELEASE);
        LOG_INFO("SMP timers unavailable in PIC mode");
        return;
    }

    size_t requested = 0;
    for (size_t i = 0; i < cpu_count; i++) {
        if (!cpus[i].requested)
            continue;
        __atomic_store_n(&cpus[i].timer_command, 1, __ATOMIC_RELEASE);
        requested++;
    }
    int boot_timer_started = apic_timer_start();
    size_t finished = 0;
    for (uint64_t spin = 0; spin < SMP_START_SPIN_LIMIT; spin++) {
        finished = 0;
        for (size_t i = 0; i < cpu_count; i++)
            if (cpus[i].requested &&
                __atomic_load_n(&cpus[i].timer_state, __ATOMIC_ACQUIRE) != 0)
                finished++;
        if (finished == requested)
            break;
        __asm__ volatile("pause");
    }

    uint64_t start = pit_ticks();
    for (uint64_t spin = 0; spin < SMP_START_SPIN_LIMIT &&
                            pit_ticks() - start < 5; spin++)
        __asm__ volatile("pause");
    for (size_t i = 0; i < cpu_count; i++) {
        struct smp_cpu *cpu = &cpus[i];
        int started = cpu->requested
                          ? __atomic_load_n(&cpu->timer_state, __ATOMIC_ACQUIRE)
                          : boot_timer_started;
        if (started == 1 &&
            __atomic_load_n(&cpu->timer_ticks, __ATOMIC_RELAXED) != 0)
            log_cpu(cpu, "local APIC timer active", LOG_INFO);
        else
            log_cpu(cpu, "local APIC timer unavailable", LOG_WARN);
    }
}
