#ifndef ABOUT_H
#define ABOUT_H

#include <stdint.h>

#define OS_NAME "Lebirun"
#define OS_VERSION "0.1.0"
#define KERNEL_BUILD_DATE __DATE__
#define KERNEL_BUILD_TIME __TIME__
#ifndef KERNEL_BUILD_TZ
#define KERNEL_BUILD_TZ "UTC"
#endif
#define KERNEL_BUILD_TIMEZONE KERNEL_BUILD_TZ
#define SYSNAME OS_NAME
#define RELEASE OS_VERSION
#define VERSION OS_VERSION
#define NODENAME "lebirun"
#define MACHINE "x86_64"

struct utsname {
    char sysname[65];
    char nodename[65];
    char release[65];
    char version[65];
    char machine[65];
};

const char *kernel_get_version(void);
const char *kernel_get_name(void);
const char *kernel_get_build_date(void);
const char *kernel_get_build_time(void);

#endif