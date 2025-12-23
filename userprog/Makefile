CC = i686-elf-gcc
LD = i686-elf-ld

LIBC = ../libc
LIBC_COMMON = ../libc-common
KERNEL_DIR = ../kernel/arch/i386
INITRD_DIR = ../initrd
NCURSES_DIR = ncurses
CFLAGS = -Wall -Wextra -ffreestanding -fno-builtin -nostdlib -fno-stack-protector -fno-pic -m32 -O2
CPPFLAGS = -I$(NCURSES_DIR)/include -I$(LIBC)/include -I$(LIBC_COMMON)/include
LDFLAGS = -T $(LIBC)/user.ld

CRT0 = $(LIBC)/crt/crt0.o
LIBC_A = $(LIBC)/libc.a
NCURSES_A = $(INITRD_DIR)/libncurses.a

PROGRAMS = shell.bin pong.bin

KERNEL_USER_SHELL = ../kernel/arch/i386/user_shell.bin

.PHONY: copy-shell copy-pong

copy-shell: shell.bin
	cp shell.bin $(KERNEL_USER_SHELL)

copy-pong: pong.bin
	cp pong.bin $(INITRD_DIR)/pong.bin

.PHONY: all clean libc

all: libc crt0 $(PROGRAMS) copy-shell copy-pong

libc:
	$(MAKE) -C $(LIBC)

crt0: $(CRT0)

$(CRT0): $(LIBC)/crt/crt0.c
	$(CC) $(CFLAGS) $(CPPFLAGS) -c $< -o $@

$(LIBC_A):
	$(MAKE) -C $(LIBC)

%.o: %.c
	$(CC) $(CFLAGS) $(CPPFLAGS) -c $< -o $@

%.bin: %.o $(CRT0) $(LIBC_A)
	$(CC) -nostdlib $(LDFLAGS) -o $@ $(CRT0) $< $(LIBC_A) -lgcc

clean:
	rm -f *.o *.bin
	rm -f $(KERNEL_USER_SHELL)
	rm -f $(CRT0)
	rm -f $(INITRD_DIR)/pong.bin