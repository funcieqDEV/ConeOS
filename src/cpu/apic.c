#include "apic.h"
#include "../drivers/serial.h"
#include "../firmware/acpi.h"
#include "../drivers/pit.h"
#include "../log.h"
#include "../mm/vmm.h"

#define MMIO_BASE 0xFFFFB00000000000ULL
#define IA32_APIC_BASE 0x1B
#define APIC_ENABLE (1ULL << 11)
#define APIC_X2_MODE (1ULL << 10)
#define APIC_PHYSICAL_MASK 0x000FFFFFFFFFF000ULL
#define LAPIC_ID 0x20
#define LAPIC_VERSION 0x30
#define LAPIC_TPR 0x80
#define LAPIC_EOI 0xB0
#define LAPIC_SVR 0xF0
#define LAPIC_ESR 0x280
#define LAPIC_LVT_TIMER 0x320
#define LAPIC_TIMER_INITIAL 0x380
#define LAPIC_TIMER_CURRENT 0x390
#define LAPIC_TIMER_DIVIDE 0x3E0
#define LAPIC_LVT_LINT0 0x350
#define LAPIC_LVT_LINT1 0x360
#define LAPIC_MASKED (1U << 16)
#define LAPIC_TIMER_PERIODIC (1U << 17)
#define LAPIC_TIMER_DIVIDE_16 0x3
#define LAPIC_CALIBRATION_TICKS 10
#define LAPIC_CALIBRATION_SPINS 100000000ULL
#define IOAPIC_REDTBL 0x10
#define NMI_DELIVERY (4U << 8)

struct ioapic {
    volatile uint32_t *registers;
    uint32_t gsi_base, pin_count;
};

struct irq_route {
    struct ioapic *controller;
    uint32_t pin;
};

static volatile uint32_t *lapic;
static struct ioapic controllers[ACPI_MAX_IOAPICS];
static struct irq_route routes[16];
static size_t controller_count;

static void set_apic_base(uint64_t value) {
    __asm__ volatile("wrmsr"
                     :
                     : "c"(IA32_APIC_BASE), "a"((uint32_t)value),
                       "d"((uint32_t)(value >> 32))
                     : "memory");
}

static uint32_t lapic_read(uint32_t offset) { return lapic[offset / 4]; }

static void lapic_write(uint32_t offset, uint32_t value) {
    lapic[offset / 4] = value;
    (void)lapic_read(LAPIC_ID);
}

static uint32_t io_read(struct ioapic *io, uint32_t reg) {
    io->registers[0] = reg;
    return io->registers[4];
}

static void io_write(struct ioapic *io, uint32_t reg, uint32_t value) {
    io->registers[0] = reg;
    io->registers[4] = value;
}

static struct irq_route route_gsi(uint32_t gsi) {
    for (size_t i = 0; i < controller_count; i++)
        if (gsi >= controllers[i].gsi_base &&
            gsi - controllers[i].gsi_base < controllers[i].pin_count)
            return (struct irq_route){&controllers[i],
                                      gsi - controllers[i].gsi_base};
    return (struct irq_route){0};
}

static uint32_t signal_flags(uint16_t flags) {
    uint32_t result = 0;
    if ((flags & 3) == 3)
        result |= 1U << 13;
    if (((flags >> 2) & 3) == 3)
        result |= 1U << 15;
    return result;
}

static void io_route(struct irq_route route, uint32_t low,
                     uint8_t destination) {
    uint32_t reg = IOAPIC_REDTBL + 2 * route.pin;
    /* Never expose half of a destination/vector update to an interrupt. */
    io_write(route.controller, reg,
             io_read(route.controller, reg) | LAPIC_MASKED);
    io_write(route.controller, reg + 1, (uint32_t)destination << 24);
    io_write(route.controller, reg, low);
}

static void *map_registers(uint64_t physical, size_t slot) {
    if (!physical || (physical & 4095) || physical >= (1ULL << 52))
        return NULL;
    uint64_t address = MMIO_BASE + slot * 4096;
    if (!vmm_map_page(address, physical,
                      VMM_WRITABLE | VMM_NO_EXECUTE | VMM_CACHE_DISABLE |
                          VMM_WRITE_THROUGH))
        return NULL;
    return (void *)address;
}

static void release_mappings(void) {
    for (size_t i = 0; i < controller_count; i++)
        vmm_unmap_page(MMIO_BASE + (i + 1) * 4096);
    if (lapic)
        vmm_unmap_page(MMIO_BASE);
    lapic = NULL;
    controller_count = 0;
}

static int prepare_controllers(const struct acpi_topology *t) {
    lapic = map_registers(t->lapic_address, 0);
    if (!lapic)
        return 0;
    uint32_t lapic_version = lapic_read(LAPIC_VERSION);
    if (lapic_version == UINT32_MAX || (lapic_version & 255) < 0x10 ||
        ((lapic_version >> 16) & 255) < 3)
        return 0;
    for (size_t i = 0; i < t->ioapic_count; i++) {
        struct ioapic *io = &controllers[i];
        io->registers = map_registers(t->ioapics[i].address, i + 1);
        if (!io->registers)
            return 0;
        controller_count++;
        uint32_t version = io_read(io, 1);
        io->gsi_base = t->ioapics[i].gsi_base;
        io->pin_count = ((version >> 16) & 255) + 1;
        if (version == UINT32_MAX || !(version & 255) || io->pin_count > 120 ||
            io->gsi_base > UINT32_MAX - (io->pin_count - 1))
            return 0;
        for (size_t j = 0; j < i; j++) {
            uint64_t end = (uint64_t)io->gsi_base + io->pin_count;
            uint64_t other_end =
                (uint64_t)controllers[j].gsi_base + controllers[j].pin_count;
            if (io->gsi_base < other_end && controllers[j].gsi_base < end)
                return 0;
        }
    }
    for (size_t i = 0; i < 16; i++) {
        routes[i] = (struct irq_route){0};
        /* IRQ2 is the PIC cascade, not an independent ISA source in APIC mode.
         */
        if (i == 2)
            continue;
        routes[i] = route_gsi(t->isa_irqs[i].gsi);
        if (!routes[i].controller)
            continue;
        for (size_t j = 0; j < i; j++)
            if (routes[j].controller &&
                t->isa_irqs[i].gsi == t->isa_irqs[j].gsi)
                return 0;
    }
    if (!routes[0].controller || !routes[1].controller)
        return 0;
    for (size_t i = 0; i < t->io_nmi_count; i++) {
        if (!route_gsi(t->io_nmis[i].gsi).controller)
            return 0;
        for (size_t j = 0; j < 16; j++)
            if (routes[j].controller && t->isa_irqs[j].gsi == t->io_nmis[i].gsi)
                return 0;
        for (size_t j = 0; j < i; j++)
            if (t->io_nmis[j].gsi == t->io_nmis[i].gsi)
                return 0;
    }
    return 1;
}

int apic_init(void) {
    const struct acpi_topology *t = acpi_topology();
    if (!t)
        return 0;
    uint32_t eax, ebx, ecx, edx;
    __asm__ volatile("cpuid"
                     : "=a"(eax), "=b"(ebx), "=c"(ecx), "=d"(edx)
                     : "a"(1), "c"(0));
    if (!(edx & (1U << 9))) {
        LOG_WARN("CPU APIC unavailable");
        return 0;
    }
    uint32_t low, high;
    __asm__ volatile("rdmsr" : "=a"(low), "=d"(high) : "c"(IA32_APIC_BASE));
    uint64_t base = ((uint64_t)high << 32) | low;
    if ((base & APIC_X2_MODE) ||
        (base & APIC_PHYSICAL_MASK) != t->lapic_address) {
        LOG_WARN("APIC mode or physical base unsupported");
        return 0;
    }
    if (!(base & APIC_ENABLE))
        set_apic_base(base | APIC_ENABLE);
    if (!prepare_controllers(t)) {
        LOG_WARN("APIC topology or MMIO unavailable");
        goto fail;
    }
    uint8_t id = lapic_read(LAPIC_ID) >> 24;
    const struct acpi_cpu *bsp = NULL;
    for (size_t i = 0; i < t->cpu_count; i++)
        if (t->cpus[i].apic_id == id && (t->cpus[i].flags & 1))
            bsp = &t->cpus[i];
    if (!bsp) {
        LOG_WARN("boot CPU missing from MADT");
        goto fail;
    }
    uint32_t lint[2] = {LAPIC_MASKED, LAPIC_MASKED};
    for (size_t i = 0; i < t->local_nmi_count; i++) {
        const struct acpi_local_nmi *nmi = &t->local_nmis[i];
        if (nmi->all_cpus || nmi->uid == bsp->uid) {
            uint32_t value = NMI_DELIVERY | signal_flags(nmi->flags);
            if (lint[nmi->lint] != LAPIC_MASKED && lint[nmi->lint] != value) {
                LOG_WARN("conflicting local APIC NMI entries");
                goto fail;
            }
            lint[nmi->lint] = value;
        }
    }

    /* Commit only after all required ISA routes and NMI entries are validated.
     */
    lapic_write(LAPIC_TPR, 0);
    uint32_t max_lvt = (lapic_read(LAPIC_VERSION) >> 16) & 255;
    lapic_write(LAPIC_LVT_TIMER, LAPIC_MASKED);
    lapic_write(LAPIC_LVT_LINT0, LAPIC_MASKED);
    lapic_write(LAPIC_LVT_LINT1, LAPIC_MASKED);
    if (max_lvt >= 3)
        lapic_write(0x370, LAPIC_MASKED);
    if (max_lvt >= 4)
        lapic_write(0x340, LAPIC_MASKED);
    if (max_lvt >= 5)
        lapic_write(0x330, LAPIC_MASKED);
    if (max_lvt >= 6)
        lapic_write(0x2F0, LAPIC_MASKED);
    lapic_write(LAPIC_ESR, 0);
    lapic_write(LAPIC_SVR, 0x100 | 0xFF);
    for (size_t i = 0; i < controller_count; i++)
        for (uint32_t pin = 0; pin < controllers[i].pin_count; pin++)
            io_write(&controllers[i], IOAPIC_REDTBL + 2 * pin,
                     io_read(&controllers[i], IOAPIC_REDTBL + 2 * pin) |
                         LAPIC_MASKED);
    for (size_t i = 0; i < 16; i++)
        if (routes[i].controller)
            io_route(routes[i],
                     LAPIC_MASKED | (0x20 + i) |
                         signal_flags(t->isa_irqs[i].flags),
                     id);
    for (size_t i = 0; i < t->io_nmi_count; i++)
        io_route(route_gsi(t->io_nmis[i].gsi),
                 NMI_DELIVERY | signal_flags(t->io_nmis[i].flags), id);
    lapic_write(LAPIC_LVT_LINT0, lint[0]);
    lapic_write(LAPIC_LVT_LINT1, lint[1]);
    LOG_INFO("Local APIC and IOAPIC initialized (xAPIC)");
    print_serial("APIC boot CPU ID: ");
    print_uint(id);
    print_serial("\n");
    print_serial("APIC ISA routes: IRQ0 -> GSI ");
    print_uint(t->isa_irqs[0].gsi);
    print_serial(", IRQ1 -> GSI ");
    print_uint(t->isa_irqs[1].gsi);
    print_serial("\n");
    return 1;
fail:
    release_mappings();
    if (!(base & APIC_ENABLE))
        set_apic_base(base);
    return 0;
}

void apic_eoi(void) { lapic_write(LAPIC_EOI, 0); }

uint32_t apic_current_id(void) { return lapic_read(LAPIC_ID) >> 24; }

static int wait_for_pit_ticks(uint64_t start, uint64_t amount) {
    for (uint64_t spin = 0; spin < LAPIC_CALIBRATION_SPINS; spin++) {
        if (pit_ticks() - start >= amount)
            return 1;
        __asm__ volatile("pause");
    }
    return 0;
}

int apic_timer_start(void) {
    if (lapic == NULL)
        return 0;
    uint32_t low, high;
    __asm__ volatile("rdmsr" : "=a"(low), "=d"(high) : "c"(IA32_APIC_BASE));
    uint64_t base = ((uint64_t)high << 32) | low;
    if (base & APIC_X2_MODE)
        return 0;
    if (!(base & APIC_ENABLE))
        set_apic_base(base | APIC_ENABLE);

    lapic_write(LAPIC_TPR, 0);
    lapic_write(LAPIC_SVR, 0x100 | 0xFF);
    lapic_write(LAPIC_LVT_TIMER, LAPIC_MASKED | APIC_TIMER_VECTOR);
    lapic_write(LAPIC_TIMER_DIVIDE, LAPIC_TIMER_DIVIDE_16);
    lapic_write(LAPIC_TIMER_INITIAL, 0);

    if (!wait_for_pit_ticks(pit_ticks(), 1))
        return 0;
    uint64_t start = pit_ticks();
    lapic_write(LAPIC_TIMER_INITIAL, UINT32_MAX);
    if (!wait_for_pit_ticks(start, LAPIC_CALIBRATION_TICKS)) {
        lapic_write(LAPIC_TIMER_INITIAL, 0);
        return 0;
    }
    uint32_t elapsed = UINT32_MAX - lapic_read(LAPIC_TIMER_CURRENT);
    lapic_write(LAPIC_TIMER_INITIAL, 0);
    uint32_t period = elapsed / LAPIC_CALIBRATION_TICKS;
    if (period < 16)
        return 0;
    lapic_write(LAPIC_LVT_TIMER, LAPIC_TIMER_PERIODIC | APIC_TIMER_VECTOR);
    lapic_write(LAPIC_TIMER_INITIAL, period);
    return 1;
}

void apic_set_irq_mask(uint8_t irq, int masked) {
    if (irq >= 16 || !routes[irq].controller)
        return;
    struct irq_route route = routes[irq];
    uint32_t reg = IOAPIC_REDTBL + 2 * route.pin;
    uint32_t value = io_read(route.controller, reg);
    io_write(route.controller, reg,
             masked ? value | LAPIC_MASKED : value & ~LAPIC_MASKED);
}
