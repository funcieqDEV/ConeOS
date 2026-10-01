#include "stack.h"
#include "../cpu/spinlock.h"
#include "pmm.h"
#include "vmm.h"

#define STACK_REGION_BASE 0xFFFFE00000000000ULL

static uint64_t next_stack_base = STACK_REGION_BASE;
static spinlock_t stack_lock = SPINLOCK_INITIALIZER;

int kernel_stack_alloc(struct kernel_stack *stack, size_t page_count) {
    if (stack == NULL || page_count == 0)
        return 0;

    uint64_t flags = spin_lock_irqsave(&stack_lock);
    uint64_t guard_base = next_stack_base;
    uint64_t bottom = guard_base + PMM_PAGE_SIZE;
    size_t mapped_pages = 0;

    for (size_t page = 0; page < page_count; page++) {
        uint64_t physical = pmm_alloc_page();
        uint64_t virtual = bottom + page * PMM_PAGE_SIZE;
        if (physical == PMM_INVALID_ADDRESS ||
            !vmm_map_page(virtual, physical, VMM_WRITABLE | VMM_NO_EXECUTE)) {
            if (physical != PMM_INVALID_ADDRESS)
                pmm_free_page(physical);
            while (mapped_pages > 0) {
                mapped_pages--;
                uint64_t rollback_virtual =
                    bottom + mapped_pages * PMM_PAGE_SIZE;
                uint64_t rollback_physical = vmm_unmap_page(rollback_virtual);
                if (rollback_physical != PMM_INVALID_ADDRESS)
                    pmm_free_page(rollback_physical);
            }
            spin_unlock_irqrestore(&stack_lock, flags);
            return 0;
        }
        mapped_pages++;
    }

    stack->guard_base = guard_base;
    stack->bottom = bottom;
    stack->top = bottom + page_count * PMM_PAGE_SIZE;
    stack->page_count = page_count;
    next_stack_base = stack->top;
    spin_unlock_irqrestore(&stack_lock, flags);
    return 1;
}

void kernel_stack_free(struct kernel_stack *stack) {
    if (stack == NULL || stack->page_count == 0)
        return;

    uint64_t flags = spin_lock_irqsave(&stack_lock);
    for (size_t page = 0; page < stack->page_count; page++) {
        uint64_t virtual = stack->bottom + page * PMM_PAGE_SIZE;
        uint64_t physical = vmm_unmap_page(virtual);
        if (physical != PMM_INVALID_ADDRESS)
            pmm_free_page(physical);
    }

    stack->guard_base = 0;
    stack->bottom = 0;
    stack->top = 0;
    stack->page_count = 0;
    spin_unlock_irqrestore(&stack_lock, flags);
}

int kernel_stack_contains_guard(const struct kernel_stack *stack,
                                uint64_t address) {
    return stack != NULL && stack->page_count != 0 &&
           address >= stack->guard_base &&
           address < stack->guard_base + PMM_PAGE_SIZE;
}
