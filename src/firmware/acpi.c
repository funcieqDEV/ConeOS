#include "acpi.h"
#include "../../limine/limine.h"
#include "../drivers/serial.h"
#include "../log.h"
#include "../mm/vmm.h"

#define TABLE_LIMIT (1024 * 1024)
#define WINDOW_BASE 0xFFFFB10000000000ULL
#define WINDOW_STRIDE (2 * TABLE_LIMIT)

extern uint64_t limine_base_revision[3];

__attribute__((
    used,
    section(".limine_requests"))) static volatile struct limine_rsdp_request
    rsdp_request = {
        .id = LIMINE_RSDP_REQUEST,
        .revision = 0,
};

struct rsdp {
    char signature[8];
    uint8_t checksum;
    char oem_id[6];
    uint8_t revision;
    uint32_t rsdt_address, length;
    uint64_t xsdt_address;
    uint8_t extended_checksum, reserved[3];
} __attribute__((packed));

static struct acpi_topology topology;
static int available;
static size_t window_pages[3];

static int signature_is(const char *a, const char *b, size_t size) {
    for (size_t i = 0; i < size; i++)
        if (a[i] != b[i])
            return 0;
    return 1;
}

static void unmap_window(size_t slot) {
    while (window_pages[slot])
        vmm_unmap_page(WINDOW_BASE + slot * WINDOW_STRIDE +
                       --window_pages[slot] * 4096);
}

/* Revision 3 does not map ACPI RAM into the HHDM. Use bounded, read-only slots.
 */
static void *map_table_bytes(size_t slot, uint64_t physical, size_t length) {
    unmap_window(slot);
    if (!physical || !length || length > TABLE_LIMIT ||
        physical >= (1ULL << 52) || length > (1ULL << 52) - physical)
        return NULL;
    uint64_t offset = physical & 4095;
    size_t pages = (offset + length + 4095) / 4096;
    uint64_t base = WINDOW_BASE + slot * WINDOW_STRIDE;
    for (size_t i = 0; i < pages; i++) {
        if (!vmm_map_page(base + i * 4096, (physical & ~4095ULL) + i * 4096,
                          VMM_NO_EXECUTE)) {
            unmap_window(slot);
            return NULL;
        }
        window_pages[slot]++;
    }
    return (void *)(base + offset);
}

static const struct acpi_sdt_header *map_sdt(size_t slot, uint64_t physical) {
    const struct acpi_sdt_header *header =
        map_table_bytes(slot, physical, sizeof(*header));
    if (!header)
        return NULL;
    uint32_t length = header->length;
    if (length < sizeof(*header) || length > TABLE_LIMIT)
        return NULL;
    header = map_table_bytes(slot, physical, length);
    return header && acpi_checksum_valid(header, length) ? header : NULL;
}

static int discover(void) {
    if (!rsdp_request.response || !rsdp_request.response->address)
        return 0;
    uint64_t address = (uint64_t)rsdp_request.response->address;
    uint64_t revision =
        LIMINE_LOADED_BASE_REV_VALID ? LIMINE_LOADED_BASE_REVISION : 0;
    const struct rsdp *rsdp;
    if (revision == 3)
        rsdp = map_table_bytes(0, address, sizeof(*rsdp));
    else
        rsdp = (const struct rsdp *)address;
    if (!rsdp || !signature_is(rsdp->signature, "RSD PTR ", 8) ||
        !acpi_checksum_valid(rsdp, 20))
        return 0;

    uint64_t root_physical = rsdp->rsdt_address;
    int extended = rsdp->revision >= 2;
    if (extended) {
        uint32_t length = rsdp->length;
        if (length < sizeof(*rsdp) || length > 4096)
            return 0;
        if (revision == 3)
            rsdp = map_table_bytes(0, address, length);
        if (!rsdp || !acpi_checksum_valid(rsdp, length))
            return 0;
        if (rsdp->xsdt_address)
            root_physical = rsdp->xsdt_address;
        else
            extended = 0;
    }
    const struct acpi_sdt_header *root = map_sdt(1, root_physical);
    size_t entry_size = extended ? 8 : 4;
    if (!root ||
        !signature_is(root->signature, extended ? "XSDT" : "RSDT", 4) ||
        (root->length - sizeof(*root)) % entry_size)
        return 0;
    size_t entries = (root->length - sizeof(*root)) / entry_size;
    const uint8_t *p = (const uint8_t *)(root + 1);
    for (size_t i = 0; i < entries; i++) {
        uint64_t physical = 0;
        for (size_t j = 0; j < entry_size; j++)
            physical |= (uint64_t)p[i * entry_size + j] << (8 * j);
        const struct acpi_sdt_header *table = map_sdt(2, physical);
        if (table && signature_is(table->signature, "APIC", 4))
            return acpi_parse_madt(table, table->length, &topology);
    }
    return 0;
}

int acpi_init(void) {
    available = discover();
    for (size_t i = 0; i < 3; i++)
        unmap_window(i);
    if (!available) {
        LOG_WARN("ACPI/MADT unavailable or invalid");
        return 0;
    }
    size_t enabled = 0;
    for (size_t i = 0; i < topology.cpu_count; i++)
        if (topology.cpus[i].flags & 1)
            enabled++;
    print_serial("ACPI MADT: ");
    print_uint(enabled);
    print_serial(" enabled CPUs, ");
    print_uint(topology.ioapic_count);
    print_serial(" IOAPICs\n");
    for (size_t i = 0; i < topology.cpu_count; i++) {
        print_serial("ACPI CPU: APIC ID ");
        print_uint(topology.cpus[i].apic_id);
        print_serial((topology.cpus[i].flags & 1) ? " enabled\n"
                                                  : " disabled\n");
    }
    return 1;
}

const struct acpi_topology *acpi_topology(void) {
    return available ? &topology : NULL;
}
