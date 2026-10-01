#include "lib/syscall.h"
#include "lib/heap.h"
#include <stdint.h>

__attribute__((noreturn)) void _start(uint64_t argc, char **argv) {
    if (argc < 3) { sys_write("usage: cp <source> <destination>\n", 33); sys_exit(1); }
    int64_t source = sys_open(argv[1], 0);
    int64_t destination = sys_open(argv[2], 1);
    if (source < 0 || destination < 0) {
        sys_write("cp: open failed\n", 16);
        if (source >= 0) sys_close((int)source);
        if (destination >= 0) sys_close((int)destination);
        sys_exit(1);
    }
    char *buffer = malloc(4096);
    if (!buffer) {
        sys_write("cp: out of memory\n", 18);
        sys_close((int)source); sys_close((int)destination); sys_exit(1);
    }
    for (;;) {
        int64_t count = sys_read_fd((int)source, buffer, 4096);
        if (count < 0) {
            sys_write("cp: read failed\n", 16);
            free(buffer); sys_close((int)source); sys_close((int)destination);
            sys_exit(1);
        }
        if (count == 0) break;
        if (sys_write_fd((int)destination, buffer, (size_t)count) != count) {
            sys_write("cp: write failed\n", 17);
            free(buffer);
            sys_close((int)source); sys_close((int)destination); sys_exit(1);
        }
    }
    free(buffer);
    sys_close((int)source); sys_close((int)destination); sys_exit(0);
}
