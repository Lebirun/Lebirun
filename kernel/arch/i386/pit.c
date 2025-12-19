#include <stdint.h>
#include <stdbool.h>
#include <kernel/tty.h>
#include <kernel/idt.h>
#include <kernel/common.h>
#include <kernel/io.h>
#include <kernel/pit.h>

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

#define MAX_TIMER_CALLBACKS 8

extern volatile uint32_t tick_count;

uint32_t pit_freq = 100;

static uint32_t calibrated_freq = 100;
static uint32_t current_divisor = 0;
static uint64_t uptime_ticks = 0;
static uint32_t last_tick = 0;
static uint32_t tick_remainder_us = 0;
static uint32_t us_per_tick = 10000;

typedef void (*pit_callback_t)(uint32_t ticks);

static struct {
    pit_callback_t callback;
    uint32_t interval;
    uint32_t remaining;
    bool active;
    bool oneshot;
} timer_callbacks[MAX_TIMER_CALLBACKS];

static uint8_t num_callbacks = 0;

static inline void io_wait(void) {
    outb(0x80, 0);
}

static inline uint32_t save_flags_cli(void) {
    uint32_t flags;
    __asm__ volatile(
        "pushfl\n\t"
        "popl %0\n\t"
        "cli"
        : "=r"(flags)
        :
        : "memory"
    );
    return flags;
}

static inline void restore_flags(uint32_t flags) {
    __asm__ volatile(
        "pushl %0\n\t"
        "popfl"
        :
        : "r"(flags)
        : "memory", "cc"
    );
}

static void pit_set_divisor(uint16_t divisor) {
    uint32_t flags = save_flags_cli();
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
    uint32_t flags = save_flags_cli();
    outb(PIT_COMMAND, PIT_CMD_LATCH);
    io_wait();
    uint16_t count = inb(PIT_CHANNEL0_DATA);
    count |= (uint16_t)inb(PIT_CHANNEL0_DATA) << 8;
    restore_flags(flags);
    return count;
}

static uint32_t pit_get_subtick_us(void) {
    if (current_divisor == 0) return 0;
    uint16_t count = pit_read_count();
    uint32_t elapsed = current_divisor - count;
    return (elapsed * 1000000UL) / PIT_BASE_FREQ;
}

void pit_init(uint32_t freq) {
    for (int i = 0; i < MAX_TIMER_CALLBACKS; i++) {
        timer_callbacks[i].callback = 0;
        timer_callbacks[i].active = false;
    }
    num_callbacks = 0;
    
    if (freq == 0) freq = 100;
    if (freq > PIT_BASE_FREQ) freq = PIT_BASE_FREQ;
    if (freq < 19) freq = 19;
    
    pit_freq = freq;
    calibrated_freq = freq;
    
    uint32_t divisor = PIT_BASE_FREQ / freq;
    if (divisor > 65535) divisor = 65535;
    if (divisor < 1) divisor = 1;
    
    us_per_tick = 1000000 / freq;
    
    pit_set_divisor((uint16_t)divisor);
    
    uint32_t actual_freq = PIT_BASE_FREQ / divisor;
    uint32_t error_ppm = 0;
    if (actual_freq > freq) {
        error_ppm = ((actual_freq - freq) * 1000000) / freq;
    } else {
        error_ppm = ((freq - actual_freq) * 1000000) / freq;
    }
    
    printf("PIT: %u Hz (req %u Hz, div %u, err %u ppm)\n", 
           actual_freq, freq, divisor, error_ppm);
}

void pit_set_frequency(uint32_t freq) {
    if (freq == 0) freq = 100;
    if (freq > PIT_BASE_FREQ) freq = PIT_BASE_FREQ;
    if (freq < 19) freq = 19;
    
    pit_freq = freq;
    calibrated_freq = freq;
    us_per_tick = 1000000 / freq;
    
    uint32_t divisor = PIT_BASE_FREQ / freq;
    if (divisor > 65535) divisor = 65535;
    if (divisor < 1) divisor = 1;
    
    pit_set_divisor((uint16_t)divisor);
}

static void delay_ticks(uint32_t ticks) {
    if (ticks == 0) return;
    volatile uint32_t start = tick_count;
    while ((tick_count - start) < ticks) {
        __asm__ volatile("hlt");
    }
}

void delay_us(uint32_t us) {
    if (us == 0) return;
    
    if (us < 50) {
        uint32_t loops = us * 10;
        while (loops--) {
            __asm__ volatile("nop");
        }
        return;
    }
    
    uint32_t start_tick = tick_count;
    uint32_t start_subtick = pit_get_subtick_us();
    uint32_t target_us = us;
    
    while (1) {
        uint32_t current_tick = tick_count;
        uint32_t current_subtick = pit_get_subtick_us();
        
        uint32_t elapsed_ticks = current_tick - start_tick;
        uint32_t elapsed_us = elapsed_ticks * us_per_tick;
        
        if (current_subtick >= start_subtick) {
            elapsed_us += current_subtick - start_subtick;
        } else {
            elapsed_us += us_per_tick - start_subtick + current_subtick;
        }
        
        if (elapsed_us >= target_us) break;
        
        if ((target_us - elapsed_us) > us_per_tick) {
            __asm__ volatile("hlt");
        }
    }
}

void delay(uint32_t ms) {
    if (ms == 0) return;
    
    uint32_t ticks_needed = (ms * calibrated_freq) / 1000;
    uint32_t remainder_us = ((ms * 1000) % (1000000 / calibrated_freq));
    
    if (ticks_needed > 0) {
        delay_ticks(ticks_needed);
    }
    
    if (remainder_us > 0) {
        delay_us(remainder_us);
    } else if (ticks_needed == 0) {
        delay_us(ms * 1000);
    }
}

void delay_inMs(uint32_t ms) {
    delay(ms);
}

void delay_inSecs(uint32_t secs) {
    while (secs > 0) {
        uint32_t chunk = (secs > 60) ? 60 : secs;
        delay(chunk * 1000);
        secs -= chunk;
    }
}

void delay_inMins(uint32_t mins) {
    while (mins > 0) {
        delay_inSecs(60);
        mins--;
    }
}

uint32_t pit_get_ticks(void) {
    return tick_count;
}

uint64_t pit_get_ticks64(void) {
    uint32_t flags = save_flags_cli();
    uint32_t current = tick_count;
    uint32_t delta = current - last_tick;
    uptime_ticks += delta;
    last_tick = current;
    uint64_t result = uptime_ticks;
    restore_flags(flags);
    return result;
}

uint64_t pit_get_uptime_us(void) {
    uint64_t ticks = pit_get_ticks64();
    uint64_t base_us = (ticks * 1000000ULL) / calibrated_freq;
    return base_us + pit_get_subtick_us();
}

uint64_t pit_get_uptime_ms(void) {
    return pit_get_uptime_us() / 1000;
}

uint32_t pit_get_uptime_secs(void) {
    return (uint32_t)(pit_get_uptime_ms() / 1000);
}

uint32_t pit_get_frequency(void) {
    return calibrated_freq;
}

uint32_t pit_ticks_to_ms(uint32_t ticks) {
    return (ticks * 1000) / calibrated_freq;
}

uint32_t pit_ms_to_ticks(uint32_t ms) {
    return (ms * calibrated_freq) / 1000;
}

uint64_t pit_ticks_to_us(uint32_t ticks) {
    return ((uint64_t)ticks * 1000000ULL) / calibrated_freq;
}

uint32_t pit_us_to_ticks(uint32_t us) {
    return (us * calibrated_freq) / 1000000;
}

int pit_register_callback(pit_callback_t callback, uint32_t interval_ticks, bool oneshot) {
    uint32_t flags = save_flags_cli();
    
    int slot = -1;
    for (int i = 0; i < MAX_TIMER_CALLBACKS; i++) {
        if (!timer_callbacks[i].active) {
            slot = i;
            break;
        }
    }
    
    if (slot < 0) {
        restore_flags(flags);
        return -1;
    }
    
    timer_callbacks[slot].callback = callback;
    timer_callbacks[slot].interval = interval_ticks;
    timer_callbacks[slot].remaining = interval_ticks;
    timer_callbacks[slot].active = true;
    timer_callbacks[slot].oneshot = oneshot;
    num_callbacks++;
    
    restore_flags(flags);
    return slot;
}

void pit_unregister_callback(int handle) {
    if (handle < 0 || handle >= MAX_TIMER_CALLBACKS) return;
    
    uint32_t flags = save_flags_cli();
    if (timer_callbacks[handle].active) {
        timer_callbacks[handle].active = false;
        timer_callbacks[handle].callback = 0;
        num_callbacks--;
    }
    restore_flags(flags);
}

void pit_process_callbacks(void) {
    for (int i = 0; i < MAX_TIMER_CALLBACKS; i++) {
        if (timer_callbacks[i].active) {
            timer_callbacks[i].remaining--;
            if (timer_callbacks[i].remaining == 0) {
                if (timer_callbacks[i].callback) {
                    timer_callbacks[i].callback(tick_count);
                }
                if (timer_callbacks[i].oneshot) {
                    timer_callbacks[i].active = false;
                    num_callbacks--;
                } else {
                    timer_callbacks[i].remaining = timer_callbacks[i].interval;
                }
            }
        }
    }
}

void speaker_play(uint32_t freq) {
    if (freq == 0) {
        speaker_stop();
        return;
    }
    
    uint32_t divisor = PIT_BASE_FREQ / freq;
    if (divisor > 65535) divisor = 65535;
    if (divisor < 1) divisor = 1;
    
    uint32_t flags = save_flags_cli();
    
    outb(PIT_COMMAND, PIT_CMD_CH2_INIT);
    io_wait();
    outb(PIT_CHANNEL2_DATA, divisor & 0xFF);
    io_wait();
    outb(PIT_CHANNEL2_DATA, (divisor >> 8) & 0xFF);
    io_wait();
    
    uint8_t tmp = inb(SPEAKER_PORT);
    outb(SPEAKER_PORT, tmp | SPEAKER_ENABLE);
    
    restore_flags(flags);
}

void speaker_stop(void) {
    uint8_t tmp = inb(SPEAKER_PORT);
    outb(SPEAKER_PORT, tmp & ~SPEAKER_ENABLE);
}

void speaker_beep(uint32_t freq, uint32_t duration_ms) {
    speaker_play(freq);
    delay(duration_ms);
    speaker_stop();
}

void calibrate_pit(void) {
    printf("PIT: Calibrating...\n");
    
    uint32_t old_freq = pit_freq;
    uint32_t cal_divisor = PIT_BASE_FREQ / 1000;
    
    uint32_t flags = save_flags_cli();
    pit_set_divisor((uint16_t)cal_divisor);
    restore_flags(flags);
    
    __asm__ volatile("sti");
    
    volatile uint32_t sync_start = tick_count;
    while (tick_count == sync_start) {
        __asm__ volatile("hlt");
    }
    
    volatile uint32_t start_ticks = tick_count;
    uint16_t count_start = pit_read_count();
    
    const uint32_t wait_ticks = 100;
    while ((tick_count - start_ticks) < wait_ticks) {
        __asm__ volatile("hlt");
    }
    
    uint16_t count_end = pit_read_count();
    volatile uint32_t observed = tick_count - start_ticks;
    
    __asm__ volatile("cli");
    
    uint32_t expected_freq = (observed * 1000) / wait_ticks;
    
    if (expected_freq >= 800 && expected_freq <= 1200) {
        calibrated_freq = (old_freq * expected_freq) / 1000;
    } else {
        printf("PIT: Calibration out of range (%u Hz)\n", expected_freq);
        calibrated_freq = old_freq;
    }
    
    us_per_tick = 1000000 / calibrated_freq;
    
    uint32_t divisor = PIT_BASE_FREQ / old_freq;
    pit_set_divisor((uint16_t)divisor);
    pit_freq = old_freq;
    
    (void)count_start;
    (void)count_end;
    
    printf("PIT: Calibrated to %u Hz\n", calibrated_freq);
}