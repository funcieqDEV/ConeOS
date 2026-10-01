#pragma once

#include <stdint.h>

#define APIC_TIMER_VECTOR 0x40

int apic_init(void);
int apic_timer_start(void);
uint32_t apic_current_id(void);
void apic_eoi(void);
void apic_set_irq_mask(uint8_t irq, int masked);
