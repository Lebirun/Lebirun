#include <stdint.h>
#include <kernel/tty.h>
#include <kernel/idt.h>
#include <kernel/common.h>
#include "io.h"

uint32_t pit_freq = 100;
void pit_init(uint32_t freq) {
    pit_freq = freq;
    uint32_t divisor = 1193182 / freq; 
    outb(0x43, 0x36);  
    outb(0x40, divisor & 0xFF);        
    outb(0x40, (divisor >> 8) & 0xFF); 
    terminal_writestring("PIT set to ~");
    terminal_writestring("100 Hz\n");
}

void delay(uint32_t ms) {
    uint32_t ticks_needed = (ms * pit_freq) / 1000;
    uint32_t start = tick_count;
    while (tick_count - start < ticks_needed) {
        asm volatile("hlt");
    }
}

void calibrate_pit(void) {
    terminal_writestring("Calibrating PIT...\n");
    
    uint32_t divisor = 1193182 / 100;
    outb(0x43, 0x36);
    outb(0x40, divisor & 0xFF);
    outb(0x40, (divisor >> 8) & 0xFF);
    
    asm volatile("sti");
    
    uint32_t sync_start = tick_count;
    while (tick_count == sync_start) {
        asm volatile("hlt");
    }
    
    uint32_t start_ticks = tick_count;
    uint32_t target_wait = 100;
    while (tick_count - start_ticks < target_wait) {
        asm volatile("hlt");
    }
    uint32_t observed_ticks = tick_count - start_ticks;
    
    asm volatile("cli");
    
    terminal_writestring("Observed ");
    char buf[12]; uint32_t temp = observed_ticks; int j = 11; buf[11] = '\0';
    if (temp == 0) { buf[--j] = '0'; }
    else { while (temp) { buf[--j] = '0' + (temp % 10); temp /= 10; } }
    terminal_writestring(&buf[j]);
    terminal_writestring(" ticks in ~1s.\n");
}