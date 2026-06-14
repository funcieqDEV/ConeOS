#include "serial.h"
#include "pic.h"

#define COM1 0x3F8

void serial_init(void) {
  outb(COM1 + 1, 0x00); // disable interrupts
  outb(COM1 + 3, 0x80); // enable DLAB
  outb(COM1 + 0, 0x03); // divisor lo: 38400 baud
  outb(COM1 + 1, 0x00); // divisor hi
  outb(COM1 + 3, 0x03); // 8 bits, no parity, 1 stop bit
  outb(COM1 + 2, 0xC7); // enable FIFO
  outb(COM1 + 4, 0x0B); // RTS/DSR set
}

static inline int serial_is_transmit_empty(void) {
    return inb(0x3F8 + 5) & 0x20;
}

static void serial_wait(void) {
  while (!(inb(COM1 + 5) & 0x20))
    ;
}

void print_serial(const char *str) {
  while (*str) {
    serial_wait();
    outb(COM1, *str++);
  }
}

void print_uint(uint64_t v) {
  char buf[32];
  int i = 30;
  buf[31] = 0;

  if (v == 0) {
    print_serial("0");
    return;
  }

  while (v && i) {
    buf[i--] = '0' + (v % 10);
    v /= 10;
  }

  print_serial(&buf[i + 1]);
}

void print_hex(uint8_t value) {
    const char hex[] = "0123456789ABCDEF";

    char buf[3];
    buf[0] = hex[(value >> 4) & 0xF];
    buf[1] = hex[value & 0xF];
    buf[2] = '\0';

    print_serial(buf);
}

void print_serial_char(char c) {
    while (!serial_is_transmit_empty())
        ;

    outb(0x3F8, (uint8_t)c);
}
