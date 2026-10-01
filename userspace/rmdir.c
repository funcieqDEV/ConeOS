#include "lib/syscall.h"
#include <stdint.h>

__attribute__((noreturn)) void _start(uint64_t argc, char **argv) {
    if (argc < 2) { sys_write("usage: rmdir <directory>\n", 25); sys_exit(1); }
    if (sys_rmdir(argv[1]) < 0) { sys_write("rmdir: failed\n", 15); sys_exit(1); }
    sys_exit(0);
}
