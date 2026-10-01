#include "syscall.h"
#include "../../src/cpu/syscall_numbers.h"

int64_t sys_invoke(uint64_t number, uint64_t first, uint64_t second) {
    __asm__ volatile("int $0x80"
                     : "+a"(number)
                     : "b"(first), "c"(second)
                     : "memory");
    return (int64_t)number;
}

int64_t sys_invoke3(uint64_t number, uint64_t first, uint64_t second,
                    uint64_t third) {
    __asm__ volatile("int $0x80" : "+a"(number) : "b"(first), "c"(second), "d"(third) : "memory");
    return (int64_t)number;
}

int64_t sys_invoke4(uint64_t number, uint64_t first, uint64_t second,
                    uint64_t third, uint64_t fourth) {
    __asm__ volatile("int $0x80" : "+a"(number)
                     : "b"(first), "c"(second), "d"(third), "S"(fourth)
                     : "memory");
    return (int64_t)number;
}

int64_t sys_write(const void *buffer, size_t length) {
    return sys_invoke(SYSCALL_WRITE, (uint64_t)buffer, length);
}

int64_t sys_read(void *buffer, size_t length) {
    return sys_invoke(SYSCALL_READ, (uint64_t)buffer, length);
}

uint64_t sys_getpid(void) {
    return (uint64_t)sys_invoke(SYSCALL_GETPID, 0, 0);
}

void sys_yield(void) { (void)sys_invoke(SYSCALL_YIELD, 0, 0); }

uint64_t sys_uptime(void) {
    return (uint64_t)sys_invoke(SYSCALL_UPTIME, 0, 0);
}

int64_t sys_sleep(uint64_t milliseconds) {
    return sys_invoke(SYSCALL_SLEEP, milliseconds, 0);
}

int64_t sys_gettime(void) { return sys_invoke(SYSCALL_GETTIME, 0, 0); }

int64_t sys_waitpid(uint64_t pid, int *status) {
    return sys_invoke(SYSCALL_WAITPID, pid, (uint64_t)status);
}

int64_t sys_open(const char *path, int create) {
    size_t length = 0; while (path[length]) length++;
    return sys_invoke3(SYSCALL_OPEN, (uint64_t)path, length, create);
}

int64_t sys_close(int fd) { return sys_invoke(SYSCALL_CLOSE, fd, 0); }
int64_t sys_read_fd(int fd, void *buffer, size_t length) {
    return sys_invoke3(SYSCALL_READ_FD, fd, length, (uint64_t)buffer);
}
int64_t sys_write_fd(int fd, const void *buffer, size_t length) {
    return sys_invoke3(SYSCALL_WRITE_FD, fd, length, (uint64_t)buffer);
}
int64_t sys_pipe(int descriptors[2]) {
    return sys_invoke(SYSCALL_PIPE, (uint64_t)descriptors, 0);
}
int64_t sys_dup(int descriptor) { return sys_invoke(SYSCALL_DUP, descriptor, 0); }
int64_t sys_dup2(int source, int destination) {
    return sys_invoke(SYSCALL_DUP2, source, destination);
}

int64_t sys_execv(const char *path, const char *const arguments[]) {
    size_t length = 0; while (path[length]) length++;
    return sys_invoke3(SYSCALL_EXEC, (uint64_t)path, length,
                       (uint64_t)arguments);
}

int64_t sys_exec(const char *path) {
    const char *arguments[] = {path, 0};
    return sys_execv(path, arguments);
}

int64_t sys_list(void *buffer, size_t length) {
    return sys_invoke(SYSCALL_LIST, (uint64_t)buffer, length);
}

int64_t sys_readdir(const char *path, uint64_t index, struct fs_dirent *entry) {
    size_t length = 0;
    while (path[length]) length++;
    return sys_invoke4(SYSCALL_READDIR, (uint64_t)path, length, index,
                       (uint64_t)entry);
}

int64_t sys_spawn(const char *path, const char *argument) {
    size_t length = 0; while (path[length]) length++;
    return sys_invoke3(SYSCALL_SPAWN, (uint64_t)path, length,
                       (uint64_t)argument);
}

int64_t sys_seek(int fd, int64_t offset, int whence) {
    return sys_invoke3(SYSCALL_SEEK, (uint64_t)fd, (uint64_t)offset,
                       (uint64_t)whence);
}

int64_t sys_stat(const char *path, uint64_t *size) {
    size_t length = 0; while (path[length]) length++;
    return sys_invoke3(SYSCALL_STAT, (uint64_t)path, length,
                       (uint64_t)size);
}

int64_t sys_getcwd(char *buffer, size_t capacity) {
    return sys_invoke(SYSCALL_GETCWD, (uint64_t)buffer, capacity);
}

int64_t sys_chdir(const char *path) {
    size_t length = 0; while (path[length]) length++;
    return sys_invoke(SYSCALL_CHDIR, (uint64_t)path, length);
}

static int64_t path_syscall(uint64_t number, const char *path) {
    size_t length = 0; while (path[length]) length++;
    return sys_invoke(number, (uint64_t)path, length);
}

int64_t sys_mkdir(const char *path) { return path_syscall(SYSCALL_MKDIR, path); }
int64_t sys_unlink(const char *path) { return path_syscall(SYSCALL_UNLINK, path); }
int64_t sys_rmdir(const char *path) { return path_syscall(SYSCALL_RMDIR, path); }
int64_t sys_sync(void) { return sys_invoke(SYSCALL_SYNC, 0, 0); }

int64_t sys_mount(const char *type, const char *target) {
    size_t type_length = 0, target_length = 0;
    while (type[type_length]) type_length++;
    while (target[target_length]) target_length++;
    return sys_invoke4(SYSCALL_MOUNT, (uint64_t)type, type_length,
                       (uint64_t)target, target_length);
}

int64_t sys_unmount(const char *target) {
    size_t length = 0;
    while (target[length]) length++;
    return sys_invoke(SYSCALL_UMOUNT, (uint64_t)target, length);
}

int64_t sys_mount_info(uint64_t index, struct fs_mount_info *info) {
    return sys_invoke(SYSCALL_MOUNT_INFO, index, (uint64_t)info);
}

int64_t sys_brk(uint64_t address) {
    return sys_invoke(SYSCALL_BRK, address, 0);
}

int64_t sys_mmap(void *address, size_t length, uint64_t protection,
                 uint64_t flags) {
    return sys_invoke4(SYSCALL_MMAP, (uint64_t)address, length, protection,
                       flags);
}

int64_t sys_munmap(void *address, size_t length) {
    return sys_invoke(SYSCALL_MUNMAP, (uint64_t)address, length);
}

int64_t sys_fork(void) { return sys_invoke(SYSCALL_FORK, 0, 0); }

int64_t sys_pci_get(uint64_t index, struct user_pci_device *device) {
    return sys_invoke(SYSCALL_PCI_LIST, index, (uint64_t)device);
}

__attribute__((noreturn)) void sys_exit(int status) {
    (void)sys_invoke(SYSCALL_EXIT, (uint64_t)status, 0);
    for (;;)
        __asm__ volatile("pause");
}
