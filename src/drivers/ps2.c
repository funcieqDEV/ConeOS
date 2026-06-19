#include "ps2.h"
#include "../gfx/console.h"
#include "pic.h"
#include "serial.h"
#include <stddef.h>

static int ps2_wait_write(void) {
    for (size_t i = 0; i < 100000; i++) {
        if (!(inb(0x64) & 0x02))
            return 1;
    }

    return 0;
}

static int ps2_wait_read(void) {
    for (size_t i = 0; i < 100000; i++) {
        if (inb(0x64) & 0x01)
            return 1;
    }

    return 0;
}

static void ps2_flush_output(void) {
    while (inb(0x64) & 0x01)
        (void)inb(0x60);
}

static void ps2_write_command(uint8_t cmd) {
    if (!ps2_wait_write())
        return;
    outb(0x64, cmd);
}

static void ps2_write_data(uint8_t data) {
    if (!ps2_wait_write())
        return;
    outb(0x60, data);
}

void ps2_init(void) {
    ps2_flush_output();

    ps2_write_command(0xAD);
    ps2_write_command(0x20);
    uint8_t status = 0;
    if (ps2_wait_read())
        status = inb(0x60);
    status |= 0x01;
    ps2_write_command(0x60);
    ps2_write_data(status);
    ps2_write_command(0xAE);

    ps2_write_data(0xF4);
    if (ps2_wait_read())
        (void)inb(0x60);
}

static const char keymap[128] = {
    [0x02] = '1',  [0x03] = '2', [0x04] = '3',  [0x05] = '4',  [0x06] = '5',
    [0x07] = '6',  [0x08] = '7', [0x09] = '8',  [0x0A] = '9',  [0x0B] = '0',
    [0x0C] = '-',  [0x0D] = '=', [0x0E] = '\b', [0x0F] = '\t',

    [0x10] = 'q',  [0x11] = 'w', [0x12] = 'e',  [0x13] = 'r',  [0x14] = 't',
    [0x15] = 'y',  [0x16] = 'u', [0x17] = 'i',  [0x18] = 'o',  [0x19] = 'p',
    [0x1A] = '[',  [0x1B] = ']', [0x1C] = '\r',

    [0x1E] = 'a',  [0x1F] = 's', [0x20] = 'd',  [0x21] = 'f',  [0x22] = 'g',
    [0x23] = 'h',  [0x24] = 'j', [0x25] = 'k',  [0x26] = 'l',  [0x27] = ';',
    [0x28] = '\'', [0x29] = '`',

    [0x2B] = '\\', [0x2C] = 'z', [0x2D] = 'x',  [0x2E] = 'c',  [0x2F] = 'v',
    [0x30] = 'b',  [0x31] = 'n', [0x32] = 'm',  [0x33] = ',',  [0x34] = '.',
    [0x35] = '/',

    [0x39] = ' ',
};

void keyboard_handler(struct interrupt_frame *f) {
    (void)f;

    uint8_t sc = inb(0x60);

    if (sc & 0x80)
        return;

    char c = keymap[sc];

    if (c == '\b') {
        print_serial("\b \b");
        console_write("\b \b");
    } else if (c) {
        print_serial_char(c);
        console_putchar(c);
    }
}
