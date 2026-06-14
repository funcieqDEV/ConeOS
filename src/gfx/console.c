#include "console.h"
#include "../utils.h"
#include "../gfx/draw.h"

static struct limine_framebuffer *g_fb = NULL;
static int cursor_x = 0;
static int cursor_y = 0;
static unsigned int text_color = 0x00FFFFFF;
#define FONT_W 8
#define FONT_H 16
#define FONT_SCALE 2 
unsigned int columns = 0;
unsigned int rows = 0;


void console_init(struct limine_framebuffer *fb){
    g_fb = fb;
    cursor_x = 0;
    cursor_y = 0;

    columns =  fb->width / (FONT_W * FONT_SCALE);
    rows = fb->height / (FONT_H * FONT_SCALE);
}

void console_set_color(unsigned int color){
    text_color = color;
}

void console_putchar(char c) {
    if(!g_fb)
        return;

    if(c == '\n'){
        cursor_x = 0;
        cursor_y++;
        return;
    }

    draw_char(g_fb, cursor_x * FONT_W * FONT_SCALE, cursor_y * FONT_H * FONT_SCALE, c, text_color, FONT_SCALE);
    cursor_x++;

    if(cursor_x >= columns){
        cursor_x = 0;
        cursor_y++;
    }
}

void console_write(const char *str) {
    while(*str)
        console_putchar(*str++);
}
