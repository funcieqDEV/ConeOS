#pragma once

#include <stdint.h>

struct rtc_time {
    uint16_t year;
    uint8_t month;
    uint8_t day;
    uint8_t hour;
    uint8_t minute;
    uint8_t second;
};

int rtc_read(struct rtc_time *time);
