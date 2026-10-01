#include "lib/syscall.h"
#include <stdint.h>

__attribute__((noreturn)) void _start(uint64_t argc, char **argv) {
    for (uint64_t i = 1; i < argc; i++) {
        uint64_t length = 0; while (argv[i][length]) length++;
        if (i > 1) sys_write(" ", 1);
        sys_write(argv[i], length);
    }
    sys_write("\n", 1); sys_exit(0);
}
