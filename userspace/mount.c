#include "lib/syscall.h"
#include <stdint.h>

__attribute__((noreturn)) void _start(uint64_t argc, char **argv) {
    if (argc == 1) {
        struct fs_mount_info info;
        for (uint64_t index = 0; sys_mount_info(index, &info) > 0; index++) {
            uint64_t type_length = 0, path_length = 0;
            while (info.type[type_length]) type_length++;
            while (info.path[path_length]) path_length++;
            sys_write(info.type, type_length);
            sys_write(" ", 1);
            sys_write(info.path, path_length);
            sys_write("\n", 1);
        }
        sys_exit(0);
    }
    if (argc != 3) {
        static const char usage[] = "usage: mount [<type> <directory>]\n";
        sys_write(usage, sizeof(usage) - 1);
        sys_exit(1);
    }
    if (sys_mount(argv[1], argv[2]) < 0) {
        static const char error[] = "mount: operation failed\n";
        sys_write(error, sizeof(error) - 1);
        sys_exit(1);
    }
    sys_exit(0);
}
