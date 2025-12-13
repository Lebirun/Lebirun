CC = i686-elf-gcc
LD = i686-elf-ld

USERLIBC = ../userlibc
KERNEL_DIR = ../kernel/arch/i386
CFLAGS = -Wall -Wextra -ffreestanding -fno-builtin -nostdlib -fno-stack-protector -fno-pic -m32 -O2
CPPFLAGS = -I$(USERLIBC)/include
LDFLAGS = -T $(USERLIBC)/user.ld

CRT0 = $(USERLIBC)/obj/crt0.o
LIBUSER = $(USERLIBC)/libuser.a

PROGRAMS = shell.bin

KERNEL_USER_SHELL = ../kernel/arch/i386/user_shell.bin

.PHONY: copy-shell

copy-shell: shell.bin
	cp shell.bin $(KERNEL_USER_SHELL)

.PHONY: all clean userlibc

all: userlibc $(PROGRAMS) copy-shell

userlibc:
	$(MAKE) -C $(USERLIBC)

%.o: %.c
	$(CC) $(CFLAGS) $(CPPFLAGS) -c $< -o $@

%.bin: %.o $(CRT0) $(LIBUSER)
	$(LD) $(LDFLAGS) -o $@ $(CRT0) $< $(LIBUSER)

clean:
	rm -f *.o *.bin
	rm -f $(KERNEL_USER_SHELL)
