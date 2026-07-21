#ifndef COMMON_H
#define COMMON_H

#include <stdio.h>
#include <stdint.h>
#include <stdbool.h>
#include <lebirun/io.h>

#ifndef __pid_t_defined
typedef int32_t pid_t;
#define __pid_t_defined
#endif

static inline void print_hex(unsigned long v) {
    char buf[17];
    int i;
    buf[16] = '\0';
    for (i = 0; i < 16; ++i) {
        unsigned int nib = (v >> ((15 - i) * 4)) & 0xF;
        buf[i] = (nib < 10) ? ('0' + nib) : ('A' + (nib - 10));
    }
    printf("%s", buf);
}

static inline uint64_t read_cr3(void) {
    uint64_t val;
    __asm__ volatile ("mov %%cr3, %0" : "=r"(val));
    return val;
}

static inline uint64_t read_cr0(void) {
    uint64_t val;
    __asm__ volatile ("mov %%cr0, %0" : "=r"(val));
    return val;
}

static inline __attribute__((unused)) void serial_puts(const char *str) {
    while (*str) {
        outb(0x3F8, *str++);
    }
}

static inline __attribute__((unused)) void serial_putchar(char c) {
    if (c == '\n')
        outb(0x3F8, '\r');
    outb(0x3F8, (uint8_t)c);
}

static inline __attribute__((unused)) void serial_puthex(uint64_t v) {
    int i;
    unsigned int nib;
    serial_puts("0x");
    for (i = 0; i < 16; i++) {
        nib = (v >> ((15 - i) * 4)) & 0xF;
        outb(0x3F8, (nib < 10) ? ('0' + nib) : ('A' + (nib - 10)));
    }
}

#endif
