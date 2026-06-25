#pragma once

#include <stdint.h>

#define GDT_KERNEL_CODE 0x08
#define GDT_KERNEL_DATA 0x10
#define GDT_USER_DATA 0x1B
#define GDT_USER_CODE 0x23
#define GDT_TSS 0x28

#define GDT_IST_DOUBLE_FAULT 1
#define GDT_IST_PAGE_FAULT 2

int gdt_init(void);
void gdt_set_kernel_stack(uint64_t stack_top);
