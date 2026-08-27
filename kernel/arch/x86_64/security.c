#include <lebirun/security.h>
#include <lebirun/common.h>
#include <stdint.h>

void KERNEL_INIT x86_security_enable(void) {
    uint32_t eax;
    uint32_t ebx;
    uint32_t ecx;
    uint32_t edx;
    uint32_t max_basic;
    uint32_t pat_low;
    uint32_t pat_high;
    uint64_t pat;
    uint64_t disabled_cr0;
    uint64_t cr0;
    uint64_t cr4;
    uint64_t new_cr4;

    __asm__ volatile ("mov %%cr0, %0" : "=r"(cr0));
    cr0 |= 1ULL << 16;
    __asm__ volatile ("mov %0, %%cr0" : : "r"(cr0) : "memory");
    eax = 0;
    ecx = 0;
    __asm__ volatile ("cpuid"
                      : "+a"(eax), "=b"(ebx), "+c"(ecx), "=d"(edx));
    max_basic = eax;
    if (max_basic >= 1) {
        eax = 1;
        ecx = 0;
        __asm__ volatile ("cpuid"
                          : "+a"(eax), "=b"(ebx), "+c"(ecx), "=d"(edx));
        if (edx & (1U << 16)) {
            ecx = 0x277;
            __asm__ volatile ("rdmsr" : "=a"(pat_low), "=d"(pat_high)
                              : "c"(ecx));
            pat = ((uint64_t)pat_high << 32) | pat_low;
            if (((pat >> 8) & 0xFF) != 1) {
                disabled_cr0 = (cr0 | (1ULL << 30)) & ~(1ULL << 29);
                __asm__ volatile ("mov %0, %%cr0" : : "r"(disabled_cr0)
                                  : "memory");
                __asm__ volatile ("wbinvd" ::: "memory");
                pat = (pat & ~(0xFFULL << 8)) | (1ULL << 8);
                pat_low = (uint32_t)pat;
                pat_high = (uint32_t)(pat >> 32);
                ecx = 0x277;
                __asm__ volatile ("wrmsr" : : "a"(pat_low), "d"(pat_high),
                                  "c"(ecx) : "memory");
                __asm__ volatile ("wbinvd" ::: "memory");
                __asm__ volatile ("mov %0, %%cr0" : : "r"(cr0) : "memory");
            }
        }
    }
    if (max_basic < 7) return;
    eax = 7;
    ecx = 0;
    __asm__ volatile ("cpuid"
                      : "+a"(eax), "=b"(ebx), "+c"(ecx), "=d"(edx));
    __asm__ volatile ("mov %%cr4, %0" : "=r"(cr4));
    new_cr4 = cr4;
    if (ebx & (1U << 7)) new_cr4 |= 1ULL << 20;
    if (ecx & (1U << 2)) new_cr4 |= 1ULL << 11;
    if (new_cr4 != cr4)
        __asm__ volatile ("mov %0, %%cr4" : : "r"(new_cr4) : "memory");
}
