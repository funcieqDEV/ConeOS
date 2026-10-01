#include "lib/syscall.h"
#include <stdint.h>

static void write_text(const char *text) {
    uint64_t length = 0; while (text[length]) length++;
    sys_write(text, length);
}

__attribute__((noreturn)) void _start(void) {
    write_text("ConeOS init: userspace started\n");
    for (;;) {
        int64_t pid = sys_spawn("/bin/shell", 0);
        if (pid < 0) { write_text("init: cannot start shell\n"); sys_sleep(1000); continue; }
        int status = 0;
        sys_waitpid((uint64_t)pid, &status);
        write_text("init: shell exited, restarting\n");
    }
}
