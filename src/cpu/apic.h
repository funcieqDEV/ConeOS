#pragma once

#include <stdint.h>

int apic_init(void);
void apic_eoi(void);
void apic_set_irq_mask(uint8_t irq, int masked);
