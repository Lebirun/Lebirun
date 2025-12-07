#include <stdint.h>
#include <kernel/tty.h>
#include <kernel/idt.h>
#include <kernel/common.h>
#include "io.h"

void pit_init(uint32_t freq) {
    uint32_t divisor = 1193182 / freq; 
    outb(0x43, 0x36);  
    outb(0x40, divisor & 0xFF);        
    outb(0x40, (divisor >> 8) & 0xFF); 
    terminal_writestring("PIT set to ~");
    terminal_writestring("100 Hz\n");
}

void delay(uint32_t ms) {
    uint32_t ticks_needed = (ms * 100) / 1000;
    uint32_t start = tick_count;
    while (tick_count - start < ticks_needed) {
        asm volatile("hlt");
    }
}

void calibrate_pit(void) {
    terminal_writestring("Calibrating PIT...\n");
    uint32_t start_ticks = tick_count;
    uint32_t loops = 10000000;
    for (uint32_t i = 0; i < loops; i++) {
        asm volatile("nop");
    }
    uint32_t observed_ticks = tick_count - start_ticks;
    terminal_writestring("Observed ");
    char buf[10]; uint32_t temp = observed_ticks; int j=9; buf[9]='\0';
    do { buf[--j] = '0' + (temp % 10); temp /= 10; } while (temp);
    terminal_writestring(&buf[j]);
    terminal_writestring(" ticks in ~1s.\n");
    if (observed_ticks > 0) {
        uint32_t target_ticks = 100;
        uint32_t old_divisor = 1193182 / 100;
        uint32_t new_divisor = (old_divisor * target_ticks) / observed_ticks;
        outb(0x43, 0x36);
        outb(0x40, new_divisor & 0xFF);
        outb(0x40, (new_divisor >> 8) & 0xFF);
        terminal_writestring("New divisor: 0x");
        print_hex(new_divisor);
        terminal_writestring("\n");
    }
}