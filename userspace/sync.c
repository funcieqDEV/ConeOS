#include "lib/syscall.h"

__attribute__((noreturn)) void _start(uint64_t argc, char **argv) {
    (void)argc;
    (void)argv;
    if (sys_sync() < 0) {
        sys_write("sync: failed\n", 13);
        sys_exit(1);
    }
    sys_exit(0);
}
