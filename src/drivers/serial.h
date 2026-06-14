#ifndef SERIAL_H
#define SERIAL_H

#include <stdint.h>

void serial_init(void);
void print_serial(const char *str);
void print_uint(uint64_t v);
void print_hex(uint8_t value);
void print_serial_char(char c);
#endif
