CC = i686-elf-gcc
LD = i686-elf-ld

V ?= 0
ifeq ($(V),0)
  Q = @
  MSG_CC    = @printf '  CC      %s\n' $<;
  MSG_LD    = @printf '  LD      %s\n' $@;
else
  Q =
  MSG_CC =
  MSG_LD =
endif

LIBC = ../libc
LIBC_ABS = $(abspath $(LIBC))
KERNEL_DIR = ../kernel/arch/i386
INITRD_DIR = ../initrd
SYSROOT_LIB = ../root/lib

CFLAGS = -Wall -Wextra -ffreestanding -fno-builtin -fno-stack-protector -fno-pic -m32 -Os -fomit-frame-pointer -nostdinc -fdiagnostics-color=always
CPPFLAGS = -isystem $(LIBC_ABS)/leblibc/include -isystem $(LIBC_ABS)/leblibc/build-i386/obj/include -isystem $(LIBC_ABS)/leblibc/arch/i386 -isystem $(LIBC_ABS)/leblibc/arch/generic -I$(LIBC_ABS)/include -I$(LIBC_ABS)/src

SO_CFLAGS = -Wall -Wextra -ffreestanding -fno-builtin -fno-stack-protector -fPIC -m32 -O0 -nostdinc -fvisibility=default

CRT1 = $(LIBC)/leblibc/build-i386/lib/crt1.o
CRTI = $(LIBC)/leblibc/build-i386/lib/crti.o
CRTN = $(LIBC)/leblibc/build-i386/lib/crtn.o

CRT1_ABS = $(LIBC_ABS)/leblibc/build-i386/lib/crt1.o
CRTI_ABS = $(LIBC_ABS)/leblibc/build-i386/lib/crti.o
CRTN_ABS = $(LIBC_ABS)/leblibc/build-i386/lib/crtn.o
LIBC_A = $(LIBC)/leblibc/build-i386/lib/libc.a
PROGRAMS = pong.bin

MAKEFILE_DIR := $(dir $(abspath $(lastword $(MAKEFILE_LIST))))
LSH_DIR := $(MAKEFILE_DIR)lsh
LSH_BUILD := $(LSH_DIR)/build-cross
LSH_OUTDIR := $(LSH_BUILD)/.build
HOSTCC ?= gcc
LSH_CONFIGURE_ARGS = --host=i686-elf --prefix=/usr

SYSROOT_BIN = ../root/bin

.PHONY: copy-pong stage-lsh stage-coreutils stage-LebInit stage-login

copy-pong: pong.bin
	cp pong.bin $(INITRD_DIR)/pong.bin

stage-lsh: lsh.bin
	mkdir -p $(SYSROOT_BIN)
	cp lsh.bin $(SYSROOT_BIN)/lsh
	ln -sf lsh $(SYSROOT_BIN)/sh

stage-coreutils:
	$(MAKE) -C coreutils V=$(V) stage

stage-LebInit:
	$(MAKE) -C LebInit V=$(V) stage

stage-login:
	$(MAKE) -C login V=$(V) stage

lsh.bin:
	@mkdir -p $(LSH_BUILD)
	@if [ ! -f "$(LSH_OUTDIR)/config.mk" ] || [ "$(LSH_DIR)/configure" -nt "$(LSH_OUTDIR)/config.mk" ]; then \
		echo "Configuring lsh..."; \
		mkdir -p "$(LSH_BUILD)"; \
		(cd "$(LSH_BUILD)" && sh ../configure $(LSH_CONFIGURE_ARGS) > "$(LSH_BUILD)/configure.log" 2>&1) || (cat "$(LSH_BUILD)/configure.log" >&2; false); \
	fi
	@if [ ! -f "$(LIBC_ABS)/leblibc/build-i386/lib/crti.o" ]; then \
		echo "Building leblibc runtime objects..."; \
		$(MAKE) -C "$(LIBC_ABS)/leblibc" build-i386 || (cat "$(LIBC_ABS)/leblibc/build-i386/config.log" >&2; false); \
	fi; \
	$(MAKE) -C $(LSH_DIR) OUTDIR="$(LSH_OUTDIR)" clean CFLAGS="-Wall -Wextra -fno-builtin -fno-stack-protector -fno-pic -m32 -O2 -nostdinc -isystem $(LIBC_ABS)/leblibc/include -isystem $(LIBC_ABS)/leblibc/build-i386/obj/include -isystem $(LIBC_ABS)/leblibc/arch/i386 -isystem $(LIBC_ABS)/leblibc/arch/generic" LDFLAGS="-nostdlib -static -Wl,-z,noexecstack -T $(LIBC_ABS)/user.ld" LIBS="$(CRT1_ABS) $(CRTI_ABS) -L$(LIBC_ABS)/leblibc/build-i386/lib -lc $(CRTN_ABS) -lgcc" lsh
	@cp "$(LSH_DIR)/lsh" "$(MAKEFILE_DIR)lsh.bin"

.PHONY: all clean

all: $(PROGRAMS) copy-pong stage-lsh stage-coreutils stage-LebInit stage-login

%.o: %.c
	$(MSG_CC)$(CC) $(CFLAGS) $(CPPFLAGS) -c $< -o $@



%.bin: %.o $(CRT1) $(CRTI) $(CRTN) $(LIBC_A)
	$(MSG_LD)$(CC) -nostdlib -static -Wl,-z,noexecstack -T $(LIBC)/user.ld -L$(LIBC)/leblibc/build-i386/lib -o $@ $(CRT1) $(CRTI) $< -lc $(CRTN) -lgcc

clean:
	rm -f *.o *.bin *.so
	rm -f $(INITRD_DIR)/pong.bin
	rm -f $(INITRD_DIR)/stdio_test.bin
	rm -f $(SYSROOT_BIN)/lsh
	rm -f $(SYSROOT_BIN)/sh
	rm -f lsh.bin
