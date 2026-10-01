#include "pci.h"
#include "../log.h"

#define PCI_CONFIG_ADDRESS 0xCF8
#define PCI_CONFIG_DATA 0xCFC

static struct pci_device_info devices[PCI_MAX_DEVICES];
static size_t device_count;

static void outl(uint16_t port, uint32_t value) {
    __asm__ volatile("outl %0, %1" : : "a"(value), "Nd"(port));
}
static uint32_t inl(uint16_t port) {
    uint32_t value;
    __asm__ volatile("inl %1, %0" : "=a"(value) : "Nd"(port));
    return value;
}
uint32_t pci_config_read32(uint8_t bus, uint8_t slot, uint8_t function,
                           uint8_t offset) {
    uint32_t address = 0x80000000U | ((uint32_t)bus << 16) |
                       ((uint32_t)slot << 11) | ((uint32_t)function << 8) |
                       (offset & 0xFC);
    outl(PCI_CONFIG_ADDRESS, address);
    return inl(PCI_CONFIG_DATA);
}

void pci_config_write16(uint8_t bus, uint8_t slot, uint8_t function,
                        uint8_t offset, uint16_t value) {
    uint32_t address = 0x80000000U | ((uint32_t)bus << 16) |
                       ((uint32_t)slot << 11) | ((uint32_t)function << 8) |
                       (offset & 0xFC);
    outl(PCI_CONFIG_ADDRESS, address);
    __asm__ volatile("outw %0, %1"
                     :
                     : "a"(value),
                       "Nd"((uint16_t)(PCI_CONFIG_DATA + (offset & 2))));
}

void pci_init(void) {
    device_count = 0;
    for (uint16_t bus = 0; bus < 256; bus++) {
        for (uint8_t slot = 0; slot < 32; slot++) {
            for (uint8_t function = 0; function < 8; function++) {
                uint32_t identity = pci_config_read32(bus, slot, function, 0);
                uint16_t vendor = identity & 0xFFFF;
                if (vendor == 0xFFFF) {
                    if (function == 0)
                        break;
                    continue;
                }
                uint32_t class_data = pci_config_read32(bus, slot, function, 8);
                if (device_count < PCI_MAX_DEVICES) {
                    devices[device_count++] = (struct pci_device_info){
                        .vendor_id = vendor,
                        .device_id = identity >> 16,
                        .class_code = class_data >> 24,
                        .subclass = class_data >> 16,
                        .bus = bus,
                        .slot = slot,
                        .function = function,
                    };
                }
                uint8_t header =
                    pci_config_read32(bus, slot, function, 12) >> 16;
                if (function == 0 && !(header & 0x80))
                    break;
            }
        }
    }
    LOG_INFO("PCI enumeration initialized");
}

size_t pci_device_count(void) { return device_count; }
int pci_get_device(size_t index, struct pci_device_info *info) {
    if (index >= device_count || info == NULL)
        return 0;
    *info = devices[index];
    return 1;
}
