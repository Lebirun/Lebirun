#include <lebirun/security.h>
#include <stdint.h>

void x86_security_enable(void) {
    uint32_t eax;
    uint32_t ebx;
    uint32_t ecx;
    uint32_t edx;
    uint32_t max_basic;
    uint64_t cr0;
    uint64_t cr4;

    __asm__ volatile ("mov %%cr0, %0" : "=r"(cr0));
    cr0 |= 1ULL << 16;
    __asm__ volatile ("mov %0, %%cr0" : : "r"(cr0) : "memory");
    eax = 0;
    ecx = 0;
    __asm__ volatile ("cpuid"
                      : "+a"(eax), "=b"(ebx), "+c"(ecx), "=d"(edx));
    max_basic = eax;
    if (max_basic < 7) return;
    eax = 7;
    ecx = 0;
    __asm__ volatile ("cpuid"
                      : "+a"(eax), "=b"(ebx), "+c"(ecx), "=d"(edx));
    if (!(ebx & (1U << 7))) return;
    __asm__ volatile ("mov %%cr4, %0" : "=r"(cr4));
    cr4 |= 1ULL << 20;
    __asm__ volatile ("mov %0, %%cr4" : : "r"(cr4) : "memory");
}
