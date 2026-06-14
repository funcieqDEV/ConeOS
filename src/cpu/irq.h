#pragma once
#include <stdint.h>
#include "../drivers/serial.h"
#include "../drivers/pic.h"

struct interrupt_frame {
    uint64_t ip;
    uint64_t cs;
    uint64_t flags;
    uint64_t sp;
    uint64_t ss;
};

typedef void (*irq_handler_t)(struct interrupt_frame *);

void irq_init(void);
void irq_install_handler(int irq, irq_handler_t handler);
void irq_uninstall_handler(int irq);
void irq_set_mask(uint8_t irq);
void irq_clear_mask(uint8_t irq);

