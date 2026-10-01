#include "lib/syscall.h"
#include <stdint.h>

static void text(const char *s) { uint64_t n = 0; while (s[n]) n++; sys_write(s, n); }
static void number(uint64_t value) {
    char buffer[21]; uint64_t i = sizeof(buffer);
    do { buffer[--i] = '0' + value % 10; value /= 10; } while (value);
    sys_write(&buffer[i], sizeof(buffer) - i);
}

__attribute__((noreturn)) void _start(uint64_t argc, char **argv) {
    if (argc < 2) { text("usage: stat <file>\n"); sys_exit(1); }
    uint64_t size;
    if (sys_stat(argv[1], &size) < 0) { text("stat: file not found\n"); sys_exit(1); }
    text("File: "); text(argv[1]); text("\nSize: "); number(size); text(" bytes\n");
    sys_exit(0);
}
