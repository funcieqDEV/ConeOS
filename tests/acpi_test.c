#include "../src/firmware/madt.h"
#include <assert.h>
#include <stdio.h>
#include <string.h>

static uint8_t table[2048];
static size_t length;
static struct acpi_topology topology;

static void put32(size_t offset, uint32_t value) {
    for (size_t i = 0; i < 4; i++)
        table[offset + i] = value >> (8 * i);
}

static void begin(void) {
    memset(table, 0, sizeof(table));
    memcpy(table, "APIC", 4);
    put32(36, 0xFEE00000);
    put32(40, 1);
    length = 44;
}

static size_t append(const uint8_t *data, size_t size) {
    size_t offset = length;
    assert(length + size <= sizeof(table));
    memcpy(table + length, data, size);
    length += size;
    return offset;
}

static void finish(void) {
    put32(4, length);
    table[9] = 0;
    uint8_t sum = 0;
    for (size_t i = 0; i < length; i++)
        sum += table[i];
    table[9] = (uint8_t)-sum;
}

static void fixture(void) {
    begin();
    const uint8_t cpu[] = {0, 8, 0, 0, 1, 0, 0, 0};
    const uint8_t io[] = {1, 12, 1, 0, 0, 0, 0xC0, 0xFE, 0, 0, 0, 0};
    append(cpu, sizeof(cpu));
    append(io, sizeof(io));
}

static int parse(void) { return acpi_parse_madt(table, length, &topology); }

int main(void) {
    fixture();
    const uint8_t cpu2[] = {0, 8, 1, 2, 1, 0, 0, 0};
    const uint8_t disabled[] = {0, 8, 2, 3, 0, 0, 0, 0};
    const uint8_t x2[] = {9, 16, 0, 0, 0x2C, 1, 0, 0, 3, 0, 0, 0, 5, 0, 0, 0};
    const uint8_t pit[] = {2, 10, 0, 0, 2, 0, 0, 0, 0, 0};
    const uint8_t sci[] = {2, 10, 0, 9, 9, 0, 0, 0, 15, 0};
    const uint8_t nmi[] = {4, 6, 255, 0, 0, 1};
    const uint8_t x2nmi[] = {10, 12, 0, 0, 5, 0, 0, 0, 0, 0, 0, 0};
    const uint8_t ionmi[] = {3, 8, 0, 0, 20, 0, 0, 0};
    const uint8_t address[] = {5, 12, 0, 0, 0, 0, 0xE0, 0xFE, 1, 0, 0, 0};
    const uint8_t unknown[] = {127, 2};
    append(cpu2, sizeof(cpu2));
    append(disabled, sizeof(disabled));
    append(x2, sizeof(x2));
    append(pit, sizeof(pit));
    append(sci, sizeof(sci));
    append(nmi, sizeof(nmi));
    append(x2nmi, sizeof(x2nmi));
    append(ionmi, sizeof(ionmi));
    append(address, sizeof(address));
    append(unknown, sizeof(unknown));
    finish();
    assert(parse());
    assert(topology.cpu_count == 4 && topology.cpus[3].apic_id == 300);
    assert(topology.cpus[2].flags == 0 && topology.cpus[3].flags == 3);
    assert(topology.lapic_address == 0x1FEE00000ULL);
    assert(topology.ioapic_count == 1 &&
           topology.ioapics[0].address == 0xFEC00000);
    assert(topology.isa_irqs[0].gsi == 2 && topology.isa_irqs[9].flags == 15);
    assert(topology.isa_irqs[1].gsi == 1 && !topology.isa_irqs[1].overridden);
    assert(topology.local_nmi_count == 2 && topology.local_nmis[0].all_cpus);
    assert(topology.local_nmis[1].uid == 5 && topology.io_nmis[0].gsi == 20);
    assert(!acpi_parse_madt(table, length - 1, &topology));
    table[9]++;
    assert(!parse());
    fixture();
    append(pit, sizeof(pit));
    append(pit, sizeof(pit));
    finish();
    assert(!parse());
    fixture();
    size_t iso = append(pit, sizeof(pit));
    table[iso + 8] = 2;
    finish();
    assert(!parse());
    table[iso + 8] = 8;
    finish();
    assert(!parse());
    table[iso + 8] = 16;
    finish();
    assert(!parse());
    table[iso + 8] = 0;
    table[iso + 3] = 16;
    finish();
    assert(!parse());
    table[iso + 3] = 0;
    table[iso + 2] = 1;
    finish();
    assert(!parse());
    fixture();
    table[45] = 0;
    finish();
    assert(!parse());
    fixture();
    table[45] = 7;
    finish();
    assert(!parse());
    fixture();
    size_t end = append(unknown, 1);
    finish();
    assert(!parse());
    assert(end == 64);
    fixture();
    append(cpu2, sizeof(cpu2));
    append(cpu2, sizeof(cpu2));
    finish();
    assert(!parse());
    fixture();
    append(address, sizeof(address));
    append(address, sizeof(address));
    finish();
    assert(!parse());
    fixture();
    size_t local = append(nmi, sizeof(nmi));
    table[local + 5] = 2;
    finish();
    assert(!parse());
    fixture();
    put32(56, 0xFEC00001);
    finish();
    assert(!parse());
    fixture();
    for (size_t i = 1; i <= ACPI_MAX_CPUS; i++) {
        uint8_t cpu[] = {0, 8, i, i, 1, 0, 0, 0};
        append(cpu, sizeof(cpu));
    }
    finish();
    assert(!parse());
    fixture();
    finish();
    put32(4, 43);
    assert(!parse());
    assert(!acpi_parse_madt(NULL, 44, &topology));
    puts("ACPI parser tests passed");
    return 0;
}
