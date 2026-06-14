#include "idt.h"
#include "../log.h"

static struct idt_entry idt[256];
static struct idt_ptr idtp;

void idt_set_gate(uint8_t vector, uint64_t handler, uint16_t selector, uint8_t type_attr) {
  struct idt_entry *e = &idt[vector];
  e->offset_low  = handler & 0xFFFF;
  e->offset_mid  = (handler >> 16) & 0xFFFF;
  e->offset_high = (handler >> 32) & 0xFFFFFFFF;
  e->selector    = selector;
  e->type_attr   = type_attr;
  e->ist         = 0;
  e->zero        = 0;
}

void load_idt(void) {
  idtp.base = (uint64_t)&idt;
  idtp.limit = sizeof(idt) - 1;

  __asm__ volatile("lidt %0" : : "m"(idtp));
  LOG_INFO("IDT loaded");
}
