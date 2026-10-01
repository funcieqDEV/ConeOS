#include "gdt.h"
#include "../log.h"
#include "../mm/kmalloc.h"
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

struct gdt_context {
    uint64_t gdt[7];
    struct tss tss;
    struct kernel_stack double_fault_stack;
    struct kernel_stack page_fault_stack;
};

static struct gdt_context bsp_context;

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

static void set_tss_descriptor(struct gdt_context *context) {
    uint64_t base = (uint64_t)&context->tss;
    uint64_t limit = sizeof(context->tss) - 1;

    context->gdt[5] =
        (limit & 0xFFFF) | ((base & 0xFFFFFF) << 16) | (0x89ULL << 40) |
        (((limit >> 16) & 0xF) << 48) | (((base >> 24) & 0xFF) << 56);
    context->gdt[6] = base >> 32;
}

static int gdt_context_init(struct gdt_context *context, uint64_t stack_top) {
    if (!kernel_stack_alloc(&context->double_fault_stack, IST_STACK_PAGES))
        return 0;
    if (!kernel_stack_alloc(&context->page_fault_stack, IST_STACK_PAGES)) {
        kernel_stack_free(&context->double_fault_stack);
        return 0;
    }

    for (size_t i = 0; i < sizeof(context->tss); i++)
        ((uint8_t *)&context->tss)[i] = 0;

    context->tss.rsp[0] = stack_top;
    context->tss.ist[GDT_IST_DOUBLE_FAULT - 1] =
        context->double_fault_stack.top;
    context->tss.ist[GDT_IST_PAGE_FAULT - 1] =
        context->page_fault_stack.top;
    context->tss.io_map_base = sizeof(context->tss);

    context->gdt[0] = 0;
    context->gdt[1] = 0x00AF9A000000FFFFULL;
    context->gdt[2] = 0x00CF92000000FFFFULL;
    context->gdt[3] = 0x00CFF2000000FFFFULL;
    context->gdt[4] = 0x00AFFA000000FFFFULL;
    set_tss_descriptor(context);
    return 1;
}

static void gdt_load_context(struct gdt_context *context) {
    struct gdt_pointer pointer = {
        .limit = sizeof(context->gdt) - 1,
        .base = (uint64_t)context->gdt,
    };
    gdt_load(&pointer);
}

int gdt_init(void) {
    uint64_t current_stack;
    __asm__ volatile("mov %%rsp, %0" : "=r"(current_stack));
    if (!gdt_context_init(&bsp_context, current_stack))
        return 0;
    gdt_load_context(&bsp_context);
    LOG_INFO("GDT and TSS initialized");
    return 1;
}

struct gdt_context *gdt_prepare_secondary(uint64_t stack_top) {
    struct gdt_context *context = kmalloc(sizeof(*context));
    if (context == NULL)
        return NULL;
    if (!gdt_context_init(context, stack_top)) {
        kfree(context);
        return NULL;
    }
    return context;
}

void gdt_load_secondary(struct gdt_context *context) {
    gdt_load_context(context);
}

void gdt_set_boot_cpu_kernel_stack(uint64_t stack_top) {
    bsp_context.tss.rsp[0] = stack_top;
}

void gdt_set_boot_cpu_page_fault_stack(uint64_t stack_top) {
    bsp_context.tss.ist[GDT_IST_PAGE_FAULT - 1] =
        stack_top ? stack_top : bsp_context.page_fault_stack.top;
}
