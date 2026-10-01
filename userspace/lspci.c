#include "lib/syscall.h"
#include <stdint.h>

static void text(const char *s) { uint64_t n = 0; while (s[n]) n++; sys_write(s, n); }
static void hex(uint64_t value, int digits) {
    static const char table[] = "0123456789abcdef"; char buffer[8];
    for (int i = digits - 1; i >= 0; i--) { buffer[i] = table[value & 15]; value >>= 4; }
    sys_write(buffer, digits);
}

__attribute__((noreturn)) void _start(void) {
    struct user_pci_device device;
    for (uint64_t i = 0; sys_pci_get(i, &device) == 0; i++) {
        hex(device.bus, 2); text(":"); hex(device.slot, 2); text("."); hex(device.function, 1);
        text(" "); hex(device.vendor_id, 4); text(":"); hex(device.device_id, 4);
        text(" class "); hex(device.class_code, 2); text(":"); hex(device.subclass, 2); text("\n");
    }
    sys_exit(0);
}
