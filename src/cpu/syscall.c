#include "syscall.h"
#include "../drivers/pci.h"
#include "../drivers/pit.h"
#include "../drivers/rtc.h"
#include "../fs/dirent.h"
#include "../fs/vfs.h"
#include "../kernel/process.h"
#include "../kernel/task.h"
#include "../mm/pmm.h"
#include "../mm/usercopy.h"
#include "../mm/vmm.h"
#include "gdt.h"
#include "idt.h"
#include "syscall_numbers.h"
#include <stdint.h>

#define SYSCALL_VECTOR 0x80
#define IDT_USER_INT_GATE 0xEE
#define MAX_WRITE_LENGTH 4096
#define MAX_EXEC_ARGUMENT_BYTES 2048

static uint64_t syscall_write(uint64_t address, uint64_t length) {
    if (length > MAX_WRITE_LENGTH)
        return SYSCALL_EINVAL;
    if (!user_range_readable(address, length))
        return SYSCALL_EFAULT;

    char buffer[MAX_WRITE_LENGTH];
    if (!copy_from_user(buffer, address, length))
        return SYSCALL_EFAULT;
    size_t written = 0;
    if (vfs_write(1, buffer, (size_t)length, &written) < 0)
        return SYSCALL_EINVAL;
    return written;
}

static uint64_t syscall_read(uint64_t address, uint64_t length) {
    if (length > MAX_WRITE_LENGTH)
        return SYSCALL_EINVAL;
    if (!user_range_writable(address, length))
        return SYSCALL_EFAULT;
    if (length == 0)
        return 0;

    char buffer[MAX_WRITE_LENGTH];
    size_t read = 0;
    if (vfs_read(0, buffer, (size_t)length, &read) < 0)
        return SYSCALL_EINVAL;
    if (!copy_to_user(address, buffer, read))
        return SYSCALL_EFAULT;
    return read;
}

static int leap_year(uint64_t year) {
    return year % 4 == 0 && (year % 100 != 0 || year % 400 == 0);
}

static uint64_t rtc_seconds_since_epoch(void) {
    static const uint16_t days_before_month[] = {
        0, 0, 31, 59, 90, 120, 151, 181, 212, 243, 273, 304, 334,
    };
    struct rtc_time time;
    if (!rtc_read(&time) || time.year < 1970)
        return (uint64_t)SYSCALL_EINVAL;

    uint64_t days = 0;
    for (uint64_t year = 1970; year < time.year; year++)
        days += leap_year(year) ? 366 : 365;
    days += days_before_month[time.month] + time.day - 1;
    if (time.month > 2 && leap_year(time.year))
        days++;
    return days * 86400 + time.hour * 3600 + time.minute * 60 + time.second;
}

static void append_list_entry(char *output, size_t *used, size_t capacity,
                              const char *name, int directory) {
    while (*name && *used < capacity)
        output[(*used)++] = *name++;
    if (directory && *used < capacity)
        output[(*used)++] = '/';
    if (*used < capacity)
        output[(*used)++] = '\n';
}

void syscall_dispatch(struct syscall_frame *frame) {
    if ((frame->cs & 3) != 3) {
        frame->rax = SYSCALL_EPERM;
        return;
    }

    switch (frame->rax) {
    case SYSCALL_WRITE:
        frame->rax = syscall_write(frame->rbx, frame->rcx);
        break;
    case SYSCALL_EXIT:
        process_exit_current((int)frame->rbx);
        break;
    case SYSCALL_GETPID:
        frame->rax = task_current_id();
        break;
    case SYSCALL_YIELD:
        task_yield();
        frame->rax = 0;
        break;
    case SYSCALL_UPTIME:
        frame->rax = pit_uptime_ms();
        break;
    case SYSCALL_READ:
        frame->rax = syscall_read(frame->rbx, frame->rcx);
        break;
    case SYSCALL_SLEEP:
        task_sleep_ms(frame->rbx);
        frame->rax = 0;
        break;
    case SYSCALL_GETTIME:
        frame->rax = rtc_seconds_since_epoch();
        break;
    case SYSCALL_WAITPID: {
        int status;
        int64_t result = process_waitpid(frame->rbx, &status);
        if (result < 0) {
            frame->rax = SYSCALL_EINVAL;
            break;
        }
        if (!copy_to_user(frame->rcx, &status, sizeof(status))) {
            frame->rax = SYSCALL_EFAULT;
            break;
        }
        frame->rax = result;
        break;
    }
    case SYSCALL_OPEN: {
        char path[FS_PATH_MAX];
        if (frame->rcx >= sizeof(path)) {
            frame->rax = SYSCALL_EINVAL;
            break;
        }
        size_t length = frame->rcx;
        if (!copy_from_user(path, frame->rbx, length)) {
            frame->rax = SYSCALL_EFAULT;
            break;
        }
        path[length] = '\0';
        frame->rax = vfs_open(path, (int)frame->rdx);
        break;
    }
    case SYSCALL_CLOSE:
        frame->rax = vfs_close((int)frame->rbx);
        break;
    case SYSCALL_READ_FD: {
        size_t length =
            frame->rcx > MAX_WRITE_LENGTH ? MAX_WRITE_LENGTH : frame->rcx;
        char buffer[MAX_WRITE_LENGTH];
        size_t count;
        if (!user_range_writable(frame->rdx, length)) {
            frame->rax = SYSCALL_EFAULT;
            break;
        }
        if (vfs_read((int)frame->rbx, buffer, length, &count) < 0) {
            frame->rax = SYSCALL_EINVAL;
            break;
        }
        if (!copy_to_user(frame->rdx, buffer, count)) {
            frame->rax = SYSCALL_EFAULT;
            break;
        }
        frame->rax = count;
        break;
    }
    case SYSCALL_WRITE_FD: {
        size_t length =
            frame->rcx > MAX_WRITE_LENGTH ? MAX_WRITE_LENGTH : frame->rcx;
        char buffer[MAX_WRITE_LENGTH];
        if (!copy_from_user(buffer, frame->rdx, length)) {
            frame->rax = SYSCALL_EFAULT;
            break;
        }
        size_t count;
        if (vfs_write((int)frame->rbx, buffer, length, &count) < 0) {
            frame->rax = SYSCALL_EINVAL;
            break;
        }
        frame->rax = count;
        break;
    }
    case SYSCALL_EXEC: {
        char path[FS_PATH_MAX];
        if (frame->rcx == 0 || frame->rcx >= sizeof(path)) {
            frame->rax = SYSCALL_EINVAL;
            break;
        }
        size_t length = frame->rcx;
        if (!copy_from_user(path, frame->rbx, length)) {
            frame->rax = SYSCALL_EFAULT;
            break;
        }
        path[length] = '\0';

        char argument_storage[MAX_EXEC_ARGUMENT_BYTES];
        const char *arguments[PROCESS_MAX_ARGUMENTS];
        size_t argc = 0, used = 0;
        int64_t error = 0;
        int terminated = frame->rdx == 0;
        while (!terminated && argc <= PROCESS_MAX_ARGUMENTS) {
            uint64_t argument_pointer;
            uint64_t slot = frame->rdx + argc * sizeof(uint64_t);
            if (slot < frame->rdx ||
                !copy_from_user(&argument_pointer, slot,
                                sizeof(argument_pointer))) {
                error = SYSCALL_EFAULT;
                break;
            }
            if (argument_pointer == 0) {
                terminated = 1;
                break;
            }
            if (argc == PROCESS_MAX_ARGUMENTS) {
                error = SYSCALL_EINVAL;
                break;
            }

            size_t argument_start = used;
            arguments[argc] = &argument_storage[argument_start];
            for (;;) {
                size_t argument_length = used - argument_start;
                if (used == sizeof(argument_storage) ||
                    argument_pointer > UINT64_MAX - argument_length) {
                    error = SYSCALL_EINVAL;
                    break;
                }
                uint8_t character;
                if (!copy_from_user(&character,
                                    argument_pointer + argument_length, 1)) {
                    error = SYSCALL_EFAULT;
                    break;
                }
                argument_storage[used++] = (char)character;
                if (character == '\0')
                    break;
            }
            if (error < 0)
                break;
            argc++;
        }
        if (error < 0) {
            frame->rax = (uint64_t)error;
            break;
        }
        if (!terminated) {
            frame->rax = SYSCALL_EINVAL;
            break;
        }
        int64_t result = process_exec_path_args(path, arguments, argc);
        if (result < 0)
            frame->rax = (uint64_t)result;
        break;
    }
    case SYSCALL_LIST: {
        size_t capacity =
            frame->rcx > MAX_WRITE_LENGTH ? MAX_WRITE_LENGTH : frame->rcx;
        if (!user_range_writable(frame->rbx, capacity)) {
            frame->rax = SYSCALL_EFAULT;
            break;
        }
        char output[MAX_WRITE_LENGTH];
        size_t used = 0;
        char cwd[FS_PATH_MAX];
        if (!process_getcwd(cwd, sizeof(cwd))) {
            frame->rax = SYSCALL_EINVAL;
            break;
        }
        for (size_t index = 0; used < capacity; index++) {
            struct fs_dirent entry;
            int result = vfs_readdir(cwd, index, &entry);
            if (result < 0) {
                frame->rax = SYSCALL_EINVAL;
                break;
            }
            if (result == 0)
                break;
            append_list_entry(output, &used, capacity, entry.name,
                              entry.type == FS_DIRENT_DIRECTORY);
        }
        if ((int64_t)frame->rax == SYSCALL_EINVAL)
            break;
        if (!copy_to_user(frame->rbx, output, used)) {
            frame->rax = SYSCALL_EFAULT;
            break;
        }
        frame->rax = used;
        break;
    }
    case SYSCALL_READDIR: {
        char path[FS_PATH_MAX];
        size_t length = frame->rcx;
        if (length >= sizeof(path) ||
            !copy_from_user(path, frame->rbx, length)) {
            frame->rax =
                length >= sizeof(path) ? SYSCALL_EINVAL : SYSCALL_EFAULT;
            break;
        }
        path[length] = '\0';
        if (!user_range_writable(frame->rsi, sizeof(struct fs_dirent))) {
            frame->rax = SYSCALL_EFAULT;
            break;
        }
        struct fs_dirent entry;
        int result = vfs_readdir(path, (size_t)frame->rdx, &entry);
        if (result > 0 && !copy_to_user(frame->rsi, &entry, sizeof(entry))) {
            frame->rax = SYSCALL_EFAULT;
            break;
        }
        frame->rax = result < 0 ? SYSCALL_EINVAL : result;
        break;
    }
    case SYSCALL_SPAWN: {
        char path[FS_PATH_MAX];
        if (frame->rcx >= sizeof(path)) {
            frame->rax = SYSCALL_EINVAL;
            break;
        }
        size_t length = frame->rcx;
        if (!copy_from_user(path, frame->rbx, length)) {
            frame->rax = SYSCALL_EFAULT;
            break;
        }
        path[length] = '\0';
        char argument[64];
        argument[0] = '\0';
        int copy_failed = 0;
        if (frame->rdx != 0) {
            size_t i = 0;
            for (; i < sizeof(argument) - 1; i++) {
                if (!copy_from_user(&argument[i], frame->rdx + i, 1)) {
                    copy_failed = 1;
                    break;
                }
                if (!argument[i])
                    break;
            }
            argument[sizeof(argument) - 1] = '\0';
            if (copy_failed) {
                frame->rax = SYSCALL_EFAULT;
                break;
            }
        }
        int64_t pid =
            process_spawn_child_args(path, argument[0] ? argument : NULL);
        frame->rax = (uint64_t)pid;
        break;
    }
    case SYSCALL_SEEK:
        frame->rax =
            vfs_seek((int)frame->rbx, (int64_t)frame->rcx, (int)frame->rdx);
        break;
    case SYSCALL_STAT: {
        char path[FS_PATH_MAX];
        if (frame->rcx >= sizeof(path)) {
            frame->rax = SYSCALL_EINVAL;
            break;
        }
        size_t length = frame->rcx;
        if (!copy_from_user(path, frame->rbx, length)) {
            frame->rax = SYSCALL_EFAULT;
            break;
        }
        path[length] = '\0';
        size_t size;
        if (!vfs_stat(path, &size)) {
            frame->rax = SYSCALL_EINVAL;
            break;
        }
        uint64_t result = size;
        if (!copy_to_user(frame->rdx, &result, sizeof(result))) {
            frame->rax = SYSCALL_EFAULT;
            break;
        }
        frame->rax = 0;
        break;
    }
    case SYSCALL_GETCWD: {
        char cwd[FS_PATH_MAX];
        if (!process_getcwd(cwd, sizeof(cwd))) {
            frame->rax = SYSCALL_EINVAL;
            break;
        }
        size_t length = 0;
        while (cwd[length])
            length++;
        if (frame->rcx <= length ||
            !copy_to_user(frame->rbx, cwd, length + 1)) {
            frame->rax = SYSCALL_EFAULT;
            break;
        }
        frame->rax = length;
        break;
    }
    case SYSCALL_CHDIR: {
        char path[FS_PATH_MAX];
        if (frame->rcx >= sizeof(path)) {
            frame->rax = SYSCALL_EINVAL;
            break;
        }
        size_t length = frame->rcx;
        if (!copy_from_user(path, frame->rbx, length)) {
            frame->rax = SYSCALL_EFAULT;
            break;
        }
        path[length] = '\0';
        frame->rax = process_chdir(path) ? 0 : SYSCALL_EINVAL;
        break;
    }
    case SYSCALL_MKDIR:
    case SYSCALL_UNLINK: {
        char path[FS_PATH_MAX];
        if (frame->rcx >= sizeof(path)) {
            frame->rax = SYSCALL_EINVAL;
            break;
        }
        size_t length = frame->rcx;
        if (!copy_from_user(path, frame->rbx, length)) {
            frame->rax = SYSCALL_EFAULT;
            break;
        }
        path[length] = '\0';
        int ok =
            frame->rax == SYSCALL_MKDIR ? vfs_mkdir(path) : vfs_unlink(path);
        frame->rax = ok ? 0 : SYSCALL_EINVAL;
        break;
    }
    case SYSCALL_RMDIR: {
        char path[FS_PATH_MAX];
        if (frame->rcx >= sizeof(path)) {
            frame->rax = SYSCALL_EINVAL;
            break;
        }
        size_t length = frame->rcx;
        if (!copy_from_user(path, frame->rbx, length)) {
            frame->rax = SYSCALL_EFAULT;
            break;
        }
        path[length] = '\0';
        frame->rax = vfs_rmdir(path) ? 0 : SYSCALL_EINVAL;
        break;
    }
    case SYSCALL_PCI_LIST: {
        struct pci_device_info info;
        if (!pci_get_device(frame->rbx, &info)) {
            frame->rax = SYSCALL_EINVAL;
            break;
        }
        if (!copy_to_user(frame->rcx, &info, sizeof(info))) {
            frame->rax = SYSCALL_EFAULT;
            break;
        }
        frame->rax = 0;
        break;
    }
    case SYSCALL_SYNC:
        frame->rax = vfs_sync() ? 0 : SYSCALL_EINVAL;
        break;
    case SYSCALL_MOUNT: {
        char type[16], target[FS_PATH_MAX];
        size_t type_length = (size_t)frame->rcx;
        size_t target_length = (size_t)frame->rsi;
        if (type_length == 0 || type_length >= sizeof(type) ||
            target_length == 0 || target_length >= sizeof(target)) {
            frame->rax = SYSCALL_EINVAL;
            break;
        }
        if (!copy_from_user(type, frame->rbx, type_length) ||
            !copy_from_user(target, frame->rdx, target_length)) {
            frame->rax = SYSCALL_EFAULT;
            break;
        }
        type[type_length] = '\0';
        target[target_length] = '\0';
        frame->rax = vfs_mount(type, target) ? 0 : SYSCALL_EINVAL;
        break;
    }
    case SYSCALL_UMOUNT: {
        char target[FS_PATH_MAX];
        size_t length = (size_t)frame->rcx;
        if (length == 0 || length >= sizeof(target)) {
            frame->rax = SYSCALL_EINVAL;
            break;
        }
        if (!copy_from_user(target, frame->rbx, length)) {
            frame->rax = SYSCALL_EFAULT;
            break;
        }
        target[length] = '\0';
        frame->rax = vfs_unmount(target) ? 0 : SYSCALL_EINVAL;
        break;
    }
    case SYSCALL_MOUNT_INFO: {
        struct fs_mount_info info;
        if (!user_range_writable(frame->rcx, sizeof(info))) {
            frame->rax = SYSCALL_EFAULT;
            break;
        }
        if (!vfs_mount_info((size_t)frame->rbx, &info)) {
            frame->rax = 0;
            break;
        }
        if (!copy_to_user(frame->rcx, &info, sizeof(info))) {
            frame->rax = SYSCALL_EFAULT;
            break;
        }
        frame->rax = 1;
        break;
    }
    case SYSCALL_BRK: {
        frame->rax = (uint64_t)process_brk(frame->rbx);
        break;
    }
    case SYSCALL_PIPE: {
        int descriptors[2];
        if (!user_range_writable(frame->rbx, sizeof(descriptors))) {
            frame->rax = SYSCALL_EFAULT;
            break;
        }
        if (vfs_pipe(descriptors) < 0) {
            frame->rax = SYSCALL_EINVAL;
            break;
        }
        if (!copy_to_user(frame->rbx, descriptors, sizeof(descriptors))) {
            vfs_close(descriptors[0]);
            vfs_close(descriptors[1]);
            frame->rax = SYSCALL_EFAULT;
            break;
        }
        frame->rax = 0;
        break;
    }
    case SYSCALL_DUP:
        frame->rax = vfs_dup((int)frame->rbx);
        break;
    case SYSCALL_DUP2:
        frame->rax = vfs_dup2((int)frame->rbx, (int)frame->rcx);
        break;
    case SYSCALL_MMAP:
        frame->rax =
            process_mmap(frame->rbx, frame->rcx, frame->rdx, frame->rsi);
        break;
    case SYSCALL_MUNMAP:
        frame->rax = process_munmap(frame->rbx, frame->rcx);
        break;
    case SYSCALL_FORK:
        frame->rax = process_fork(frame);
        break;
    default:
        frame->rax = SYSCALL_ENOSYS;
        break;
    }
}

extern void syscall_stub(void);
__asm__(".global syscall_resume_user\n"
        ".type syscall_resume_user, @function\n"
        "syscall_resume_user:\n"
        "    movq %rdi, %rsp\n"
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
        ".size syscall_resume_user, .-syscall_resume_user\n");

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
        "    pushfq\n"
        "    andq $-262145, (%rsp)\n"
        "    popfq\n"
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
