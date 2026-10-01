#include "lib/syscall.h"
#include <stdint.h>

__attribute__((noreturn)) void _start(void) {
    uint64_t value = sys_uptime(); char buffer[21]; uint64_t index = sizeof(buffer);
    do { buffer[--index] = '0' + value % 10; value /= 10; } while (value);
    sys_write("up ", 3); sys_write(&buffer[index], sizeof(buffer) - index);
    sys_write(" ms\n", 4); sys_exit(0);
}
