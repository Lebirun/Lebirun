CC = i686-elf-gcc
LD = i686-elf-ld

LIBC = ../libc
LIBC_COMMON = ../libc-common
KERNEL_DIR = ../kernel/arch/i386
CFLAGS = -Wall -Wextra -ffreestanding -fno-builtin -nostdlib -fno-stack-protector -fno-pic -m32 -O2
CPPFLAGS = -I$(LIBC)/include -I$(LIBC_COMMON)/include
LDFLAGS = -T $(LIBC)/user.ld

CRT0 = $(LIBC)/crt/crt0.o
LIBC_A = $(LIBC)/libc.a

PROGRAMS = shell.bin

KERNEL_USER_SHELL = ../kernel/arch/i386/user_shell.bin

.PHONY: copy-shell

copy-shell: shell.bin
	cp shell.bin $(KERNEL_USER_SHELL)

.PHONY: all clean libc

all: libc crt0 $(PROGRAMS) copy-shell

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
