#include "lib/syscall.h"
#include <stdint.h>

static uint64_t length(const char *s) { uint64_t n = 0; while (s[n]) n++; return n; }

__attribute__((noreturn)) void _start(uint64_t argc, char **argv) {
    if (argc < 2) {
        char buffer[256];
        for (;;) {
            int64_t count = sys_read_fd(0, buffer, sizeof(buffer));
            if (count < 0) sys_exit(1);
            if (count == 0) sys_exit(0);
            sys_write(buffer, (uint64_t)count);
        }
    }
    int64_t fd = sys_open(argv[1], 0);
    if (fd < 0) { sys_write("cat: not found: ", 16); sys_write(argv[1], length(argv[1])); sys_write("\n", 1); sys_exit(1); }
    char buffer[256]; int64_t count;
    while ((count = sys_read_fd((int)fd, buffer, sizeof(buffer))) > 0) sys_write(buffer, (uint64_t)count);
    sys_close((int)fd); sys_exit(count < 0 ? 1 : 0);
}
