#include "lib/syscall.h"
#include <stdint.h>
__attribute__((noreturn)) void _start(uint64_t argc, char **argv) {
    if (argc < 2) { sys_write("usage: mkdir <dir>\n", 19); sys_exit(1); }
    if (sys_mkdir(argv[1]) < 0) { sys_write("mkdir: failed\n", 14); sys_exit(1); }
    sys_exit(0);
}
