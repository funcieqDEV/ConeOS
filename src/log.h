#pragma once

typedef enum {
    LOG_DEBUG = 0,
    LOG_INFO,
    LOG_WARN,
    LOG_ERROR,
} log_level_t;

void log_set_level(log_level_t min_level);
void log_console_enable(void);
void log_msg(log_level_t level, const char *msg);

#define LOG_DEBUG(msg) log_msg(LOG_DEBUG, msg)
#define LOG_INFO(msg) log_msg(LOG_INFO, msg)
#define LOG_WARN(msg) log_msg(LOG_WARN, msg)
#define LOG_ERROR(msg) log_msg(LOG_ERROR, msg)
