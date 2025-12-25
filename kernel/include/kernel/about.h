#ifndef ABOUT_H
#define ABOUT_H

#include <stdint.h>

#define SYSNAME "Lebirun"
#define NODENAME "lebirun"
#define RELEASE "0.1.0"
#define VERSION "0.1.0"
#define MACHINE "i386"

struct utsname {
    char sysname[65];
    char nodename[65];
    char release[65];
    char version[65];
    char machine[65];
};

#endif