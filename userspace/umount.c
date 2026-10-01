#include "lib/syscall.h"
#include <stdint.h>

__attribute__((noreturn)) void _start(uint64_t argc, char **argv) {
    if (argc != 2) {
        static const char usage[] = "usage: umount <directory>\n";
        sys_write(usage, sizeof(usage) - 1);
        sys_exit(1);
    }
    if (sys_unmount(argv[1]) < 0) {
        static const char error[] = "umount: busy or not mounted\n";
        sys_write(error, sizeof(error) - 1);
        sys_exit(1);
    }
    sys_exit(0);
}
