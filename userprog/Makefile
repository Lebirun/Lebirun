CC = x86_64-elf-gcc
LD = x86_64-elf-ld

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
SYSROOT = $(abspath ../sysroot)
KERNEL_DIR = ../kernel/arch/x86_64
INITRD_DIR = ../initrd
SYSROOT_LIB = ../root/lib

CFLAGS = -Wall -Wextra -ffreestanding -fno-builtin -fno-stack-protector -fno-pic -Os -fomit-frame-pointer -nostdinc -fdiagnostics-color=always
CPPFLAGS = -isystem $(LIBC_ABS)/leblibc/include -isystem $(SYSROOT)/usr/include -isystem $(LIBC_ABS)/leblibc/arch/x86_64 -isystem $(LIBC_ABS)/leblibc/arch/generic -I$(LIBC_ABS)/include -I$(LIBC_ABS)/src

SO_CFLAGS = -Wall -Wextra -ffreestanding -fno-builtin -fno-stack-protector -fPIC -O0 -nostdinc -fvisibility=default

CRT1 = $(SYSROOT)/usr/lib/crt1.o
CRTI = $(SYSROOT)/usr/lib/crti.o
CRTN = $(SYSROOT)/usr/lib/crtn.o

CRT1_ABS = $(SYSROOT)/usr/lib/crt1.o
CRTI_ABS = $(SYSROOT)/usr/lib/crti.o
CRTN_ABS = $(SYSROOT)/usr/lib/crtn.o
LIBC_A = $(SYSROOT)/usr/lib/libc.a
PROGRAMS =

MAKEFILE_DIR := $(dir $(abspath $(lastword $(MAKEFILE_LIST))))
LSH_DIR := $(MAKEFILE_DIR)lsh
LSH_BUILD := $(LSH_DIR)/build-cross
LSH_OUTDIR := $(LSH_BUILD)/.build
HOSTCC ?= gcc
LSH_CONFIGURE_ARGS = --host=x86_64-elf --prefix=/usr

SYSROOT_BIN = ../root/bin
BINDIR = bin

.PHONY: stage-lsh stage-lebutils stage-LebInit stage-login

stage-lsh: bin/lsh.bin
	mkdir -p $(SYSROOT_BIN)
	cp bin/lsh.bin $(SYSROOT_BIN)/lsh
	ln -sf lsh $(SYSROOT_BIN)/sh

stage-lebutils:
	$(MAKE) -C lebutils V=$(V) stage

stage-LebInit:
	$(MAKE) -C LebInit V=$(V) stage

stage-login:
	$(MAKE) -C login V=$(V) stage

stage-liblebui:
	$(MAKE) -C liblebui V=$(V) all

stage-lebinstaller: stage-liblebui
	$(MAKE) -C lebinstaller V=$(V) stage

lsh.bin: bin/lsh.bin

bin/lsh.bin:
	@mkdir -p $(LSH_BUILD)
	@mkdir -p $(BINDIR)
	@if [ ! -f "$(LSH_OUTDIR)/config.mk" ] || [ "$(LSH_DIR)/configure" -nt "$(LSH_OUTDIR)/config.mk" ]; then \
		echo "Configuring lsh..."; \
		mkdir -p "$(LSH_BUILD)"; \
		(cd "$(LSH_BUILD)" && sh ../configure $(LSH_CONFIGURE_ARGS) > "$(LSH_BUILD)/configure.log" 2>&1) || (cat "$(LSH_BUILD)/configure.log" >&2; false); \
	fi
	$(MAKE) -C $(LSH_DIR) OUTDIR="$(LSH_OUTDIR)" clean CFLAGS="-Wall -Wextra -fno-builtin -fno-stack-protector -fno-pic -O2 -nostdinc -isystem $(LIBC_ABS)/leblibc/include -isystem $(SYSROOT)/usr/include -isystem $(LIBC_ABS)/leblibc/arch/x86_64 -isystem $(LIBC_ABS)/leblibc/arch/generic" LDFLAGS="-nostdlib -static -Wl,-z,noexecstack -T $(LIBC_ABS)/user.ld" LIBS="$(CRT1_ABS) $(CRTI_ABS) -L$(SYSROOT)/usr/lib -lc $(CRTN_ABS) -lgcc" lsh
	@cp "$(LSH_DIR)/lsh" "$(MAKEFILE_DIR)bin/lsh.bin"

.PHONY: all clean

all: $(PROGRAMS) stage-lsh stage-lebutils stage-LebInit stage-login stage-lebinstaller

build/%.o: %.c
	$(Q)mkdir -p build
	$(MSG_CC)$(CC) $(CFLAGS) $(CPPFLAGS) -c $< -o $@



bin/%.bin: build/%.o $(CRT1) $(CRTI) $(CRTN) $(LIBC_A)
	$(Q)mkdir -p $(BINDIR)
	$(MSG_LD)$(CC) -nostdlib -static -Wl,-z,noexecstack -T $(LIBC)/user.ld -L$(SYSROOT)/usr/lib -o $@ $(CRT1) $(CRTI) $< -lc $(CRTN) -lgcc

clean:
	rm -f *.so
	rm -rf build $(BINDIR)
	rm -f $(SYSROOT_BIN)/lsh
	rm -f $(SYSROOT_BIN)/sh
	$(MAKE) -C liblebui clean
