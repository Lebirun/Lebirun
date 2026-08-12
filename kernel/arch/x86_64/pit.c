#include <stdint.h>
#include <stdbool.h>
#include <lebirun/tty.h>
#include <lebirun/idt.h>
#include <lebirun/common.h>
#include <lebirun/io.h>
#include <lebirun/pit.h>
#include <lebirun/spinlock.h>
#include <lebirun/mem_map.h>

#define PIT_BASE_FREQ       1193182
#define PIT_CHANNEL0_DATA   0x40
#define PIT_CHANNEL1_DATA   0x41
#define PIT_CHANNEL2_DATA   0x42
#define PIT_COMMAND         0x43

#define PIT_CMD_CHANNEL0    0x00
#define PIT_CMD_CHANNEL2    0x80
#define PIT_CMD_LATCH       0x00
#define PIT_CMD_LOHI        0x30
#define PIT_CMD_MODE0       0x00
#define PIT_CMD_MODE2       0x04
#define PIT_CMD_MODE3       0x06
#define PIT_CMD_BINARY      0x00

#define PIT_CMD_READBACK    0xC2

#define PIT_CMD_INIT        (PIT_CMD_CHANNEL0 | PIT_CMD_LOHI | PIT_CMD_MODE3 | PIT_CMD_BINARY)
#define PIT_CMD_CH2_INIT    (PIT_CMD_CHANNEL2 | PIT_CMD_LOHI | PIT_CMD_MODE3 | PIT_CMD_BINARY)
#define PIT_CMD_ONESHOT     (PIT_CMD_CHANNEL0 | PIT_CMD_LOHI | PIT_CMD_MODE0 | PIT_CMD_BINARY)

#define SPEAKER_PORT        0x61
#define SPEAKER_ENABLE      0x03

extern volatile uint64_t tick_count;

uint64_t pit_freq = 100;

static uint64_t calibrated_freq = 100;
static uint64_t current_divisor = 0;
static uint64_t uptime_ticks = 0;
static uint64_t last_tick = 0;
static uint64_t us_per_tick = 10000;
static uint64_t last_uptime_us = 0;
static spinlock_t uptime_lock = { .locked = 0 };

typedef void (*pit_callback_t)(uint64_t ticks);

typedef struct pit_callback_entry {
    pit_callback_t callback;
    uint64_t interval;
    uint64_t remaining;
    bool active;
    bool oneshot;
    int handle;
    struct pit_callback_entry *next;
} pit_callback_entry_t;

static pit_callback_entry_t *timer_callbacks;
static int next_callback_handle = 1;

static inline void io_wait(void) {
    outb(0x80, 0);
}

static inline uint64_t save_flags_cli(void) {
    uint64_t flags;
    __asm__ volatile(
        "pushfq\n\t"
        "popq %0\n\t"
        "cli"
        : "=r"(flags)
        :
        : "memory"
    );
    return flags;
}

static inline void restore_flags(uint64_t flags) {
    __asm__ volatile(
        "pushq %0\n\t"
        "popfq"
        :
        : "r"(flags)
        : "memory", "cc"
    );
}

static void pit_set_divisor(uint16_t divisor) {
    uint64_t flags = save_flags_cli();
    current_divisor = divisor;
    outb(PIT_COMMAND, PIT_CMD_INIT);
    io_wait();
    outb(PIT_CHANNEL0_DATA, divisor & 0xFF);
    io_wait();
    outb(PIT_CHANNEL0_DATA, (divisor >> 8) & 0xFF);
    io_wait();
    restore_flags(flags);
}

static uint16_t pit_read_count(void) {
    uint64_t flags;
    uint16_t count;

    flags = save_flags_cli();
    outb(PIT_COMMAND, PIT_CMD_LATCH);
    io_wait();
    count = inb(PIT_CHANNEL0_DATA);
    count |= (uint16_t)inb(PIT_CHANNEL0_DATA) << 8;
    restore_flags(flags);
    return count;
}

static uint64_t pit_get_subtick_us(void) {
    uint16_t count;
    uint64_t elapsed;

    if (current_divisor == 0) return 0;
    count = pit_read_count();
    elapsed = current_divisor - count;
    return (elapsed * 1000000UL) / PIT_BASE_FREQ;
}

void KERNEL_INIT pit_init(uint64_t freq) {
    uint64_t divisor;
    uint64_t actual_freq;
    uint64_t error_ppm;

    timer_callbacks = NULL;
    next_callback_handle = 1;
    
    if (freq == 0) freq = 100;
    if (freq > PIT_BASE_FREQ) freq = PIT_BASE_FREQ;
    if (freq < 19) freq = 19;
    
    pit_freq = freq;
    calibrated_freq = freq;
    
    divisor = PIT_BASE_FREQ / freq;
    if (divisor > 65535) divisor = 65535;
    if (divisor < 1) divisor = 1;
    
    us_per_tick = 1000000 / freq;

    uptime_ticks = 0;
    last_tick = tick_count;
    
    pit_set_divisor((uint16_t)divisor);
    
    actual_freq = PIT_BASE_FREQ / divisor;
    if (actual_freq > freq) {
        error_ppm = ((actual_freq - freq) * 1000000) / freq;
    } else {
        error_ppm = ((freq - actual_freq) * 1000000) / freq;
    }
    
    KERNEL_INIT_LOG("PIT: %u Hz (req %u Hz, div %u, err %u ppm)\n",
           actual_freq, freq, divisor, error_ppm);
}

void pit_set_frequency(uint64_t freq) {
    uint64_t divisor;

    if (freq == 0) freq = 100;
    if (freq > PIT_BASE_FREQ) freq = PIT_BASE_FREQ;
    if (freq < 19) freq = 19;
    
    pit_freq = freq;
    calibrated_freq = freq;
    us_per_tick = 1000000 / freq;
    
    divisor = PIT_BASE_FREQ / freq;
    if (divisor > 65535) divisor = 65535;
    if (divisor < 1) divisor = 1;
    
    pit_set_divisor((uint16_t)divisor);
}

static void delay_ticks(uint64_t ticks) {
    volatile uint64_t start;

    if (ticks == 0) return;
    start = tick_count;
    while ((tick_count - start) < ticks) {
        __asm__ volatile("hlt");
    }
}

void delay_us(uint64_t us) {
    uint64_t loops;
    uint64_t start_tick;
    uint64_t start_subtick;
    uint64_t current_tick;
    uint64_t current_subtick;
    uint64_t elapsed_ticks;
    uint64_t elapsed_us;

    if (us == 0) return;
    
    if (us < 50) {
        loops = us * 10;
        while (loops--) {
            __asm__ volatile("nop");
        }
        return;
    }
    
    start_tick = tick_count;
    start_subtick = pit_get_subtick_us();
    
    while (1) {
        current_tick = tick_count;
        current_subtick = pit_get_subtick_us();
        
        elapsed_ticks = current_tick - start_tick;
        elapsed_us = elapsed_ticks * us_per_tick;
        
        if (current_subtick >= start_subtick) {
            elapsed_us += current_subtick - start_subtick;
        } else {
            elapsed_us += us_per_tick - start_subtick + current_subtick;
        }
        
        if (elapsed_us >= us) break;
        
        if ((us - elapsed_us) > us_per_tick) {
            __asm__ volatile("hlt");
        }
    }
}

void delay(uint64_t ms) {
    uint64_t ticks_needed;
    uint64_t remainder_us;

    if (ms == 0) return;
    
    ticks_needed = (ms * calibrated_freq) / 1000;
    remainder_us = ((ms * 1000) % (1000000 / calibrated_freq));
    
    if (ticks_needed > 0) {
        delay_ticks(ticks_needed);
    }
    
    if (remainder_us > 0) {
        delay_us(remainder_us);
    } else if (ticks_needed == 0) {
        delay_us(ms * 1000);
    }
}

void delay_inMs(uint64_t ms) {
    delay(ms);
}

void delay_inSecs(uint64_t secs) {
    uint64_t chunk;

    while (secs > 0) {
        chunk = (secs > 60) ? 60 : secs;
        delay(chunk * 1000);
        secs -= chunk;
    }
}

void delay_inMins(uint64_t mins) {
    while (mins > 0) {
        delay(60000);
        mins--;
    }
}

uint64_t pit_get_ticks(void) {
    return tick_count;
}

uint64_t pit_get_uptime_us(void) {
    extern volatile uint32_t *lapic_base;
    extern uint32_t lapic_timer_reload;
    uint64_t flags;
    uint64_t current;
    uint64_t delta;
    uint64_t ticks;
    uint32_t ccr;
    uint32_t reload;
    uint64_t base_us;
    uint64_t result;
    uint64_t elapsed;
    uint64_t subtick_us;

    flags = save_flags_cli();
    spin_lock(&uptime_lock);
    current = tick_count;
    delta = current - last_tick;
    uptime_ticks += delta;
    last_tick = current;
    ticks = uptime_ticks;
    ccr = 0;
    reload = lapic_timer_reload;
    if (lapic_base && reload > 0)
        ccr = lapic_base[0x390 / 4];
    base_us = (ticks * 1000000ULL) / calibrated_freq;
    if (lapic_base && reload > 0) {
        elapsed = (ccr < reload) ? (reload - ccr) : 0;
        subtick_us = (elapsed * (1000000ULL / calibrated_freq)) / reload;
        result = base_us + subtick_us;
    } else {
        spin_unlock(&uptime_lock);
        result = base_us + pit_get_subtick_us();
        flags = save_flags_cli();
        spin_lock(&uptime_lock);
    }
    if (result < last_uptime_us)
        result = last_uptime_us;
    else
        last_uptime_us = result;
    spin_unlock(&uptime_lock);
    restore_flags(flags);
    return result;
}

uint64_t pit_get_ticks64(void) {
    uint64_t flags;
    uint64_t current;
    uint64_t delta;
    uint64_t result;

    flags = save_flags_cli();
    spin_lock(&uptime_lock);
    current = tick_count;
    delta = current - last_tick;
    uptime_ticks += delta;
    last_tick = current;
    result = uptime_ticks;
    spin_unlock(&uptime_lock);
    restore_flags(flags);
    return result;
}

uint64_t pit_get_uptime_ms(void) {
    return pit_get_uptime_us() / 1000;
}

uint64_t pit_get_uptime_secs(void) {
    return pit_get_uptime_us() / 1000000;
}

uint64_t pit_get_frequency(void) {
    return calibrated_freq;
}

uint64_t pit_ticks_to_ms(uint64_t ticks) {
    return (ticks * 1000) / calibrated_freq;
}

uint64_t pit_ms_to_ticks(uint64_t ms) {
    return (ms * calibrated_freq) / 1000;
}

uint64_t pit_ticks_to_us(uint64_t ticks) {
    return (ticks * 1000000ULL) / calibrated_freq;
}

uint64_t pit_us_to_ticks(uint64_t us) {
    return (us * calibrated_freq) / 1000000;
}

int KERNEL_INIT pit_register_callback(pit_callback_t callback,
                                      uint64_t interval_ticks,
                                      bool oneshot) {
    uint64_t flags;
    pit_callback_entry_t *entry;
    int handle;

    if (!callback || interval_ticks == 0) return -1;
    entry = (pit_callback_entry_t *)kmalloc(sizeof(pit_callback_entry_t));
    if (!entry) return -1;

    flags = save_flags_cli();
    handle = next_callback_handle;
    if (next_callback_handle == 0x7FFFFFFF) next_callback_handle = 1;
    else next_callback_handle++;
    entry->callback = callback;
    entry->interval = interval_ticks;
    entry->remaining = interval_ticks;
    entry->active = true;
    entry->oneshot = oneshot;
    entry->handle = handle;
    entry->next = timer_callbacks;
    timer_callbacks = entry;
    restore_flags(flags);
    return handle;
}

void pit_unregister_callback(int handle) {
    uint64_t flags;
    pit_callback_entry_t *entry;

    if (handle <= 0) return;
    flags = save_flags_cli();
    entry = timer_callbacks;
    while (entry) {
        if (entry->handle == handle) {
            entry->active = false;
            entry->callback = NULL;
            break;
        }
        entry = entry->next;
    }
    restore_flags(flags);
}

void pit_process_callbacks(void) {
    pit_callback_entry_t *entry;
    pit_callback_entry_t *previous;
    pit_callback_entry_t *next;

    previous = NULL;
    entry = timer_callbacks;
    while (entry) {
        next = entry->next;
        if (entry->active) {
            entry->remaining--;
            if (entry->remaining == 0) {
                if (entry->callback) {
                    entry->callback(tick_count);
                }
                if (entry->oneshot) {
                    entry->active = false;
                } else {
                    entry->remaining = entry->interval;
                }
            }
        }
        if (!entry->active) {
            if (previous) previous->next = next;
            else timer_callbacks = next;
            kfree(entry);
        } else {
            previous = entry;
        }
        entry = next;
    }
}

void speaker_play(uint64_t freq) {
    uint64_t divisor;
    uint64_t flags;
    uint8_t tmp;

    if (freq == 0) {
        speaker_stop();
        return;
    }
    
    divisor = PIT_BASE_FREQ / freq;
    if (divisor > 65535) divisor = 65535;
    if (divisor < 1) divisor = 1;
    
    flags = save_flags_cli();
    
    outb(PIT_COMMAND, PIT_CMD_CH2_INIT);
    io_wait();
    outb(PIT_CHANNEL2_DATA, divisor & 0xFF);
    io_wait();
    outb(PIT_CHANNEL2_DATA, (divisor >> 8) & 0xFF);
    io_wait();
    
    tmp = inb(SPEAKER_PORT);
    outb(SPEAKER_PORT, tmp | SPEAKER_ENABLE);
    
    restore_flags(flags);
}

void speaker_stop(void) {
    uint8_t tmp;

    tmp = inb(SPEAKER_PORT);
    outb(SPEAKER_PORT, tmp & ~SPEAKER_ENABLE);
}

void speaker_beep(uint64_t freq, uint64_t duration_ms) {
    speaker_play(freq);
    delay(duration_ms);
    speaker_stop();
}

void KERNEL_INIT calibrate_pit(void) {
    uint64_t entry_flags;
    uint64_t sync_start;
    uint64_t start_ticks;
    uint64_t observed;
    uint64_t old_freq;
    uint64_t cal_divisor;
    uint64_t divisor;
    uint64_t spin_limit;
    uint64_t expected_freq;

    KERNEL_INIT_LOG("PIT: Calibrating...\n");

    old_freq = pit_freq;
    cal_divisor = PIT_BASE_FREQ / 1000;

    entry_flags = save_flags_cli();
    pit_set_divisor((uint16_t)cal_divisor);

    __asm__ volatile("sti");

    sync_start = tick_count;
    spin_limit = 20000000ULL;
    while (tick_count == sync_start) {
        if (spin_limit-- == 0) {
            calibrated_freq = old_freq;
            us_per_tick = 1000000 / calibrated_freq;
            divisor = PIT_BASE_FREQ / old_freq;
            if (divisor > 65535) divisor = 65535;
            if (divisor < 1) divisor = 1;
            pit_set_divisor((uint16_t)divisor);
            pit_freq = old_freq;
            restore_flags(entry_flags);
            KERNEL_INIT_LOG("PIT: Calibration skipped (no timer IRQs yet)\n");
            return;
        }
        __asm__ volatile("pause");
    }

    start_ticks = tick_count;

    while ((tick_count - start_ticks) < 100) {
        if (spin_limit-- == 0) {
            calibrated_freq = old_freq;
            us_per_tick = 1000000 / calibrated_freq;
            divisor = PIT_BASE_FREQ / old_freq;
            if (divisor > 65535) divisor = 65535;
            if (divisor < 1) divisor = 1;
            pit_set_divisor((uint16_t)divisor);
            pit_freq = old_freq;
            restore_flags(entry_flags);
            KERNEL_INIT_LOG("PIT: Calibration timeout, using base frequency\n");
            return;
        }
        __asm__ volatile("pause");
    }

    observed = tick_count - start_ticks;
    if (observed == 0) observed = 1;
    expected_freq = (observed * 1000) / 100;

    if (expected_freq >= 800 && expected_freq <= 1200) {
        calibrated_freq = (old_freq * expected_freq) / 1000;
    } else {
        KERNEL_INIT_LOG("PIT: Calibration out of range (%u Hz)\n", expected_freq);
        calibrated_freq = old_freq;
    }
    
    us_per_tick = 1000000 / calibrated_freq;
    
    divisor = PIT_BASE_FREQ / old_freq;
    if (divisor > 65535) divisor = 65535;
    if (divisor < 1) divisor = 1;
    pit_set_divisor((uint16_t)divisor);
    pit_freq = old_freq;

    restore_flags(entry_flags);

    KERNEL_INIT_LOG("PIT: Calibrated to %u Hz\n", calibrated_freq);
}
