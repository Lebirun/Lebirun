CC = x86_64-elf-gcc
STRIP = x86_64-elf-strip

V ?= 0
ifeq ($(V),0)
  Q = @
  MSG_CC    = @printf '  CC      %s\n' $<;
  MSG_LD    = @printf '  LD      %s\n' $@;
  MSG_STRIP = @printf '  STRIP   %s\n' $@;
else
  Q =
  MSG_CC =
  MSG_LD =
  MSG_STRIP =
endif

LIBC = ../../libc
LIBC_ABS = $(abspath $(LIBC))
SYSROOT = ../../sysroot

CFLAGS = -Wall -Wextra -ffreestanding -fno-builtin -fno-stack-protector -fno-pic -Os -fomit-frame-pointer -nostdinc -ffunction-sections -fdata-sections -fdiagnostics-color=always
CPPFLAGS = -isystem $(LIBC_ABS)/leblibc/include -isystem $(abspath $(SYSROOT))/usr/include -isystem $(LIBC_ABS)/leblibc/arch/x86_64 -isystem $(LIBC_ABS)/leblibc/arch/generic -I$(LIBC_ABS)/include -I$(LIBC_ABS)/src

CRT1 = $(SYSROOT)/usr/lib/crt1.o
CRTI = $(SYSROOT)/usr/lib/crti.o
CRTN = $(SYSROOT)/usr/lib/crtn.o
LIBC_A = $(SYSROOT)/usr/lib/libc.a

LD_SCRIPT = $(LIBC)/user.ld

SYSROOT_BIN = ../../root/bin
SYSROOT_SBIN = ../../root/sbin

LOGIN_SRCS = src/login.c
LOGIN_OBJS = $(patsubst src/%.c,build/%.o,$(LOGIN_SRCS))

GETTY_SRCS = src/getty.c
GETTY_OBJS = $(patsubst src/%.c,build/%.o,$(GETTY_SRCS))

BINDIR = bin

.PHONY: all clean stage

all: $(BINDIR)/login.bin $(BINDIR)/getty.bin

build/%.o: src/%.c
	$(Q)mkdir -p $(dir $@)
	$(MSG_CC)$(CC) $(CFLAGS) $(CPPFLAGS) -c $< -o $@

$(BINDIR)/login.bin: $(LOGIN_OBJS) $(CRT1) $(CRTI) $(CRTN) $(LIBC_A)
	$(Q)mkdir -p $(BINDIR)
	$(MSG_LD)$(CC) -nostdlib -static -Wl,-z,noexecstack -Wl,--gc-sections -T $(LD_SCRIPT) -L$(SYSROOT)/usr/lib -o $@ $(CRT1) $(CRTI) $(LOGIN_OBJS) -lc $(CRTN) -lgcc

$(BINDIR)/getty.bin: $(GETTY_OBJS) $(CRT1) $(CRTI) $(CRTN) $(LIBC_A)
	$(Q)mkdir -p $(BINDIR)
	$(MSG_LD)$(CC) -nostdlib -static -Wl,-z,noexecstack -Wl,--gc-sections -T $(LD_SCRIPT) -L$(SYSROOT)/usr/lib -o $@ $(CRT1) $(CRTI) $(GETTY_OBJS) -lc $(CRTN) -lgcc

stage: all
	$(Q)mkdir -p $(SYSROOT_BIN)
	$(Q)cp $(BINDIR)/login.bin $(SYSROOT_BIN)/login
	$(MSG_STRIP)$(STRIP) -s $(SYSROOT_BIN)/login
	$(Q)mkdir -p $(SYSROOT_SBIN)
	$(Q)cp $(BINDIR)/getty.bin $(SYSROOT_SBIN)/getty
	$(MSG_STRIP)$(STRIP) -s $(SYSROOT_SBIN)/getty

clean:
	rm -rf build $(BINDIR)
