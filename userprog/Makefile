CC = i686-elf-gcc
LD = i686-elf-ld

LIBC = ../libc
LIBC_ABS = $(abspath $(LIBC))
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

CRT1_ABS = $(LIBC_ABS)/lib/crt1.o
CRTI_ABS = $(LIBC_ABS)/lib/crti.o
CRTN_ABS = $(LIBC_ABS)/lib/crtn.o
LIBC_A = $(LIBC)/lib/libc.a
LIBLEBIRUN = $(LIBC)/lib/liblebirun.a
NCURSES_A = $(INITRD_DIR)/libncurses.a

PROGRAMS = pong.bin test100.bin
SHARED_LIBS = libtest.so

MAKEFILE_DIR := $(dir $(abspath $(lastword $(MAKEFILE_LIST))))
LSH_DIR := $(MAKEFILE_DIR)lsh
LSH_BUILD := $(LSH_DIR)/build-cross
LSH_OUTDIR := $(LSH_BUILD)/.build
HOSTCC ?= gcc
LSH_CONFIGURE_ARGS = --host=i686-elf --prefix=/usr

KERNEL_USER_SHELL = ../kernel/arch/i386/user_shell.bin
SYSROOT_BIN = ../root/bin

.PHONY: copy-shell copy-pong copy-test100 copy-libtest stage-lsh stage-coreutils

copy-shell: lsh.bin
	cp lsh.bin $(KERNEL_USER_SHELL)
	@echo "Using lsh.bin as user shell"

copy-pong: pong.bin
	cp pong.bin $(INITRD_DIR)/pong.bin

copy-test100: test100.bin
	cp test100.bin $(SYSROOT_BIN)/test100

copy-libtest: libtest.so#
	mkdir -p $(SYSROOT_LIB)
	cp libtest.so $(SYSROOT_LIB)/libtest.so

stage-lsh: lsh.bin
	mkdir -p $(SYSROOT_BIN)
	cp lsh.bin $(SYSROOT_BIN)/lsh
	ln -sf lsh $(SYSROOT_BIN)/sh

stage-coreutils:
	$(MAKE) -C coreutils stage

lsh.bin:
	@echo "Building lsh in $(LSH_DIR)"
	@test -d "$(LSH_DIR)" || { echo "ERROR: missing $(LSH_DIR)"; exit 1; }
	@mkdir -p $(LSH_BUILD)
	@if [ ! -f "$(LSH_OUTDIR)/config.mk" ] || [ "$(LSH_DIR)/configure" -nt "$(LSH_OUTDIR)/config.mk" ]; then \
		echo "Configuring lsh..."; \
		mkdir -p "$(LSH_BUILD)"; \
		(cd "$(LSH_BUILD)" && sh ../configure $(LSH_CONFIGURE_ARGS) > "$(LSH_BUILD)/configure.log" 2>&1) || (cat "$(LSH_BUILD)/configure.log" >&2; false); \
	fi
	$(MAKE) -C $(LSH_DIR) OUTDIR="$(LSH_OUTDIR)" clean CFLAGS="-Wall -Wextra -fno-builtin -fno-stack-protector -fno-pic -m32 -O2 -nostdinc -isystem $(LIBC_ABS)/include" LDFLAGS="-nostdlib -static -Wl,-z,noexecstack -T $(LIBC_ABS)/user.ld" LIBS="$(CRT1_ABS) $(CRTI_ABS) -L$(LIBC_ABS)/lib -llebirun -lc $(CRTN_ABS) -lgcc" lsh
	@cp "$(LSH_DIR)/lsh" "$(MAKEFILE_DIR)lsh.bin"

.PHONY: all clean

all: $(PROGRAMS) $(SHARED_LIBS) copy-shell copy-pong copy-test100 copy-libtest stage-lsh stage-coreutils

%.o: %.c
	$(CC) $(CFLAGS) $(CPPFLAGS) -c $< -o $@

libtest_pic.o: libtest.c
	$(CC) $(SO_CFLAGS) $(CPPFLAGS) -c $< -o $@

libtest.so: libtest_pic.o
	$(CC) -nostdlib -shared -Wl,-z,noexecstack -T $(LIBC)/shared.ld -o $@ $< -lgcc

%.bin: %.o $(CRT1) $(CRTI) $(CRTN) $(LIBC_A) $(LIBLEBIRUN)
	$(CC) -nostdlib -static -Wl,-z,noexecstack -T $(LIBC)/user.ld -L$(LIBC)/lib -o $@ $(CRT1) $(CRTI) $< -llebirun -lc $(CRTN) -lgcc

clean:
	rm -f *.o *.bin *.so
	rm -f $(KERNEL_USER_SHELL)
	rm -f $(INITRD_DIR)/pong.bin
	rm -f $(INITRD_DIR)/stdio_test.bin
	rm -f $(SYSROOT_BIN)/test100
	rm -f $(SYSROOT_BIN)/lsh
	rm -f lsh.bin
	rm -f $(SYSROOT_LIB)/libtest.so