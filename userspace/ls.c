#include "lib/syscall.h"
#include <stdint.h>

__attribute__((noreturn)) void _start(uint64_t argc, char **argv) {
    char cwd[FS_PATH_MAX];
    const char *path;
    if (argc > 1) {
        path = argv[1];
    } else {
        if (sys_getcwd(cwd, sizeof(cwd)) < 0) {
            sys_write("ls: cannot get current directory\n", 33);
            sys_exit(1);
        }
        path = cwd;
    }

    for (uint64_t index = 0;; index++) {
        struct fs_dirent entry;
        int64_t result = sys_readdir(path, index, &entry);
        if (result < 0) {
            sys_write("ls: directory not found\n", 24);
            sys_exit(1);
        }
        if (result == 0) break;

        size_t length = 0;
        while (entry.name[length]) length++;
        sys_write(entry.name, length);
        if (entry.type == FS_DIRENT_DIRECTORY) sys_write("/", 1);
        sys_write("\n", 1);
    }
    sys_exit(0);
}
