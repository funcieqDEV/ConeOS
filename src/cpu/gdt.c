#include "gdt.h"
#include "../log.h"
#include "../mm/stack.h"
#include <stddef.h>

#define IST_STACK_PAGES 4

struct tss {
    uint32_t reserved0;
    uint64_t rsp[3];
    uint64_t reserved1;
    uint64_t ist[7];
    uint64_t reserved2;
    uint16_t reserved3;
    uint16_t io_map_base;
} __attribute__((packed));

struct gdt_pointer {
    uint16_t limit;
    uint64_t base;
} __attribute__((packed));

static uint64_t gdt[7];
static struct tss tss;
static struct kernel_stack double_fault_stack;
static struct kernel_stack page_fault_stack;

extern void gdt_load(const struct gdt_pointer *pointer);

__asm__(".global gdt_load\n"
        ".type gdt_load, @function\n"
        "gdt_load:\n"
        "    lgdt (%rdi)\n"
        "    pushq $0x08\n"
        "    leaq 1f(%rip), %rax\n"
        "    pushq %rax\n"
        "    lretq\n"
        "1:\n"
        "    movw $0x10, %ax\n"
        "    movw %ax, %ds\n"
        "    movw %ax, %es\n"
        "    movw %ax, %ss\n"
        "    movw %ax, %fs\n"
        "    movw %ax, %gs\n"
        "    movw $0x28, %ax\n"
        "    ltr %ax\n"
        "    retq\n"
        ".size gdt_load, .-gdt_load\n");

static void set_tss_descriptor(void) {
    uint64_t base = (uint64_t)&tss;
    uint64_t limit = sizeof(tss) - 1;

    gdt[5] = (limit & 0xFFFF) | ((base & 0xFFFFFF) << 16) | (0x89ULL << 40) |
             (((limit >> 16) & 0xF) << 48) | (((base >> 24) & 0xFF) << 56);
    gdt[6] = base >> 32;
}

int gdt_init(void) {
    if (!kernel_stack_alloc(&double_fault_stack, IST_STACK_PAGES))
        return 0;
    if (!kernel_stack_alloc(&page_fault_stack, IST_STACK_PAGES)) {
        kernel_stack_free(&double_fault_stack);
        return 0;
    }

    for (size_t i = 0; i < sizeof(tss); i++)
        ((uint8_t *)&tss)[i] = 0;

    uint64_t current_stack;
    __asm__ volatile("mov %%rsp, %0" : "=r"(current_stack));
    tss.rsp[0] = current_stack;
    tss.ist[GDT_IST_DOUBLE_FAULT - 1] = double_fault_stack.top;
    tss.ist[GDT_IST_PAGE_FAULT - 1] = page_fault_stack.top;
    tss.io_map_base = sizeof(tss);

    gdt[0] = 0;
    gdt[1] = 0x00AF9A000000FFFFULL;
    gdt[2] = 0x00CF92000000FFFFULL;
    gdt[3] = 0x00CFF2000000FFFFULL;
    gdt[4] = 0x00AFFA000000FFFFULL;
    set_tss_descriptor();

    struct gdt_pointer pointer = {
        .limit = sizeof(gdt) - 1,
        .base = (uint64_t)gdt,
    };
    gdt_load(&pointer);
    LOG_INFO("GDT and TSS initialized");
    return 1;
}

void gdt_set_kernel_stack(uint64_t stack_top) { tss.rsp[0] = stack_top; }
