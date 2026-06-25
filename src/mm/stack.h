#pragma once

#include <stddef.h>
#include <stdint.h>

struct kernel_stack {
    uint64_t guard_base;
    uint64_t bottom;
    uint64_t top;
    size_t page_count;
};

int kernel_stack_alloc(struct kernel_stack *stack, size_t page_count);
void kernel_stack_free(struct kernel_stack *stack);
int kernel_stack_contains_guard(const struct kernel_stack *stack,
                                uint64_t address);
