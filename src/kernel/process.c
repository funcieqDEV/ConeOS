#include "process.h"
#include "../mm/pmm.h"
#include "../mm/vmm.h"
#include "task.h"
#include <stddef.h>
#include <stdint.h>

#define USER_CODE_ADDRESS 0x0000000000400000ULL
#define USER_STACK_ADDRESS 0x0000000000800000ULL
#define USER_STACK_TOP (USER_STACK_ADDRESS + PMM_PAGE_SIZE)

static int process_running;

extern void process_enter_user(uint64_t instruction_pointer,
                               uint64_t stack_pointer);

__asm__(".global process_enter_user\n"
        ".type process_enter_user, @function\n"
        "process_enter_user:\n"
        "    cli\n"
        "    movw $0x1b, %ax\n"
        "    movw %ax, %ds\n"
        "    movw %ax, %es\n"
        "    pushq $0x1b\n"
        "    pushq %rsi\n"
        "    pushfq\n"
        "    orq $0x200, (%rsp)\n"
        "    pushq $0x23\n"
        "    pushq %rdi\n"
        "    iretq\n"
        ".size process_enter_user, .-process_enter_user\n");

static void write_u64(uint8_t *destination, uint64_t value) {
    for (size_t i = 0; i < 8; i++)
        destination[i] = (uint8_t)(value >> (i * 8));
}

static void build_hello_program(uint8_t *code) {
    static const char message[] = "Hello from ring 3!\n";
    size_t offset = 0;

    uint8_t prefix[] = {
        0xB8,
        1,
        0,
        0,
        0,
        0x48,
        0xBB,
        0,
        0,
        0,
        0,
        0,
        0,
        0,
        0,
        0xB9,
        sizeof(message) - 1,
        0,
        0,
        0,
        0xCD,
        0x80,
        0xB8,
        2,
        0,
        0,
        0,
        0xCD,
        0x80,
        0x0F,
        0x0B,
    };

    for (size_t i = 0; i < sizeof(prefix); i++)
        code[offset++] = prefix[i];

    write_u64(&code[7], USER_CODE_ADDRESS + offset);
    for (size_t i = 0; i < sizeof(message) - 1; i++)
        code[offset++] = message[i];
}

static void user_process_task(void *argument) {
    (void)argument;
    process_enter_user(USER_CODE_ADDRESS, USER_STACK_TOP);
    __builtin_unreachable();
}

int process_run_hello(void) {
    if (process_running)
        return 0;

    struct vmm_space *space = vmm_space_create();
    if (space == NULL)
        return 0;

    uint64_t code_physical = pmm_alloc_page();
    uint64_t stack_physical = pmm_alloc_page();
    if (code_physical == PMM_INVALID_ADDRESS ||
        stack_physical == PMM_INVALID_ADDRESS)
        goto failure;

    if (!vmm_space_map_page(space, USER_CODE_ADDRESS, code_physical, VMM_USER))
        goto failure;
    if (!vmm_space_map_page(space, USER_STACK_ADDRESS, stack_physical,
                            VMM_USER | VMM_WRITABLE | VMM_NO_EXECUTE))
        goto failure;

    uint8_t *code = pmm_physical_to_virtual(code_physical);
    for (size_t i = 0; i < PMM_PAGE_SIZE; i++)
        code[i] = 0;
    build_hello_program(code);

    if (task_create_in_space("user-hello", user_process_task, NULL, space) ==
        UINT64_MAX)
        goto failure;

    process_running = 1;
    return 1;

failure:
    if (vmm_space_translate(space, USER_CODE_ADDRESS) == PMM_INVALID_ADDRESS &&
        code_physical != PMM_INVALID_ADDRESS)
        pmm_free_page(code_physical);
    if (vmm_space_translate(space, USER_STACK_ADDRESS) == PMM_INVALID_ADDRESS &&
        stack_physical != PMM_INVALID_ADDRESS)
        pmm_free_page(stack_physical);
    vmm_space_destroy(space);
    return 0;
}

int process_is_running(void) { return process_running; }

__attribute__((noreturn)) void process_exit_current(void) {
    process_running = 0;
    task_exit_current();
}
