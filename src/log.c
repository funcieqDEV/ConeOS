#include "log.h"
#include "drivers/serial.h"
#include "gfx/console.h"

static log_level_t current_level = LOG_DEBUG;
static int console_ready = 0;

void log_console_enable(void) { console_ready = 1; }

static const char *level_str[] = {
    "[DEBUG] ",
    "[INFO]  ",
    "[WARN]  ",
    "[ERROR] ",
};

/* Colors matching level: gray, white, yellow, red */
static const unsigned int level_color[] = {
    0x888888,
    0xFFFFFF,
    0xFFFF00,
    0xFF4444,
};

void log_set_level(log_level_t min_level) { current_level = min_level; }

void log_msg(log_level_t level, const char *msg) {
    if (level < current_level)
        return;

    print_serial(level_str[level]);
    print_serial(msg);
    print_serial("\n");

    if (!console_ready)
        return;

    console_set_color(level_color[level]);
    console_write(level_str[level]);
    console_set_color(0xFFFFFF);
    console_write(msg);
    console_write("\n");
}
