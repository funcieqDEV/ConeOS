#pragma once

#include <stddef.h>
#include <stdint.h>

#define ACPI_MAX_CPUS 64
#define ACPI_MAX_IOAPICS 8
#define ACPI_MAX_NMIS 64

struct acpi_sdt_header {
    char signature[4];
    uint32_t length;
    uint8_t revision, checksum;
    char oem_id[6], oem_table_id[8];
    uint32_t oem_revision, creator_id, creator_revision;
} __attribute__((packed));

struct acpi_cpu {
    uint32_t uid, apic_id, flags;
};

struct acpi_ioapic {
    uint32_t address, gsi_base;
    uint8_t id;
};

struct acpi_isa_irq {
    uint32_t gsi;
    uint16_t flags;
    uint8_t overridden;
};

struct acpi_local_nmi {
    uint32_t uid;
    uint16_t flags;
    uint8_t lint, all_cpus;
};

struct acpi_io_nmi {
    uint32_t gsi;
    uint16_t flags;
};

struct acpi_topology {
    uint64_t lapic_address;
    uint32_t flags;
    size_t cpu_count, ioapic_count, local_nmi_count, io_nmi_count;
    struct acpi_cpu cpus[ACPI_MAX_CPUS];
    struct acpi_ioapic ioapics[ACPI_MAX_IOAPICS];
    struct acpi_isa_irq isa_irqs[16];
    struct acpi_local_nmi local_nmis[ACPI_MAX_NMIS];
    struct acpi_io_nmi io_nmis[ACPI_MAX_NMIS];
};

int acpi_checksum_valid(const void *data, size_t length);
int acpi_interrupt_flags_valid(uint16_t flags);
/* Output is usable only when parsing succeeds. */
int acpi_parse_madt(const void *data, size_t available,
                    struct acpi_topology *topology);
