#pragma once

#include <stdint.h>

#define PIT_FREQUENCY_HZ 100

void pit_init(void);
uint64_t pit_ticks(void);
uint64_t pit_uptime_ms(void);
