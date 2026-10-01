#include "../../limine/limine.h"
#include "../cpu/exceptions.h"
#include "../cpu/gdt.h"
#include "../cpu/idt.h"
#include "../cpu/irq.h"
#include "../cpu/smp.h"
#include "../cpu/syscall.h"
#include "../drivers/block.h"
#include "../drivers/framebuffer.h"
#include "../drivers/pci.h"
#include "../drivers/pit.h"
#include "../drivers/ps2.h"
#include "../drivers/serial.h"
#include "../firmware/acpi.h"
#include "../fs/fat32.h"
#include "../fs/initramfs.h"
#include "../fs/ramfs.h"
#include "../fs/vfs.h"
#include "../gfx/console.h"
#include "../gfx/draw.h"
#include "../log.h"
#include "../mm/kmalloc.h"
#include "../mm/pmm.h"
#include "../mm/vmm.h"
#include "../utils.h"
#include "process.h"
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
    if (!smp_init()) {
        LOG_ERROR("failed to start all CPUs before protecting boot mappings");
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
    ramfs_init();
    if (!initramfs_mount())
        LOG_ERROR("failed to mount initramfs");
    vfs_init();
    if (!gdt_init()) {
        LOG_ERROR("failed to initialize GDT and TSS");
        for (;;)
            __asm__ volatile("hlt");
    }
    task_init();

    exceptions_init();
    irq_init();
    syscall_init();
    load_idt();
    if (!smp_prepare_aps()) {
        LOG_ERROR("failed to initialize secondary CPU contexts");
        for (;;)
            __asm__ volatile("hlt");
    }
    acpi_init();
    if (!irq_controller_init()) {
        for (;;)
            __asm__ volatile("hlt");
    }
    LOG_DEBUG("IDT, exceptions and IRQ initialized");
    ps2_init();
    pit_init();
    irq_install_handler(1, keyboard_handler);
    LOG_INFO("PS/2 keyboard initialized");
    LOG_INFO("framebuffer OK");
    pci_init();
    if (block_init()) {
        uint8_t boot_sector[512];
        if (block_read_sector(0, boot_sector))
            LOG_INFO("VirtIO block sector I/O verified");
        else
            LOG_ERROR("VirtIO block sector I/O failed");
        if (fat32_mount() && !vfs_mount("fat32", "/disk"))
            LOG_ERROR("failed to register FAT32 mount at /disk");
    }
    irq_clear_mask(1);
    task_enable_preemption();
    asm volatile("sti");
    smp_start_timers();
    int userspace_active = process_run_hello();
    if (!userspace_active)
        shell_init();
    for (;;) {
        if (userspace_active && !process_is_running()) {
            shell_init();
            userspace_active = 0;
        }
        if (userspace_active)
            task_yield();
        else
            shell_poll();
        __asm__ volatile("hlt");
    }
}
