#include "irq.h"
#include "../drivers/pic.h"
#include "../firmware/acpi.h"
#include "../log.h"
#include "apic.h"
#include "gdt.h"
#include "idt.h"
#include "protection.h"
#include <stddef.h>

#define IDT_INT_GATE 0x8E

static irq_handler_t irq_handlers[16];
static int use_apic;

static void change_mask(uint8_t irq, int masked) {
    uint64_t flags;
    /* IOAPIC selector/window accesses and PIC mask updates must be atomic. */
    __asm__ volatile("pushfq; pop %0; cli" : "=r"(flags) : : "memory");
    if (use_apic)
        apic_set_irq_mask(irq, masked);
    else
        pic_set_irq_mask(irq, masked);
    __asm__ volatile("push %0; popfq" : : "r"(flags) : "memory", "cc");
}

void irq_set_mask(uint8_t irq) { change_mask(irq, 1); }
void irq_clear_mask(uint8_t irq) { change_mask(irq, 0); }

void irq_install_handler(int irq, irq_handler_t handler) {
    if (irq >= 0 && irq < 16)
        irq_handlers[irq] = handler;
}

void irq_uninstall_handler(int irq) { irq_install_handler(irq, NULL); }

static void irq_dispatch(int irq, struct interrupt_frame *frame) {
    cpu_clear_access_override();
    /* The timer handler may switch tasks before returning to this frame. */
    if (use_apic)
        apic_eoi();
    else
        pic_eoi(irq);
    if (irq_handlers[irq])
        irq_handlers[irq](frame);
}

__attribute__((interrupt)) static void
apic_spurious_stub(struct interrupt_frame *frame) {
    (void)frame;
    cpu_clear_access_override();
    /* A LAPIC spurious vector has no in-service bit and needs no EOI. */
}

#define IRQ_STUB(n)                                                            \
    __attribute__((interrupt)) static void irq##n##_stub(                      \
        struct interrupt_frame *frame) {                                       \
        irq_dispatch(n, frame);                                                \
    }

IRQ_STUB(0);
IRQ_STUB(1);
IRQ_STUB(2);
IRQ_STUB(3);
IRQ_STUB(4);
IRQ_STUB(5);
IRQ_STUB(6);
IRQ_STUB(7);
IRQ_STUB(8);
IRQ_STUB(9);
IRQ_STUB(10);
IRQ_STUB(11);
IRQ_STUB(12);
IRQ_STUB(13);
IRQ_STUB(14);
IRQ_STUB(15);

static void *const irq_stubs[16] = {
    irq0_stub,  irq1_stub,  irq2_stub,  irq3_stub,  irq4_stub,  irq5_stub,
    irq6_stub,  irq7_stub,  irq8_stub,  irq9_stub,  irq10_stub, irq11_stub,
    irq12_stub, irq13_stub, irq14_stub, irq15_stub,
};

void irq_init(void) {
    for (int i = 0; i < 16; i++) {
        idt_set_gate(0x20 + i, (uint64_t)irq_stubs[i], GDT_KERNEL_CODE,
                     IDT_INT_GATE, 0);
        irq_handlers[i] = 0;
    }

    idt_set_gate(0xFF, (uint64_t)apic_spurious_stub, GDT_KERNEL_CODE,
                 IDT_INT_GATE, 0);
    LOG_INFO("IRQ initialized");
}

int irq_controller_init(void) {
    const struct acpi_topology *topology = acpi_topology();
    int has_pic = !topology || (topology->flags & 1);
    if (has_pic) {
        pic_remap();
        pic_disable();
    }
    use_apic = apic_init();
    if (!use_apic && !has_pic) {
        LOG_ERROR("no usable interrupt controller; MADT reports no legacy PIC");
        return 0;
    }
    LOG_INFO(use_apic ? "IRQ controller: APIC" : "IRQ controller: PIC");
    return 1;
}
