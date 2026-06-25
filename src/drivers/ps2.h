#pragma once
#include "../cpu/irq.h"
#include <stdint.h>

void ps2_init(void);
void keyboard_handler(struct interrupt_frame *f);
int keyboard_read_char(char *c);
