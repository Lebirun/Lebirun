#include <stdint.h>
#include <kernel/tty.h>
#include <kernel/idt.h>
#include <kernel/common.h>
#include <kernel/io.h>
#include <kernel/pit.h>

#define PIT_BASE_FREQ 1193182

uint32_t pit_freq = 100;
static uint32_t calibrated_freq = 100;

void pit_init(uint32_t freq) {
    pit_freq = freq;
    uint32_t divisor = PIT_BASE_FREQ / freq;
    outb(0x43, 0x36);
    outb(0x40, divisor & 0xFF);
    outb(0x40, (divisor >> 8) & 0xFF);
    printf("PIT initialized at %u Hz\n", freq);
}

static void delay_ticks(uint32_t ticks) {
    uint32_t start = tick_count;
    while (tick_count - start < ticks) {
        asm volatile("hlt");
    }
}

void delay(uint32_t ms) {
    uint32_t ticks_needed = (ms * calibrated_freq) / 1000;
    if (ticks_needed == 0) ticks_needed = 1;
    delay_ticks(ticks_needed);
}

void delay_inMs(uint32_t ms) {
    delay(ms);
}

void delay_inSecs(uint32_t secs) {
    delay(secs * 1000);
}

void delay_inMins(uint32_t mins) {
    delay(mins * 60 * 1000);
}

uint32_t pit_get_ticks(void) {
    return tick_count;
}

void calibrate_pit(void) {
    printf("Calibrating PIT (fast)...\n");
    
    uint32_t divisor = PIT_BASE_FREQ / 1000;
    outb(0x43, 0x36);
    outb(0x40, divisor & 0xFF);
    outb(0x40, (divisor >> 8) & 0xFF);
    
    asm volatile("sti");
    
    uint32_t sync_start = tick_count;
    while (tick_count == sync_start) {
        asm volatile("hlt");
    }
    
    uint32_t start_ticks = tick_count;
    
    outb(0x43, 0x00);
    uint16_t count_start = inb(0x40);
    count_start |= (uint16_t)inb(0x40) << 8;
    
    uint32_t wait_ticks = 50;
    while (tick_count - start_ticks < wait_ticks) {
        asm volatile("hlt");
    }
    
    outb(0x43, 0x00);
    uint16_t count_end = inb(0x40);
    count_end |= (uint16_t)inb(0x40) << 8;
    
    uint32_t observed = tick_count - start_ticks;
    
    asm volatile("cli");
    
    calibrated_freq = (observed * 1000) / wait_ticks;
    if (calibrated_freq < 50) calibrated_freq = 100;
    if (calibrated_freq > 2000) calibrated_freq = 100;
    
    divisor = PIT_BASE_FREQ / pit_freq;
    outb(0x43, 0x36);
    outb(0x40, divisor & 0xFF);
    outb(0x40, (divisor >> 8) & 0xFF);
    
    printf("PIT calibrated: %u Hz (observed %u ticks in 50ms)\n", calibrated_freq, observed);
}