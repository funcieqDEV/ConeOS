#include "../../limine/limine.h"
#include "../cpu/idt.h"
#include "../cpu/irq.h"
#include "../drivers/framebuffer.h"
#include "../drivers/pic.h"
#include "../drivers/ps2.h"
#include "../drivers/serial.h"
#include "../gfx/console.h"
#include "../gfx/draw.h"
#include "../log.h"
#include "../mm/kmalloc.h"
#include "../utils.h"
#include <stdint.h>

LIMINE_BASE_REVISION(6);

static void irq0_handler(struct interrupt_frame *f) {
    (void)f;
    // print_serial("tick\n");
}

void kmain(void) {
    serial_init();

    struct limine_framebuffer *fb = framebuffer_get();
    if (fb == NULL) {
        print_serial("no framebuffer\n");
        for (;;)
            __asm__ volatile("hlt");
    }
    console_init(fb);
    log_console_enable();

    LOG_INFO("kernel start");
    kmalloc_init();

    pic_remap();
    load_idt();
    irq_init();
    LOG_DEBUG("IDT + IRQ initialized");
    ps2_init();
    irq_install_handler(0, irq0_handler);
    irq_clear_mask(0);
    irq_install_handler(1, keyboard_handler);
    LOG_INFO("PS/2 keyboard initialized");
    LOG_INFO("framebuffer OK");
    irq_clear_mask(1);
    asm volatile("sti");
    for (;;)
        __asm__ volatile("hlt");
}
