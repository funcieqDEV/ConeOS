#pragma once

#include <stddef.h>
#include <stdint.h>

#define PROCESS_MAX_ARGUMENTS 16
#define PROCESS_EXIT_OUT_OF_MEMORY 137

struct syscall_frame;

int process_run_hello(void);
int process_spawn_path(const char *path);
int64_t process_exec_path(const char *path);
int64_t process_exec_path_args(const char *path, const char *const arguments[],
                               size_t argc);
int64_t process_spawn_child(const char *path);
int64_t process_spawn_child_args(const char *path, const char *argument);
int process_is_running(void);
int process_waitpid(uint64_t pid, int *status);
__attribute__((noreturn)) void process_exit_current(int status);
int process_getcwd(char *buffer, size_t capacity);
int process_chdir(const char *path);
int process_cwd_in_mount(const char *mount_path);
int64_t process_brk(uint64_t requested);
int64_t process_mmap(uint64_t address, uint64_t length, uint64_t protection,
                     uint64_t flags);
int64_t process_munmap(uint64_t address, uint64_t length);
int process_handle_page_fault(uint64_t address, uint64_t error_code);
int64_t process_fork(struct syscall_frame *frame);
