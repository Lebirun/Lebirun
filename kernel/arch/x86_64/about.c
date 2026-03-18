#include <kernel/about.h>
#include <string.h>

const char *kernel_get_version(void) {
    return OS_VERSION;
}

const char *kernel_get_name(void) {
    return OS_NAME;
}

const char *kernel_get_build_date(void) {
    return KERNEL_BUILD_DATE;
}

const char *kernel_get_build_time(void) {
    return KERNEL_BUILD_TIME;
}