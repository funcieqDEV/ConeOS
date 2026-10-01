#pragma once

#include <stddef.h>
#include <stdint.h>
#include "../../src/fs/dirent.h"
#include "../../src/cpu/syscall_numbers.h"

int64_t sys_write(const void *buffer, size_t length);
int64_t sys_read(void *buffer, size_t length);
uint64_t sys_getpid(void);
void sys_yield(void);
uint64_t sys_uptime(void);
int64_t sys_sleep(uint64_t milliseconds);
int64_t sys_gettime(void);
int64_t sys_waitpid(uint64_t pid, int *status);
int64_t sys_open(const char *path, int create);
int64_t sys_close(int fd);
int64_t sys_read_fd(int fd, void *buffer, size_t length);
int64_t sys_write_fd(int fd, const void *buffer, size_t length);
int64_t sys_pipe(int descriptors[2]);
int64_t sys_dup(int descriptor);
int64_t sys_dup2(int source, int destination);
int64_t sys_exec(const char *path);
int64_t sys_execv(const char *path, const char *const arguments[]);
int64_t sys_list(void *buffer, size_t length);
int64_t sys_readdir(const char *path, uint64_t index, struct fs_dirent *entry);
int64_t sys_spawn(const char *path, const char *argument);
int64_t sys_seek(int fd, int64_t offset, int whence);
int64_t sys_stat(const char *path, uint64_t *size);
int64_t sys_getcwd(char *buffer, size_t capacity);
int64_t sys_chdir(const char *path);
int64_t sys_mkdir(const char *path);
int64_t sys_unlink(const char *path);
int64_t sys_rmdir(const char *path);
int64_t sys_sync(void);
int64_t sys_mount(const char *type, const char *target);
int64_t sys_unmount(const char *target);
int64_t sys_mount_info(uint64_t index, struct fs_mount_info *info);
int64_t sys_brk(uint64_t address);
int64_t sys_mmap(void *address, size_t length, uint64_t protection,
                 uint64_t flags);
int64_t sys_munmap(void *address, size_t length);
int64_t sys_fork(void);
struct user_pci_device {
    uint16_t vendor_id; uint16_t device_id;
    uint8_t class_code, subclass, bus, slot, function;
};
int64_t sys_pci_get(uint64_t index, struct user_pci_device *device);
int64_t sys_invoke(uint64_t number, uint64_t first, uint64_t second);
int64_t sys_invoke3(uint64_t number, uint64_t first, uint64_t second, uint64_t third);
int64_t sys_invoke4(uint64_t number, uint64_t first, uint64_t second,
                    uint64_t third, uint64_t fourth);
__attribute__((noreturn)) void sys_exit(int status);
