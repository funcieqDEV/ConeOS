#include "syscall.h"
#include "../drivers/serial.h"
#include "../gfx/console.h"
#include "../kernel/process.h"
#include "../mm/pmm.h"
#include "../mm/vmm.h"
#include "gdt.h"
#include "idt.h"
#include <stdint.h>

#define SYSCALL_VECTOR 0x80
#define IDT_USER_INT_GATE 0xEE
#define SYSCALL_WRITE 1
#define SYSCALL_EXIT 2
#define MAX_WRITE_LENGTH 4096

struct syscall_frame {
    uint64_t r15;
    uint64_t r14;
    uint64_t r13;
    uint64_t r12;
    uint64_t r11;
    uint64_t r10;
    uint64_t r9;
    uint64_t r8;
    uint64_t rbp;
    uint64_t rdi;
    uint64_t rsi;
    uint64_t rdx;
    uint64_t rcx;
    uint64_t rbx;
    uint64_t rax;
    uint64_t ip;
    uint64_t cs;
    uint64_t flags;
    uint64_t sp;
    uint64_t ss;
};

static int user_range_readable(uint64_t address, uint64_t length) {
    if (length > MAX_WRITE_LENGTH || address >= 0x0000800000000000ULL ||
        address + length < address)
        return 0;

    uint64_t end = address + length;
    for (uint64_t page = address & ~(PMM_PAGE_SIZE - 1); page < end;
         page += PMM_PAGE_SIZE) {
        if (vmm_translate(page) == PMM_INVALID_ADDRESS)
            return 0;
    }
    return 1;
}

static void syscall_write(uint64_t address, uint64_t length) {
    if (!user_range_readable(address, length))
        return;

    for (uint64_t i = 0; i < length; i++) {
        char c = ((const char *)address)[i];
        print_serial_char(c);
        console_putchar(c);
    }
}

void syscall_dispatch(struct syscall_frame *frame) {
    if ((frame->cs & 3) != 3)
        return;

    switch (frame->rax) {
    case SYSCALL_WRITE:
        syscall_write(frame->rbx, frame->rcx);
        break;
    case SYSCALL_EXIT:
        process_exit_current();
        break;
    default:
        break;
    }
}

extern void syscall_stub(void);

__asm__(".global syscall_stub\n"
        ".type syscall_stub, @function\n"
        "syscall_stub:\n"
        "    pushq %rax\n"
        "    pushq %rbx\n"
        "    pushq %rcx\n"
        "    pushq %rdx\n"
        "    pushq %rsi\n"
        "    pushq %rdi\n"
        "    pushq %rbp\n"
        "    pushq %r8\n"
        "    pushq %r9\n"
        "    pushq %r10\n"
        "    pushq %r11\n"
        "    pushq %r12\n"
        "    pushq %r13\n"
        "    pushq %r14\n"
        "    pushq %r15\n"
        "    movq %rsp, %rdi\n"
        "    cld\n"
        "    callq syscall_dispatch\n"
        "    popq %r15\n"
        "    popq %r14\n"
        "    popq %r13\n"
        "    popq %r12\n"
        "    popq %r11\n"
        "    popq %r10\n"
        "    popq %r9\n"
        "    popq %r8\n"
        "    popq %rbp\n"
        "    popq %rdi\n"
        "    popq %rsi\n"
        "    popq %rdx\n"
        "    popq %rcx\n"
        "    popq %rbx\n"
        "    popq %rax\n"
        "    iretq\n"
        ".size syscall_stub, .-syscall_stub\n");

void syscall_init(void) {
    idt_set_gate(SYSCALL_VECTOR, (uint64_t)syscall_stub, GDT_KERNEL_CODE,
                 IDT_USER_INT_GATE, 0);
}
