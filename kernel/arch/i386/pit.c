#include <stdint.h>
#include <kernel/tty.h>
#include <kernel/idt.h>
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