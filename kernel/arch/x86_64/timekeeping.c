#include <lebirun/timekeeping.h>
#include <lebirun/pit.h>
#include <lebirun/rtc.h>
#include <lebirun/spinlock.h>
#include <lebirun/common.h>
#include <lebirun/mem_map.h>
#include <lebirun/io.h>

static spinlock_t timekeeping_lock;
static int64_t realtime_offset_ns;
static int realtime_initialized;

uint64_t timekeeping_monotonic_ns(void) {
    uint64_t tsc_ns;
    uint64_t microseconds;

    if (tsc_available()) {
        tsc_ns = tsc_get_ns();
        if (tsc_ns != 0)
            return tsc_ns;
    }
    microseconds = pit_get_uptime_us();
    if (microseconds > UINT64_MAX / 1000) return UINT64_MAX;
    return microseconds * 1000;
}

static void timekeeping_initialize_realtime(void) {
    uint64_t monotonic;
    uint64_t realtime;

    if (realtime_initialized) return;
    spin_lock(&timekeeping_lock);
    if (!realtime_initialized) {
        monotonic = timekeeping_monotonic_ns();
        realtime = rtc_get_time();
        if (realtime > UINT64_MAX / 1000000000ULL)
            realtime = UINT64_MAX;
        else
            realtime *= 1000000000ULL;
        if (realtime >= monotonic) {
            if (realtime - monotonic > INT64_MAX)
                realtime_offset_ns = INT64_MAX;
            else
                realtime_offset_ns = (int64_t)(realtime - monotonic);
        } else if (monotonic - realtime > (uint64_t)INT64_MAX) {
            realtime_offset_ns = INT64_MIN;
        } else {
            realtime_offset_ns = -(int64_t)(monotonic - realtime);
        }
        __atomic_store_n(&realtime_initialized, 1, __ATOMIC_RELEASE);
    }
    spin_unlock(&timekeeping_lock);
}

uint64_t timekeeping_realtime_ns(void) {
    uint64_t monotonic;
    int64_t offset;

    timekeeping_initialize_realtime();
    monotonic = timekeeping_monotonic_ns();
    offset = __atomic_load_n(&realtime_offset_ns, __ATOMIC_ACQUIRE);
    if (offset >= 0) {
        if (monotonic > UINT64_MAX - (uint64_t)offset) return UINT64_MAX;
        return monotonic + (uint64_t)offset;
    }
    if ((uint64_t)(-(offset + 1)) + 1 > monotonic) return 0;
    return monotonic - ((uint64_t)(-(offset + 1)) + 1);
}

int timekeeping_get_ns(int clock_id, uint64_t *value) {
    if (!value) return -1;
    if (clock_id == TIMEKEEPING_CLOCK_REALTIME) {
        *value = timekeeping_realtime_ns();
        return 0;
    }
    if (clock_id == TIMEKEEPING_CLOCK_MONOTONIC ||
        clock_id == TIMEKEEPING_CLOCK_MONOTONIC_RAW ||
        clock_id == TIMEKEEPING_CLOCK_BOOTTIME) {
        *value = timekeeping_monotonic_ns();
        return 0;
    }
    return -1;
}

int timekeeping_set_realtime_ns(uint64_t value) {
    uint64_t monotonic;
    int64_t offset;

    monotonic = timekeeping_monotonic_ns();
    if (value >= monotonic) {
        if (value - monotonic > INT64_MAX) return -1;
        offset = (int64_t)(value - monotonic);
    } else {
        if (monotonic - value > (uint64_t)INT64_MAX) return -1;
        offset = -(int64_t)(monotonic - value);
    }
    spin_lock(&timekeeping_lock);
    realtime_offset_ns = offset;
    __atomic_store_n(&realtime_initialized, 1, __ATOMIC_RELEASE);
    spin_unlock(&timekeeping_lock);
    return 0;
}
static uint64_t tsc_freq_hz;
static uint64_t tsc_base_tsc;
static uint64_t tsc_base_us;
static uint64_t tsc_last_ns;
static int tsc_ready;
static inline uint64_t tsc_rdtsc(void)
{
    uint32_t lo;
    uint32_t hi;
    __asm__ volatile("lfence" ::: "memory");
    __asm__ volatile("rdtsc" : "=a"(lo), "=d"(hi));
    return ((uint64_t)hi << 32) | lo;
}
static uint64_t tsc_kvm_frequency(void) {
    static struct {
        uint32_t version;
        uint32_t padding;
        uint64_t timestamp;
        uint64_t system_time;
        uint32_t multiplier;
        int8_t shift;
        uint8_t flags;
        uint8_t reserved[2];
    } clock __attribute__((aligned(32)));
    uint32_t eax;
    uint32_t ebx;
    uint32_t ecx;
    uint32_t edx;
    uint32_t version;
    uint32_t multiplier;
    uint32_t old_low;
    uint32_t old_high;
    int shift;
    uint64_t address;
    uint64_t frequency;

    eax = 1;
    __asm__ volatile("cpuid" : "+a"(eax), "=b"(ebx), "=c"(ecx), "=d"(edx));
    if (!(ecx & (1u << 31))) return 0;
    eax = 0x40000000u;
    __asm__ volatile("cpuid" : "+a"(eax), "=b"(ebx), "=c"(ecx), "=d"(edx));
    if (ebx != 0x4b4d564bu || ecx != 0x564b4d56u || edx != 0x4du)
        return 0;
    eax = 0x40000001u;
    __asm__ volatile("cpuid" : "+a"(eax), "=b"(ebx), "=c"(ecx), "=d"(edx));
    if (!(eax & (1u << 3))) return 0;
    address = vmm_get_phys_in_pml4(vmm_get_kernel_cr3(),
                                   (uint64_t)(uintptr_t)&clock & ~(PAGE_SIZE - 1));
    if (!address) return 0;
    address += (uintptr_t)&clock & (PAGE_SIZE - 1);
    __asm__ volatile("rdmsr" : "=a"(old_low), "=d"(old_high) : "c"(0x4b564d01u));
    __asm__ volatile("wrmsr" : : "c"(0x4b564d01u),
                      "a"((uint32_t)(address | 1)), "d"((uint32_t)(address >> 32))
                      : "memory");
    do {
        version = __atomic_load_n(&clock.version, __ATOMIC_ACQUIRE);
        multiplier = __atomic_load_n(&clock.multiplier, __ATOMIC_RELAXED);
        shift = __atomic_load_n(&clock.shift, __ATOMIC_RELAXED);
        __atomic_thread_fence(__ATOMIC_ACQUIRE);
    } while ((version & 1) || version !=
             __atomic_load_n(&clock.version, __ATOMIC_ACQUIRE));
    __asm__ volatile("wrmsr" : : "c"(0x4b564d01u), "a"(old_low), "d"(old_high)
                      : "memory");
    if (!version || !multiplier || shift <= -64 || shift >= 64) return 0;
    frequency = (1000000000ULL << 32) / multiplier;
    if (shift >= 0) frequency >>= shift;
    else {
        if (frequency > (UINT64_MAX >> -shift)) return 0;
        frequency <<= -shift;
    }
    return frequency;
}

void tsc_early_init(void)
{
    uint64_t samples[5];
    uint64_t w;
    uint64_t v;
    uint64_t flags;
    uint8_t speaker;
    uint32_t polls;
    uint16_t latch = 1193182 / 100;
    uint64_t tw0;
    uint64_t tw1;
    uint64_t temporary;
    uint32_t maxleaf = 0;
    uint32_t leaf = 0;
    uint32_t reserved = 0;
    uint32_t numer = 0;
    uint32_t crystal = 0;
    if (tsc_ready)
        return;
    tsc_freq_hz = tsc_kvm_frequency();
    if (!tsc_freq_hz) {
        __asm__ volatile("cpuid"
                         : "=a"(maxleaf) : "a"(leaf) : "ebx", "ecx", "edx");
        if (maxleaf >= 0x15) {
            leaf = 0x15;
            __asm__ volatile("cpuid"
                             : "+a"(leaf), "=b"(numer), "=c"(crystal),
                               "=d"(reserved));
            if (leaf && numer && crystal)
                tsc_freq_hz = (uint64_t)numer * crystal / leaf;
        }
    }
    if (tsc_freq_hz) {
        tsc_base_tsc = tsc_rdtsc();
        tsc_base_us = pit_get_uptime_us();
        tsc_ready = 1;
        printf("TSC: %llu Hz\n", (unsigned long long)tsc_freq_hz);
        return;
    }
    __asm__ volatile("pushfq; popq %0; cli" : "=r"(flags) : : "memory");
    speaker = inb(0x61);
    for (w = 0; w < 5; w++) {
        outb(0x61, speaker & ~0x03);
        outb(0x43, 0xb0);
        outb(0x42, latch & 0xff);
        outb(0x42, latch >> 8);
        if (inb(0x61) & 0x20)
            break;
        tw0 = tsc_rdtsc();
        outb(0x61, (speaker & ~0x02) | 0x01);
        for (polls = 0; polls < 1000000; polls++) {
            if (inb(0x61) & 0x20)
                break;
        }
        tw1 = tsc_rdtsc();
        if (polls == 1000000)
            break;
        samples[w] = tw1 > tw0 ? (tw1 - tw0) * 1193182 / latch : 0;
    }
    outb(0x61, speaker);
    __asm__ volatile("pushq %0; popfq" : : "r"(flags) : "memory", "cc");
    if (w != 5)
        return;
    for (w = 0; w < 4; w++) {
        for (v = w + 1; v < 5; v++) {
            if (samples[v] < samples[w]) {
                temporary = samples[w];
                samples[w] = samples[v];
                samples[v] = temporary;
            }
        }
    }
    if (!samples[2] || samples[2] < 1000000ULL)
        return;
    tsc_freq_hz = samples[2];
    tsc_base_tsc = tsc_rdtsc();
    tsc_base_us = pit_get_uptime_us();
    tsc_ready = 1;
    printf("TSC: %llu Hz\n", (unsigned long long)tsc_freq_hz);
}
void tsc_init(void)
{
    if (!tsc_ready) tsc_early_init();
}
uint64_t tsc_get_freq_hz(void)
{
    return tsc_freq_hz;
}
int tsc_available(void)
{
    return tsc_ready;
}
uint64_t tsc_get_ns(void)
{
    uint64_t now;
    uint64_t delta;
    uint64_t result;
    uint64_t last;
    if (!tsc_ready || !tsc_freq_hz)
        return 0;
    now = tsc_rdtsc();
    delta = now >= tsc_base_tsc ? now - tsc_base_tsc : 0;
    result = tsc_base_us * 1000ULL + delta / tsc_freq_hz * 1000000000ULL +
             (delta % tsc_freq_hz) * 1000000000ULL / tsc_freq_hz;
    last = __atomic_load_n(&tsc_last_ns, __ATOMIC_ACQUIRE);
    if (result <= last)
        return last;
    while (!__atomic_compare_exchange_n(&tsc_last_ns, &last, result, 0,
                                        __ATOMIC_RELEASE, __ATOMIC_ACQUIRE)) {
        if (result <= last)
            return last;
    }
    return result;
}
