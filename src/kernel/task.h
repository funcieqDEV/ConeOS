#pragma once

#include <stddef.h>
#include <stdint.h>

typedef void (*task_entry_t)(void *argument);
struct vmm_space;

enum task_state {
    TASK_READY,
    TASK_RUNNING,
    TASK_BLOCKED,
    TASK_SLEEPING,
    TASK_ZOMBIE,
};

struct task_info {
    uint64_t id;
    const char *name;
    enum task_state state;
};

void task_init(void);
void task_enable_preemption(void);
uint64_t task_create(const char *name, task_entry_t entry, void *argument);
uint64_t task_create_in_space(const char *name, task_entry_t entry,
                              void *argument, struct vmm_space *space);
int task_cancel_ready(uint64_t id);
struct vmm_space *task_replace_address_space(struct vmm_space *space);
void task_yield(void);
void task_preempt(void);
__attribute__((noreturn)) void task_exit_current(void);
size_t task_count(void);
int task_get_info(size_t index, struct task_info *info);
uint64_t task_current_stack_guard(void);
int task_current_stack_guard_contains(uint64_t address);
uint64_t task_current_id(void);
void task_block_current(void);
int task_block_current_if(int (*should_block)(void *), void *argument);
void task_wake_blocked(void);
void task_sleep_ms(uint64_t milliseconds);
