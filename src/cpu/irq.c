#include "irq.h"
#include "idt.h"
#include "../drivers/pic.h"

#define PIC1_DATA 0x21
#define PIC2_DATA 0xA1
#define PIC1_CMD  0x20
#define PIC2_CMD  0xA0
#define PIC_EOI   0x20

/* Limine protocol GDT: entry 5 = 64-bit code segment */
#define KERNEL_CS    0x28
#define IDT_INT_GATE 0x8E

static irq_handler_t irq_handlers[16];

static void pic_eoi(uint8_t irq) {
    if (irq >= 8)
        outb(PIC2_CMD, PIC_EOI);
    outb(PIC1_CMD, PIC_EOI);
}

void irq_set_mask(uint8_t irq) {
    uint16_t port = (irq < 8) ? PIC1_DATA : PIC2_DATA;
    if (irq >= 8) irq -= 8;
    outb(port, inb(port) | (1 << irq));
}

void irq_clear_mask(uint8_t irq) {
    uint16_t port = (irq < 8) ? PIC1_DATA : PIC2_DATA;
    if (irq >= 8) irq -= 8;
    outb(port, inb(port) & ~(1 << irq));
}

void irq_install_handler(int irq, irq_handler_t handler) {
    irq_handlers[irq] = handler;
}

void irq_uninstall_handler(int irq) {
    irq_handlers[irq] = 0;
}

static void irq_dispatch(int irq, struct interrupt_frame *frame) {
    if (irq_handlers[irq])
        irq_handlers[irq](frame);
    pic_eoi(irq);
}

/* One stub per IRQ — GCC __attribute__((interrupt)) handles pushes/pops and iretq */
#define IRQ_STUB(n)                                              \
    __attribute__((interrupt))                                   \
    static void irq##n##_stub(struct interrupt_frame *frame) {  \
        irq_dispatch(n, frame);                                  \
    }

IRQ_STUB(0)  IRQ_STUB(1)  IRQ_STUB(2)  IRQ_STUB(3)
IRQ_STUB(4)  IRQ_STUB(5)  IRQ_STUB(6)  IRQ_STUB(7)
IRQ_STUB(8)  IRQ_STUB(9)  IRQ_STUB(10) IRQ_STUB(11)
IRQ_STUB(12) IRQ_STUB(13) IRQ_STUB(14) IRQ_STUB(15)

static void *const irq_stubs[16] = {
    irq0_stub,  irq1_stub,  irq2_stub,  irq3_stub,
    irq4_stub,  irq5_stub,  irq6_stub,  irq7_stub,
    irq8_stub,  irq9_stub,  irq10_stub, irq11_stub,
    irq12_stub, irq13_stub, irq14_stub, irq15_stub,
};

void irq_init(void) {
    for (int i = 0; i < 16; i++) {
        idt_set_gate(0x20 + i, (uint64_t)irq_stubs[i], KERNEL_CS, IDT_INT_GATE);
        irq_handlers[i] = 0;
    }

    outb(PIC1_DATA, 0xFF);
    outb(PIC2_DATA, 0xFF);
}
