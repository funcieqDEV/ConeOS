#pragma once

#include "../../limine/limine.h"

void console_init(struct limine_framebuffer *fb);

void console_putchar(char c);
void console_write(const char *str);

void console_set_color(unsigned int color);
void console_clear(void);
