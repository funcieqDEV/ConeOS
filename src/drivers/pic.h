#pragma once
#include <stdint.h>
void pic_remap(void);
void pic_init_masks(void);
void pic_disable(void);
void pic_eoi(uint8_t irq);
void pic_set_irq_mask(uint8_t irq, int masked);
static inline void outb(uint16_t port, uint8_t val) {
    __asm__ volatile("outb %0, %1" : : "a"(val), "Nd"(port));
}

static inline uint8_t inb(uint16_t port) {
    uint8_t ret;
    asm volatile("inb %1, %0" : "=a"(ret) : "Nd"(port));
    return ret;
}
