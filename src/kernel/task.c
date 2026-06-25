#include "task.h"
#include "../cpu/gdt.h"
#include "../log.h"
#include "../mm/kmalloc.h"
#include "../mm/pmm.h"
#include "../mm/stack.h"
#include "../mm/vmm.h"

#define TASK_STACK_SIZE (64 * 1024)

struct task {
    uint64_t *stack_pointer;
    uint64_t id;
    const char *name;
    enum task_state state;
    task_entry_t entry;
    void *argument;
    struct kernel_stack stack;
    struct vmm_space *space;
    int owns_space;
    struct task *next;
};

static struct task main_task;
static struct task *task_list;
static struct task *current_task;
static uint64_t next_task_id = 1;
static int preemption_enabled;

extern void task_context_switch(uint64_t **old_stack_pointer,
                                uint64_t *new_stack_pointer);

__asm__(".global task_context_switch\n"
        ".type task_context_switch, @function\n"
        "task_context_switch:\n"
        "    pushq %rbx\n"
        "    pushq %rbp\n"
        "    pushq %r12\n"
        "    pushq %r13\n"
        "    pushq %r14\n"
        "    pushq %r15\n"
        "    movq %rsp, (%rdi)\n"
        "    movq %rsi, %rsp\n"
        "    popq %r15\n"
        "    popq %r14\n"
        "    popq %r13\n"
        "    popq %r12\n"
        "    popq %rbp\n"
        "    popq %rbx\n"
        "    retq\n"
        ".size task_context_switch, .-task_context_switch\n");

static uint64_t interrupt_lock(void) {
    uint64_t flags;
    __asm__ volatile("pushfq; pop %0; cli" : "=r"(flags) : : "memory");
    return flags;
}

static void interrupt_restore(uint64_t flags) {
    __asm__ volatile("push %0; popfq" : : "r"(flags) : "memory", "cc");
}

static void remove_zombies(void) {
    struct task *previous = task_list;
    struct task *task = task_list->next;

    while (task != NULL) {
        if (task != current_task && task->state == TASK_ZOMBIE) {
            previous->next = task->next;
            kernel_stack_free(&task->stack);
            if (task->owns_space)
                vmm_space_destroy(task->space);
            kfree(task);
            task = previous->next;
        } else {
            previous = task;
            task = task->next;
        }
    }
}

static struct task *next_ready_task(void) {
    struct task *task =
        current_task->next != NULL ? current_task->next : task_list;

    while (task != current_task) {
        if (task->state == TASK_READY)
            return task;
        task = task->next != NULL ? task->next : task_list;
    }

    return current_task->state == TASK_READY ? current_task : NULL;
}

__attribute__((noreturn)) void task_exit_current(void) {
    current_task->state = TASK_ZOMBIE;
    task_yield();
    __builtin_unreachable();
}

__attribute__((noreturn)) static void task_bootstrap(void) {
    __asm__ volatile("sti");
    current_task->entry(current_task->argument);
    task_exit_current();
}

void task_init(void) {
    main_task.stack_pointer = NULL;
    main_task.id = 0;
    main_task.name = "kernel";
    main_task.state = TASK_RUNNING;
    main_task.entry = NULL;
    main_task.argument = NULL;
    main_task.stack.guard_base = 0;
    main_task.stack.bottom = 0;
    __asm__ volatile("mov %%rsp, %0" : "=r"(main_task.stack.top));
    main_task.stack.page_count = 0;
    main_task.space = vmm_kernel_space();
    main_task.owns_space = 0;
    main_task.next = NULL;
    task_list = &main_task;
    current_task = &main_task;
    preemption_enabled = 0;
    LOG_INFO("scheduler initialized");
}

void task_enable_preemption(void) {
    preemption_enabled = 1;
    LOG_INFO("preemptive scheduling enabled");
}

uint64_t task_create(const char *name, task_entry_t entry, void *argument) {
    return task_create_in_space(name, entry, argument, vmm_kernel_space());
}

uint64_t task_create_in_space(const char *name, task_entry_t entry,
                              void *argument, struct vmm_space *space) {
    if (name == NULL || entry == NULL || space == NULL)
        return UINT64_MAX;

    struct task *task = kmalloc(sizeof(*task));
    if (task == NULL)
        return UINT64_MAX;

    struct kernel_stack stack;
    if (!kernel_stack_alloc(&stack, TASK_STACK_SIZE / PMM_PAGE_SIZE)) {
        kfree(task);
        return UINT64_MAX;
    }

    uint64_t *stack_pointer = (uint64_t *)stack.top;
    *--stack_pointer = 0;
    *--stack_pointer = (uint64_t)task_bootstrap;
    *--stack_pointer = 0;
    *--stack_pointer = 0;
    *--stack_pointer = 0;
    *--stack_pointer = 0;
    *--stack_pointer = 0;
    *--stack_pointer = 0;

    task->stack_pointer = stack_pointer;
    task->id = next_task_id++;
    task->name = name;
    task->state = TASK_READY;
    task->entry = entry;
    task->argument = argument;
    task->stack = stack;
    task->space = space;
    task->owns_space = space != vmm_kernel_space();
    task->next = NULL;

    struct task *last = task_list;
    while (last->next != NULL)
        last = last->next;
    last->next = task;
    return task->id;
}

static void schedule(int clean_zombies) {
    uint64_t flags = interrupt_lock();
    if (clean_zombies)
        remove_zombies();
    struct task *previous = current_task;
    if (previous->state == TASK_RUNNING)
        previous->state = TASK_READY;

    struct task *next = next_ready_task();
    if (next == NULL) {
        previous->state = TASK_RUNNING;
        interrupt_restore(flags);
        return;
    }
    if (next == previous) {
        previous->state = TASK_RUNNING;
        interrupt_restore(flags);
        return;
    }

    next->state = TASK_RUNNING;
    current_task = next;
    gdt_set_kernel_stack(next->stack.top);
    vmm_space_activate(next->space);
    task_context_switch(&previous->stack_pointer, next->stack_pointer);
    interrupt_restore(flags);
}

void task_yield(void) { schedule(1); }

void task_preempt(void) {
    if (preemption_enabled)
        schedule(0);
}

size_t task_count(void) {
    uint64_t flags = interrupt_lock();
    remove_zombies();
    size_t count = 0;
    for (struct task *task = task_list; task != NULL; task = task->next)
        count++;
    interrupt_restore(flags);
    return count;
}

int task_get_info(size_t index, struct task_info *info) {
    if (info == NULL)
        return 0;

    uint64_t flags = interrupt_lock();
    remove_zombies();
    struct task *task = task_list;
    while (task != NULL && index > 0) {
        task = task->next;
        index--;
    }
    if (task == NULL) {
        interrupt_restore(flags);
        return 0;
    }

    info->id = task->id;
    info->name = task->name;
    info->state = task->state;
    interrupt_restore(flags);
    return 1;
}

uint64_t task_current_stack_guard(void) {
    return current_task != NULL ? current_task->stack.guard_base : 0;
}

int task_current_stack_guard_contains(uint64_t address) {
    return current_task != NULL &&
           kernel_stack_contains_guard(&current_task->stack, address);
}
