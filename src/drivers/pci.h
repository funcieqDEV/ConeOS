#pragma once
#include <stddef.h>
#include <stdint.h>

#define PCI_MAX_DEVICES 64

struct pci_device_info {
    uint16_t vendor_id;
    uint16_t device_id;
    uint8_t class_code;
    uint8_t subclass;
    uint8_t bus;
    uint8_t slot;
    uint8_t function;
};

void pci_init(void);
size_t pci_device_count(void);
int pci_get_device(size_t index, struct pci_device_info *info);
uint32_t pci_config_read32(uint8_t bus, uint8_t slot, uint8_t function,
                           uint8_t offset);
void pci_config_write16(uint8_t bus, uint8_t slot, uint8_t function,
                        uint8_t offset, uint16_t value);
