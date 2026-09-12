#ifndef COMMON_H
#define COMMON_H

#include <stdio.h>
#include <stdint.h>
#include <stdbool.h>
#include <lebirun/io.h>

#define KERNEL_EARLY_INIT __attribute__((section(".init.early.text")))
#define KERNEL_INIT __attribute__((section(".init.text")))
#define KERNEL_INIT_RODATA __attribute__((section(".init.rodata")))
#define KERNEL_INIT_BSS __attribute__((section(".init.bss")))
#define KERNEL_INIT_OPTIONAL_BSS \
    __attribute__((section(".init.optional.bss")))
#define KERNEL_INIT_STRING(value) \
    ({ static const char kernel_init_string[] KERNEL_INIT_RODATA = value; kernel_init_string; })
#define KERNEL_INIT_LOG(format, ...) \
    printf(KERNEL_INIT_STRING(format), ##__VA_ARGS__)

#ifndef __pid_t_defined
typedef int32_t pid_t;
#define __pid_t_defined
#endif

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

#endif
