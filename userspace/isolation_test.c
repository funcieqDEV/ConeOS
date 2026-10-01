#include "lib/syscall.h"
#include <stdint.h>

#define KERNEL_ADDRESS 0xFFFFFFFF80100000ULL
#define PAGE_FAULT_STATUS 142

static volatile uint8_t cow_value = 0x11;

static void write_text(const char *text) {
    size_t length = 0;
    while (text[length]) length++;
    sys_write(text, length);
}

static void read_kernel(void) {
    (void)*(volatile uint8_t *)(uintptr_t)KERNEL_ADDRESS;
}

static void write_kernel(void) {
    *(volatile uint8_t *)(uintptr_t)KERNEL_ADDRESS = 0;
}

static void execute_kernel(void) {
    ((void (*)(void))(uintptr_t)KERNEL_ADDRESS)();
}

static void write_user_text(void) {
    *(volatile uint8_t *)(uintptr_t)read_kernel = 0;
}

static void execute_stack(void) {
    volatile uint8_t instruction[] = {0xC3};
    ((void (*)(void))(uintptr_t)instruction)();
}

static void read_kernel_with_ac(void) {
    __asm__ volatile("pushfq; orq $0x40000, (%%rsp); popfq"
                     : : : "memory", "cc");
    read_kernel();
}

static int expect_fault(const char *name, void (*operation)(void)) {
    int64_t pid = sys_fork();
    if (pid == 0) {
        operation();
        sys_exit(1);
    }
    int status = -1;
    if (pid < 0 || sys_waitpid((uint64_t)pid, &status) != pid ||
        status != PAGE_FAULT_STATUS) {
        write_text("isolation test failed: ");
        write_text(name);
        write_text("\n");
        return 0;
    }
    return 1;
}

__attribute__((noreturn)) void _start(void) {
    if (!expect_fault("kernel read", read_kernel) ||
        !expect_fault("kernel write", write_kernel) ||
        !expect_fault("kernel execution", execute_kernel) ||
        !expect_fault("user text write", write_user_text) ||
        !expect_fault("stack execution", execute_stack) ||
        !expect_fault("kernel read with AC", read_kernel_with_ac))
        sys_exit(1);

    const void *kernel = (const void *)(uintptr_t)KERNEL_ADDRESS;
    if (sys_write_fd(1, kernel, 1) != SYSCALL_EFAULT ||
        sys_read_fd(0, (void *)kernel, 1) != SYSCALL_EFAULT ||
        sys_mmap(0, 4096, CONEOS_PROT_READ | CONEOS_PROT_WRITE | CONEOS_PROT_EXEC,
                  CONEOS_MAP_PRIVATE | CONEOS_MAP_ANONYMOUS) != SYSCALL_EINVAL) {
        write_text("isolation test failed: syscall permissions\n");
        sys_exit(1);
    }

    uint64_t own_pid = sys_getpid();
    __asm__ volatile("pushfq; orq $0x40000, (%%rsp); popfq"
                     : : : "memory", "cc");
    uint64_t returned_pid = sys_getpid();
    int64_t sleep_result = sys_sleep(20);
    __asm__ volatile("pushfq; andq $-262145, (%%rsp); popfq"
                     : : : "memory", "cc");
    if (returned_pid != own_pid || sleep_result < 0) {
        write_text("isolation test failed: syscall/IRQ with AC\n");
        sys_exit(1);
    }

    int64_t pid = sys_fork();
    if (pid == 0) {
        cow_value = 0x22;
        const char *arguments[] = {"echo", "isolation exec passed", 0};
        if (sys_execv("/bin/echo", arguments) < 0) sys_exit(1);
        sys_exit(1);
    }
    int status = -1;
    if (pid < 0 || sys_waitpid((uint64_t)pid, &status) != pid || status != 0 ||
        cow_value != 0x11) {
        write_text("isolation test failed: COW/exec\n");
        sys_exit(1);
    }
    write_text("isolation tests passed\n");
    sys_exit(0);
}
