#include "pic.h"
#include <stdint.h>

void pic_remap(void) {
    // ICW1: start init + ICW4
    outb(0x20, 0x11);
    outb(0xA0, 0x11);

    // ICW2: remap offsets
    outb(0x21, 0x20); // IRQ0-7 -> 0x20-0x27
    outb(0xA1, 0x28); // IRQ8-15 -> 0x28-0x2F

    // ICW3: wiring
    outb(0x21, 0x04); // slave at IRQ2
    outb(0xA1, 0x02);

    // ICW4: 8086 mode
    outb(0x21, 0x01);
    outb(0xA1, 0x01);
}

void pic_init_masks(void) {
    outb(0x21, 0xFE); // turn on irq
    outb(0xA1, 0xFF);
}

void pic_disable(void) {
    outb(0x21, 0xFF);
    outb(0xA1, 0xFF);
}

void pic_eoi(uint8_t irq) {
    if (irq >= 8)
        outb(0xA0, 0x20);
    outb(0x20, 0x20);
}

void pic_set_irq_mask(uint8_t irq, int masked) {
    if (irq >= 16)
        return;
    uint16_t port = irq < 8 ? 0x21 : 0xA1;
    uint8_t bit = 1U << (irq & 7);
    uint8_t value = inb(port);
    outb(port, masked ? value | bit : value & ~bit);
    if (!masked && irq >= 8)
        outb(0x21, inb(0x21) & ~(1U << 2));
}
