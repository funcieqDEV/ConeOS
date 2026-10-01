#include "rtc.h"
#include "pic.h"
#include <stddef.h>

#define CMOS_ADDRESS 0x70
#define CMOS_DATA 0x71
#define RTC_SECONDS 0x00
#define RTC_MINUTES 0x02
#define RTC_HOURS 0x04
#define RTC_DAY 0x07
#define RTC_MONTH 0x08
#define RTC_YEAR 0x09
#define RTC_STATUS_A 0x0A
#define RTC_STATUS_B 0x0B
#define RTC_UPDATE_IN_PROGRESS 0x80
#define RTC_BINARY_MODE 0x04
#define RTC_24_HOUR_MODE 0x02

static uint8_t cmos_read(uint8_t reg) {
    outb(CMOS_ADDRESS, reg);
    return inb(CMOS_DATA);
}

static int wait_for_update(void) {
    for (size_t i = 0; i < 100000; i++) {
        if (!(cmos_read(RTC_STATUS_A) & RTC_UPDATE_IN_PROGRESS))
            return 1;
    }
    return 0;
}

static uint8_t from_bcd(uint8_t value) {
    return (value & 0x0F) + (value >> 4) * 10;
}

static int same_time(const struct rtc_time *left,
                     const struct rtc_time *right) {
    return left->year == right->year && left->month == right->month &&
           left->day == right->day && left->hour == right->hour &&
           left->minute == right->minute && left->second == right->second;
}

static int read_once(struct rtc_time *time, uint8_t *status_b) {
    if (!wait_for_update())
        return 0;

    time->second = cmos_read(RTC_SECONDS);
    time->minute = cmos_read(RTC_MINUTES);
    time->hour = cmos_read(RTC_HOURS);
    time->day = cmos_read(RTC_DAY);
    time->month = cmos_read(RTC_MONTH);
    time->year = cmos_read(RTC_YEAR);
    *status_b = cmos_read(RTC_STATUS_B);
    return 1;
}

int rtc_read(struct rtc_time *time) {
    if (time == NULL)
        return 0;

    struct rtc_time first;
    struct rtc_time second;
    uint8_t first_status;
    uint8_t status;

    if (!read_once(&first, &first_status) || !read_once(&second, &status))
        return 0;
    for (size_t attempts = 0; !same_time(&first, &second) && attempts < 3;
         attempts++) {
        first = second;
        first_status = status;
        if (!read_once(&second, &status))
            return 0;
    }
    if (!same_time(&first, &second) || first_status != status)
        return 0;

    uint8_t pm = second.hour & 0x80;
    second.hour &= 0x7F;
    if (!(status & RTC_BINARY_MODE)) {
        second.second = from_bcd(second.second);
        second.minute = from_bcd(second.minute);
        second.hour = from_bcd(second.hour);
        second.day = from_bcd(second.day);
        second.month = from_bcd(second.month);
        second.year = from_bcd((uint8_t)second.year);
    }
    if (!(status & RTC_24_HOUR_MODE)) {
        if (pm && second.hour < 12)
            second.hour += 12;
        else if (!pm && second.hour == 12)
            second.hour = 0;
    }

    second.year += 2000;
    if (second.month < 1 || second.month > 12 || second.day < 1 ||
        second.day > 31 || second.hour > 23 || second.minute > 59 ||
        second.second > 59)
        return 0;

    *time = second;
    return 1;
}
