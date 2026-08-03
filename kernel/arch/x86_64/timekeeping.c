#include <lebirun/timekeeping.h>
#include <lebirun/pit.h>
#include <lebirun/rtc.h>
#include <lebirun/spinlock.h>

static spinlock_t timekeeping_lock;
static int64_t realtime_offset_ns;
static int realtime_initialized;

uint64_t timekeeping_monotonic_ns(void) {
    uint64_t microseconds;

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
