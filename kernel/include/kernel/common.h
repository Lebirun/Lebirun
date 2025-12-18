#ifndef COMMON_H
#define COMMON_H

#include <stdio.h>
#include <stdint.h>
#include <stdbool.h>
#include <kernel/io.h>

typedef uint32_t pid_t;

static inline void print_hex(unsigned long v) {
    char buf[9];
    buf[8] = '\0';
    for (int i = 0; i < 8; ++i) {
        unsigned int nib = (v >> ((7 - i) * 4)) & 0xF;
        buf[i] = (nib < 10) ? ('0' + nib) : ('A' + (nib - 10));
    }
    printf("%s", buf);
}

static inline uint32_t read_cr3(void) {
    uint32_t val;
    __asm__ volatile ("mov %%cr3, %0" : "=r"(val));
    return val;
}

static inline uint32_t read_cr0(void) {
    uint32_t val;
    __asm__ volatile ("mov %%cr0, %0" : "=r"(val));
    return val;
}

static inline __attribute__((unused)) void serial_puts(const char *str) {
    while (*str) {
        outb(0x3F8, *str++);
    }
}

static inline __attribute__((unused)) void serial_putchar(char c) {
    outb(0x3F8, (uint8_t)c);
}

#endif