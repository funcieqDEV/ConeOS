#include "lib/syscall.h"
#include <stdint.h>
__attribute__((noreturn)) void _start(uint64_t argc, char **argv) {
    if (argc < 2) { sys_write("usage: rm <file>\n", 17); sys_exit(1); }
    if (sys_unlink(argv[1]) < 0) { sys_write("rm: failed\n", 11); sys_exit(1); }
    sys_exit(0);
}
