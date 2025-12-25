CC = i686-elf-gcc
LD = i686-elf-ld

LIBC = ../libc
KERNEL_DIR = ../kernel/arch/i386
INITRD_DIR = ../initrd
NCURSES_DIR = ncurses
SYSROOT_LIB = ../root/lib

CFLAGS = -Wall -Wextra -ffreestanding -fno-builtin -fno-stack-protector -fno-pic -m32 -O2 -nostdinc
CPPFLAGS = -isystem $(LIBC)/include -I$(NCURSES_DIR)/include

SO_CFLAGS = -Wall -Wextra -ffreestanding -fno-builtin -fno-stack-protector -fPIC -m32 -O0 -nostdinc -fvisibility=default

CRT1 = $(LIBC)/lib/crt1.o
CRTI = $(LIBC)/lib/crti.o
CRTN = $(LIBC)/lib/crtn.o
LIBC_A = $(LIBC)/lib/libc.a
LIBLEBIRUN = $(LIBC)/lib/liblebirun.a
NCURSES_A = $(INITRD_DIR)/libncurses.a

PROGRAMS = shell.bin pong.bin test100.bin
SHARED_LIBS = libtest.so

KERNEL_USER_SHELL = ../kernel/arch/i386/user_shell.bin
SYSROOT_BIN = ../root/bin

.PHONY: copy-shell copy-pong copy-test100 copy-libtest

copy-shell: shell.bin
	cp shell.bin $(KERNEL_USER_SHELL)

copy-pong: pong.bin
	cp pong.bin $(INITRD_DIR)/pong.bin

copy-test100: test100.bin
	cp test100.bin $(SYSROOT_BIN)/test100

copy-libtest: libtest.so
	mkdir -p $(SYSROOT_LIB)
	cp libtest.so $(SYSROOT_LIB)/libtest.so

.PHONY: all clean

all: $(PROGRAMS) $(SHARED_LIBS) copy-shell copy-pong copy-test100 copy-libtest

%.o: %.c
	$(CC) $(CFLAGS) $(CPPFLAGS) -c $< -o $@

libtest_pic.o: libtest.c
	$(CC) $(SO_CFLAGS) $(CPPFLAGS) -c $< -o $@

libtest.so: libtest_pic.o
	$(CC) -nostdlib -shared -T $(LIBC)/shared.ld -o $@ $< -lgcc

%.bin: %.o $(CRT1) $(CRTI) $(CRTN) $(LIBC_A) $(LIBLEBIRUN)
	$(CC) -nostdlib -static -T $(LIBC)/user.ld -L$(LIBC)/lib -o $@ $(CRT1) $(CRTI) $< -llebirun -lc $(CRTN) -lgcc

clean:
	rm -f *.o *.bin *.so
	rm -f $(KERNEL_USER_SHELL)
	rm -f $(INITRD_DIR)/pong.bin
	rm -f $(SYSROOT_BIN)/test100
	rm -f $(SYSROOT_LIB)/libtest.so