#include "console.h"
#include "../utils.h"
#include <flanterm.h>
#include <flanterm_backends/fb.h>
#include <stddef.h>
#include <stdint.h>

static struct flanterm_context *g_term = NULL;
unsigned int columns = 0;
unsigned int rows = 0;

void console_init(struct limine_framebuffer *fb) {
  uint32_t default_bg = 0x000000;
  uint32_t default_fg = 0xffffff;

  g_term = flanterm_fb_init(NULL, NULL, fb->address, fb->width, fb->height,
                            fb->pitch, fb->red_mask_size, fb->red_mask_shift,
                            fb->green_mask_size, fb->green_mask_shift,
                            fb->blue_mask_size, fb->blue_mask_shift, NULL, NULL,
                            NULL, &default_bg, &default_fg, NULL, NULL, NULL, 0,
                            0, 0, 1, 1, 0, FLANTERM_FB_ROTATE_0);

  if (g_term) {
    size_t term_columns;
    size_t term_rows;

    flanterm_clear(g_term, true);
    flanterm_get_dimensions(g_term, &term_columns, &term_rows);
    columns = (unsigned int)term_columns;
    rows = (unsigned int)term_rows;
  }
}

void console_set_color(unsigned int color) {
  if (!g_term)
    return;

  if (color == 0xFF4444) {
    flanterm_set_text_fg(g_term, 1, true);
  } else if (color == 0xFFFF00) {
    flanterm_set_text_fg(g_term, 3, true);
  } else if (color == 0x888888) {
    flanterm_set_text_fg(g_term, 7, false);
  } else {
    flanterm_set_text_fg(g_term, 7, true);
  }
}

void console_putchar(char c) {
  if (!g_term)
    return;

  if (c == '\r') {
    flanterm_write(g_term, "\r\n", 2);
    return;
  }

  if (c == '\n') {
    flanterm_write(g_term, "\r\n", 2);
    return;
  }

  flanterm_write(g_term, &c, 1);
}

void console_write(const char *str) {
  if (!g_term)
    return;

  while (*str) {
    if (*str == '\n') {
      flanterm_write(g_term, "\r\n", 2);
    } else {
      flanterm_write(g_term, str, 1);
    }
    str++;
  }
}

void console_clear(void) {
  if (!g_term)
    return;

  flanterm_clear(g_term, true);
}
