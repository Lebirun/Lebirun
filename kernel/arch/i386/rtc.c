#include <stdint.h>
#include <kernel/io.h>
#include <kernel/rtc.h>

#define CMOS_ADDRESS 0x70
#define CMOS_DATA    0x71

#define RTC_SECONDS  0x00
#define RTC_MINUTES  0x02
#define RTC_HOURS    0x04
#define RTC_DAY      0x07
#define RTC_MONTH    0x08
#define RTC_YEAR     0x09
#define RTC_CENTURY  0x32
#define RTC_STATUS_A 0x0A
#define RTC_STATUS_B 0x0B

static uint8_t bcd_to_binary(uint8_t bcd) {
    return ((bcd >> 4) * 10) + (bcd & 0x0F);
}

static uint8_t read_cmos(uint8_t reg) {
    outb(CMOS_ADDRESS, reg);
    return inb(CMOS_DATA);
}

static int is_updating(void) {
    outb(CMOS_ADDRESS, RTC_STATUS_A);
    return (inb(CMOS_DATA) & 0x80) != 0;
}

static void read_rtc(uint8_t *second, uint8_t *minute, uint8_t *hour,
                     uint8_t *day, uint8_t *month, uint16_t *year) {
    uint8_t century;
    uint8_t status_b;

    while (is_updating());

    *second = read_cmos(RTC_SECONDS);
    *minute = read_cmos(RTC_MINUTES);
    *hour = read_cmos(RTC_HOURS);
    *day = read_cmos(RTC_DAY);
    *month = read_cmos(RTC_MONTH);
    *year = read_cmos(RTC_YEAR);
    century = read_cmos(RTC_CENTURY);

    status_b = read_cmos(RTC_STATUS_B);

    if (!(status_b & 0x04)) {
        *second = bcd_to_binary(*second);
        *minute = bcd_to_binary(*minute);
        *hour = bcd_to_binary(*hour);
        *day = bcd_to_binary(*day);
        *month = bcd_to_binary(*month);
        *year = bcd_to_binary(*year);
        century = bcd_to_binary(century);
    }

    if (!(status_b & 0x02) && (*hour & 0x80)) {
        *hour = ((*hour & 0x7F) + 12) % 24;
    }

    *year = *year + (century * 100);
}

static uint32_t days_since_epoch(uint16_t year, uint8_t month, uint8_t day) {
    uint32_t days;
    uint16_t y;
    uint8_t m;
    const uint8_t days_in_month[] = {31, 28, 31, 30, 31, 30, 31, 31, 30, 31, 30, 31};

    days = 0;

    for (y = 1970; y < year; y++) {
        if ((y % 4 == 0 && y % 100 != 0) || (y % 400 == 0)) {
            days += 366;
        } else {
            days += 365;
        }
    }

    for (m = 1; m < month; m++) {
        days += days_in_month[m - 1];
        if (m == 2 && ((year % 4 == 0 && year % 100 != 0) || (year % 400 == 0))) {
            days += 1;
        }
    }

    days += day - 1;

    return days;
}

uint32_t rtc_get_time(void) {
    uint8_t second, minute, hour, day, month;
    uint16_t year;
    uint32_t days;
    uint32_t timestamp;

    read_rtc(&second, &minute, &hour, &day, &month, &year);

    days = days_since_epoch(year, month, day);
    timestamp = days * 86400 + hour * 3600 + minute * 60 + second;

    return timestamp;
}

void rtc_init(void) {
    uint32_t time;
    
    time = rtc_get_time();
    (void)time;
}
