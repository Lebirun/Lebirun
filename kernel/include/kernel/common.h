#ifndef COMMON_H
#define COMMON_H

#include <stdio.h>

typedef unsigned char bool;
#define true  1
#define false 0

static inline void print_hex(unsigned long v) {
    char buf[9];
    buf[8] = '\0';
    for (int i = 0; i < 8; ++i) {
        unsigned int nib = (v >> ((7 - i) * 4)) & 0xF;
        buf[i] = (nib < 10) ? ('0' + nib) : ('A' + (nib - 10));
    }
    printf("%s", buf);
}

#endif