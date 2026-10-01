#include "madt.h"

static uint16_t read16(const uint8_t *p) {
    return p[0] | ((uint16_t)p[1] << 8);
}

static uint32_t read32(const uint8_t *p) {
    return read16(p) | ((uint32_t)read16(p + 2) << 16);
}

static uint64_t read64(const uint8_t *p) {
    return read32(p) | ((uint64_t)read32(p + 4) << 32);
}

int acpi_checksum_valid(const void *data, size_t length) {
    const uint8_t *bytes = data;
    uint8_t sum = 0;
    for (size_t i = 0; i < length; i++)
        sum += bytes[i];
    return sum == 0;
}

int acpi_interrupt_flags_valid(uint16_t flags) {
    return !(flags & ~0xFU) && (flags & 3) != 2 && ((flags >> 2) & 3) != 2;
}

static int add_cpu(struct acpi_topology *t, uint32_t uid, uint32_t id,
                   uint32_t flags) {
    if (t->cpu_count == ACPI_MAX_CPUS)
        return 0;
    for (size_t i = 0; i < t->cpu_count; i++)
        if (t->cpus[i].apic_id == id || t->cpus[i].uid == uid)
            return 0;
    t->cpus[t->cpu_count++] = (struct acpi_cpu){uid, id, flags};
    return 1;
}

int acpi_parse_madt(const void *data, size_t available,
                    struct acpi_topology *t) {
    if (!data || !t || available < 44)
        return 0;
    const uint8_t *bytes = data;
    uint32_t length = read32(bytes + 4);
    if (bytes[0] != 'A' || bytes[1] != 'P' || bytes[2] != 'I' ||
        bytes[3] != 'C' || length < 44 || length > available ||
        !acpi_checksum_valid(data, length))
        return 0;

    *t = (struct acpi_topology){0};
    t->lapic_address = read32(bytes + 36);
    t->flags = read32(bytes + 40);
    for (size_t i = 0; i < 16; i++)
        t->isa_irqs[i].gsi = i;
    int address_override = 0;
    for (size_t offset = 44; offset < length;) {
        if (length - offset < 2)
            return 0;
        const uint8_t *p = bytes + offset;
        uint8_t size = p[1];
        if (size < 2 || size > length - offset)
            return 0;
        switch (p[0]) {
        case 0:
            if (size < 8 || !add_cpu(t, p[2], p[3], read32(p + 4)))
                return 0;
            break;
        case 1: {
            if (size < 12 || t->ioapic_count == ACPI_MAX_IOAPICS)
                return 0;
            uint32_t address = read32(p + 4);
            if (!address || (address & 4095))
                return 0;
            for (size_t i = 0; i < t->ioapic_count; i++)
                if (t->ioapics[i].id == p[2] ||
                    t->ioapics[i].address == address)
                    return 0;
            t->ioapics[t->ioapic_count++] =
                (struct acpi_ioapic){address, read32(p + 8), p[2]};
            break;
        }
        case 2: {
            if (size < 10 || p[2] != 0 || p[3] >= 16 ||
                !acpi_interrupt_flags_valid(read16(p + 8)))
                return 0;
            struct acpi_isa_irq *irq = &t->isa_irqs[p[3]];
            if (irq->overridden)
                return 0;
            *irq = (struct acpi_isa_irq){read32(p + 4), read16(p + 8), 1};
            break;
        }
        case 3:
            if (size < 8 || t->io_nmi_count == ACPI_MAX_NMIS ||
                !acpi_interrupt_flags_valid(read16(p + 2)))
                return 0;
            t->io_nmis[t->io_nmi_count++] =
                (struct acpi_io_nmi){read32(p + 4), read16(p + 2)};
            break;
        case 4:
            if (size < 6 || t->local_nmi_count == ACPI_MAX_NMIS || p[5] > 1 ||
                !acpi_interrupt_flags_valid(read16(p + 3)))
                return 0;
            t->local_nmis[t->local_nmi_count++] =
                (struct acpi_local_nmi){p[2], read16(p + 3), p[5], p[2] == 255};
            break;
        case 5:
            if (size < 12 || address_override)
                return 0;
            t->lapic_address = read64(p + 4);
            address_override = 1;
            break;
        case 9:
            if (size < 16 ||
                !add_cpu(t, read32(p + 12), read32(p + 4), read32(p + 8)))
                return 0;
            break;
        case 10:
            if (size < 12 || t->local_nmi_count == ACPI_MAX_NMIS || p[8] > 1 ||
                !acpi_interrupt_flags_valid(read16(p + 2)))
                return 0;
            t->local_nmis[t->local_nmi_count++] =
                (struct acpi_local_nmi){read32(p + 4), read16(p + 2), p[8],
                                        read32(p + 4) == UINT32_MAX};
            break;
        default:
            break;
        }
        offset += size;
    }
    return t->cpu_count && t->ioapic_count && t->lapic_address &&
           !(t->lapic_address & 4095) && t->lapic_address < (1ULL << 52);
}
