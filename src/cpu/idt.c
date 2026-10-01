#include "idt.h"
#include "../log.h"

static struct idt_entry idt[256];

void idt_set_gate(uint8_t vector, uint64_t handler, uint16_t selector,
                  uint8_t type_attr, uint8_t ist) {
    struct idt_entry *e = &idt[vector];
    e->offset_low = handler & 0xFFFF;
    e->offset_mid = (handler >> 16) & 0xFFFF;
    e->offset_high = (handler >> 32) & 0xFFFFFFFF;
    e->selector = selector;
    e->type_attr = type_attr;
    e->ist = ist & 0x7;
    e->zero = 0;
}

void idt_load_current(void) {
    struct idt_ptr pointer = {
        .base = (uint64_t)&idt,
        .limit = sizeof(idt) - 1,
    };
    __asm__ volatile("lidt %0" : : "m"(pointer));
}

void load_idt(void) {
    idt_load_current();
    LOG_INFO("IDT loaded");
}
