#include <lebirun/timekeeping.h>
#include <lebirun/pit.h>
#include <lebirun/rtc.h>
#include <lebirun/spinlock.h>
#include <lebirun/common.h>

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
static int tsc_ready;
static inline uint64_t tsc_rdtsc(void)
{
    uint32_t lo;
    uint32_t hi;
    __asm__ volatile("rdtsc" : "=a"(lo), "=d"(hi));
    return ((uint64_t)hi << 32) | lo;
}
static int tsc_has_invariant(void)
{
    uint32_t eax;
    uint32_t ebx;
    uint32_t ecx;
    uint32_t edx;
    eax = 0x80000000u;
    __asm__ volatile("cpuid" : "+a"(eax), "=b"(ebx), "=c"(ecx), "=d"(edx));
    if (eax < 0x80000007u)
        return 0;
    eax = 0x80000007u;
    __asm__ volatile("cpuid" : "+a"(eax), "=b"(ebx), "=c"(ecx), "=d"(edx));
    return (edx & (1u << 8)) != 0;
}
void tsc_init(void)
{
    uint64_t t0;
    uint64_t t1;
    uint64_t tick0;
    uint64_t spins;
    if (tsc_ready)
        return;
    if (!tsc_has_invariant())
        return;
    tick0 = pit_get_ticks();
    t0 = tsc_rdtsc();
    spins = 0;
    do {
        if (++spins > 100000000ULL)
            return;
        cpu_relax();
    } while (pit_get_ticks() - tick0 < 25);
    t1 = tsc_rdtsc();
    if (t1 <= t0)
        return;
    tsc_freq_hz = (t1 - t0) * pit_get_frequency() / 25;
    if (tsc_freq_hz < 1000000ULL)
        return;
    tsc_base_tsc = t1;
    tsc_base_us = pit_get_uptime_us();
    tsc_ready = 1;
    printf("TSC: %llu Hz\n", (unsigned long long)tsc_freq_hz);
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
    if (!tsc_ready || !tsc_freq_hz)
        return 0;
    now = tsc_rdtsc();
    delta = now >= tsc_base_tsc ? now - tsc_base_tsc : 0;
    return tsc_base_us * 1000ULL + delta * 1000000000ULL / tsc_freq_hz;
}
