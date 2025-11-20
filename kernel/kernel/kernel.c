#include <stdio.h>
#include <stdint.h>
#include <kernel/tty.h>

static inline unsigned long read_cr3(void) {
	unsigned long val;
	__asm__ volatile ("mov %%cr3, %0" : "=r"(val));
	return val;
}

static inline unsigned long read_cr0(void) {
	unsigned long val;
	__asm__ volatile ("mov %%cr0, %0" : "=r"(val));
	return val;
}

static void print_hex(unsigned long v) {
	char buf[9];
	buf[8] = '\0';
	for (int i = 0; i < 8; ++i) {
		unsigned int nib = (v >> ((7 - i) * 4)) & 0xF;
		buf[i] = (nib < 10) ? ('0' + nib) : ('A' + (nib - 10));
	}
	printf("0x");
	printf(buf);
}

extern uint32_t boot_page_directory[1024] __attribute__((aligned(4096)));

void kernel_main(void) {
	terminal_initialize();
	printf("Hello, kernel World!\n");
	unsigned long cr3 = read_cr3();
	unsigned long cr0 = read_cr0();
	printf("CR3="); print_hex(cr3);
	printf(" CR0="); print_hex(cr0);
	printf("\n");
	unsigned long expected = (unsigned long)(&boot_page_directory) - 0xC0000000UL;
	printf("EXP="); print_hex(expected);
	printf("\n");
	if (cr3 == expected) {
		printf("PAGING OK\n");
	} else {
		printf("PAGING MISMATCH\n");
	}
	unsigned long *pd = (unsigned long*)0xFFFFF000UL;
	printf("PDE0="); print_hex(pd[0]);
	printf("\n");
}
