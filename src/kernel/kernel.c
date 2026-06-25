#include "../../limine/limine.h"
#include "../cpu/exceptions.h"
#include "../cpu/gdt.h"
#include "../cpu/idt.h"
#include "../cpu/irq.h"
#include "../cpu/syscall.h"
#include "../drivers/framebuffer.h"
#include "../drivers/pic.h"
#include "../drivers/pit.h"
#include "../drivers/ps2.h"
#include "../drivers/serial.h"
#include "../gfx/console.h"
#include "../gfx/draw.h"
#include "../log.h"
#include "../mm/kmalloc.h"
#include "../mm/pmm.h"
#include "../mm/vmm.h"
#include "../utils.h"
#include "shell.h"
#include "task.h"
#include <stdint.h>

LIMINE_BASE_REVISION(6);

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
    if (!pmm_init()) {
        LOG_ERROR("failed to initialize physical memory manager");
        for (;;)
            __asm__ volatile("hlt");
    }
    if (!vmm_init()) {
        LOG_ERROR("failed to initialize virtual memory manager");
        for (;;)
            __asm__ volatile("hlt");
    }
    if (!kmalloc_init()) {
        LOG_ERROR("failed to initialize kernel heap");
        for (;;)
            __asm__ volatile("hlt");
    }
    if (!gdt_init()) {
        LOG_ERROR("failed to initialize GDT and TSS");
        for (;;)
            __asm__ volatile("hlt");
    }
    task_init();

    pic_remap();
    exceptions_init();
    irq_init();
    syscall_init();
    load_idt();
    LOG_DEBUG("IDT, exceptions and IRQ initialized");
    ps2_init();
    pit_init();
    irq_install_handler(1, keyboard_handler);
    LOG_INFO("PS/2 keyboard initialized");
    LOG_INFO("framebuffer OK");
    irq_clear_mask(1);
    task_enable_preemption();
    asm volatile("sti");
    shell_init();
    for (;;) {
        shell_poll();
        __asm__ volatile("hlt");
    }
}
