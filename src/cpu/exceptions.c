#include "exceptions.h"
#include "../drivers/serial.h"
#include "../gfx/console.h"
#include "../kernel/task.h"
#include "gdt.h"
#include "idt.h"
#include "irq.h"
#include <stdint.h>

#define IDT_INT_GATE 0x8E

static const char *const exception_names[32] = {
    "Divide Error",
    "Debug",
    "Non-maskable Interrupt",
    "Breakpoint",
    "Overflow",
    "Bound Range Exceeded",
    "Invalid Opcode",
    "Device Not Available",
    "Double Fault",
    "Coprocessor Segment Overrun",
    "Invalid TSS",
    "Segment Not Present",
    "Stack-Segment Fault",
    "General Protection Fault",
    "Page Fault",
    "Reserved",
    "x87 Floating-Point Exception",
    "Alignment Check",
    "Machine Check",
    "SIMD Floating-Point Exception",
    "Virtualization Exception",
    "Control Protection Exception",
    "Reserved",
    "Reserved",
    "Reserved",
    "Reserved",
    "Reserved",
    "Reserved",
    "Hypervisor Injection Exception",
    "VMM Communication Exception",
    "Security Exception",
    "Reserved",
};

static void panic_write(const char *text) {
    print_serial(text);
    console_write(text);
}

static void panic_write_hex(uint64_t value) {
    static const char digits[] = "0123456789ABCDEF";
    char buffer[19];

    buffer[0] = '0';
    buffer[1] = 'x';
    for (int i = 0; i < 16; i++) {
        unsigned int shift = (unsigned int)(15 - i) * 4;
        buffer[i + 2] = digits[(value >> shift) & 0xF];
    }
    buffer[18] = '\0';
    panic_write(buffer);
}

__attribute__((noreturn)) static void
exception_panic(uint8_t vector, uint64_t error_code,
                struct interrupt_frame *frame) {
    __asm__ volatile("cli");

    console_set_color(0xFF4444);
    panic_write("\n\nKERNEL PANIC: CPU exception\n");
    console_set_color(0xFFFFFF);

    panic_write("Exception: ");
    panic_write(exception_names[vector]);
    panic_write(" (");
    panic_write_hex(vector);
    panic_write(")\nError code: ");
    panic_write_hex(error_code);
    panic_write("\nRIP: ");
    panic_write_hex(frame->ip);
    panic_write("\nCS: ");
    panic_write_hex(frame->cs);
    panic_write("\nRFLAGS: ");
    panic_write_hex(frame->flags);

    if (vector == 14) {
        uint64_t fault_address;
        __asm__ volatile("mov %%cr2, %0" : "=r"(fault_address));
        panic_write("\nFault address (CR2): ");
        panic_write_hex(fault_address);
        if (task_current_stack_guard_contains(fault_address))
            panic_write("\nCause: kernel task stack guard page hit");
    }

    panic_write("\nSystem halted.\n");
    for (;;)
        __asm__ volatile("hlt");
}

#define EXCEPTION_NO_ERROR(vector)                                             \
    __attribute__((interrupt)) static void exception_##vector(                 \
        struct interrupt_frame *frame) {                                       \
        exception_panic(vector, 0, frame);                                     \
    }

#define EXCEPTION_WITH_ERROR(vector)                                           \
    __attribute__((interrupt)) static void exception_##vector(                 \
        struct interrupt_frame *frame, uint64_t error_code) {                  \
        exception_panic(vector, error_code, frame);                            \
    }

EXCEPTION_NO_ERROR(0)
EXCEPTION_NO_ERROR(1)
EXCEPTION_NO_ERROR(2)
EXCEPTION_NO_ERROR(3)
EXCEPTION_NO_ERROR(4)
EXCEPTION_NO_ERROR(5)
EXCEPTION_NO_ERROR(6)
EXCEPTION_NO_ERROR(7)
EXCEPTION_WITH_ERROR(8)
EXCEPTION_NO_ERROR(9)
EXCEPTION_WITH_ERROR(10)
EXCEPTION_WITH_ERROR(11)
EXCEPTION_WITH_ERROR(12)
EXCEPTION_WITH_ERROR(13)
EXCEPTION_WITH_ERROR(14)
EXCEPTION_NO_ERROR(15)
EXCEPTION_NO_ERROR(16)
EXCEPTION_WITH_ERROR(17)
EXCEPTION_NO_ERROR(18)
EXCEPTION_NO_ERROR(19)
EXCEPTION_NO_ERROR(20)
EXCEPTION_WITH_ERROR(21)
EXCEPTION_NO_ERROR(22)
EXCEPTION_NO_ERROR(23)
EXCEPTION_NO_ERROR(24)
EXCEPTION_NO_ERROR(25)
EXCEPTION_NO_ERROR(26)
EXCEPTION_NO_ERROR(27)
EXCEPTION_NO_ERROR(28)
EXCEPTION_WITH_ERROR(29)
EXCEPTION_WITH_ERROR(30)
EXCEPTION_NO_ERROR(31)

static void *const exception_stubs[32] = {
    exception_0,  exception_1,  exception_2,  exception_3,  exception_4,
    exception_5,  exception_6,  exception_7,  exception_8,  exception_9,
    exception_10, exception_11, exception_12, exception_13, exception_14,
    exception_15, exception_16, exception_17, exception_18, exception_19,
    exception_20, exception_21, exception_22, exception_23, exception_24,
    exception_25, exception_26, exception_27, exception_28, exception_29,
    exception_30, exception_31,
};

void exceptions_init(void) {
    for (uint8_t vector = 0; vector < 32; vector++) {
        uint8_t ist = 0;
        if (vector == 8)
            ist = GDT_IST_DOUBLE_FAULT;
        else if (vector == 14)
            ist = GDT_IST_PAGE_FAULT;
        idt_set_gate(vector, (uint64_t)exception_stubs[vector], GDT_KERNEL_CODE,
                     IDT_INT_GATE, ist);
    }
}
