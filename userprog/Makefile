CC = i686-elf-gcc
LD = i686-elf-ld

LIBC = ../libc
KERNEL_DIR = ../kernel/arch/i386
INITRD_DIR = ../initrd
NCURSES_DIR = ncurses

CFLAGS = -Wall -Wextra -ffreestanding -fno-builtin -fno-stack-protector -fno-pic -m32 -O2 -nostdinc
CPPFLAGS = -isystem $(LIBC)/include -I$(NCURSES_DIR)/include

CRT1 = $(LIBC)/lib/crt1.o
CRTI = $(LIBC)/lib/crti.o
CRTN = $(LIBC)/lib/crtn.o
LIBC_A = $(LIBC)/lib/libc.a
LIBLEBIRUN = $(LIBC)/lib/liblebirun.a
NCURSES_A = $(INITRD_DIR)/libncurses.a

PROGRAMS = shell.bin pong.bin

KERNEL_USER_SHELL = ../kernel/arch/i386/user_shell.bin

.PHONY: copy-shell copy-pong

copy-shell: shell.bin
	cp shell.bin $(KERNEL_USER_SHELL)

copy-pong: pong.bin
	cp pong.bin $(INITRD_DIR)/pong.bin

.PHONY: all clean

all: $(PROGRAMS) copy-shell copy-pong

%.o: %.c
	$(CC) $(CFLAGS) $(CPPFLAGS) -c $< -o $@

%.bin: %.o $(CRT1) $(CRTI) $(CRTN) $(LIBC_A) $(LIBLEBIRUN)
	$(CC) -nostdlib -static -T $(LIBC)/user.ld -L$(LIBC)/lib -o $@ $(CRT1) $(CRTI) $< -llebirun -lc $(CRTN) -lgcc

clean:
	rm -f *.o *.bin
	rm -f $(KERNEL_USER_SHELL)
	rm -f $(INITRD_DIR)/pong.bin