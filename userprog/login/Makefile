CC = i686-elf-gcc
STRIP = i686-elf-strip

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

CFLAGS = -Wall -Wextra -ffreestanding -fno-builtin -fno-stack-protector -fno-pic -m32 -Os -fomit-frame-pointer -nostdinc -ffunction-sections -fdata-sections
CPPFLAGS = -isystem $(LIBC_ABS)/leblibc/include -isystem $(LIBC_ABS)/leblibc/build-i386/obj/include -isystem $(LIBC_ABS)/leblibc/arch/i386 -isystem $(LIBC_ABS)/leblibc/arch/generic -I$(LIBC_ABS)/include -I$(LIBC_ABS)/src

CRT1 = $(LIBC)/leblibc/build-i386/lib/crt1.o
CRTI = $(LIBC)/leblibc/build-i386/lib/crti.o
CRTN = $(LIBC)/leblibc/build-i386/lib/crtn.o
LIBC_A = $(LIBC)/leblibc/build-i386/lib/libc.a

LD_SCRIPT = $(LIBC)/user.ld

SYSROOT_BIN = ../../root/bin
SYSROOT_SBIN = ../../root/sbin

LOGIN_SRCS = src/login.c
LOGIN_OBJS = $(LOGIN_SRCS:.c=.o)

GETTY_SRCS = src/getty.c
GETTY_OBJS = $(GETTY_SRCS:.c=.o)

.PHONY: all clean stage

all: login.bin getty.bin

%.o: %.c
	$(MSG_CC)$(CC) $(CFLAGS) $(CPPFLAGS) -c $< -o $@

login.bin: $(LOGIN_OBJS) $(CRT1) $(CRTI) $(CRTN) $(LIBC_A)
	$(MSG_LD)$(CC) -nostdlib -static -Wl,-z,noexecstack -Wl,--gc-sections -T $(LD_SCRIPT) -L$(LIBC)/leblibc/build-i386/lib -o $@ $(CRT1) $(CRTI) $(LOGIN_OBJS) -lc $(CRTN) -lgcc

getty.bin: $(GETTY_OBJS) $(CRT1) $(CRTI) $(CRTN) $(LIBC_A)
	$(MSG_LD)$(CC) -nostdlib -static -Wl,-z,noexecstack -Wl,--gc-sections -T $(LD_SCRIPT) -L$(LIBC)/leblibc/build-i386/lib -o $@ $(CRT1) $(CRTI) $(GETTY_OBJS) -lc $(CRTN) -lgcc

stage: all
	$(Q)mkdir -p $(SYSROOT_BIN)
	$(Q)cp login.bin $(SYSROOT_BIN)/login
	$(MSG_STRIP)$(STRIP) -s $(SYSROOT_BIN)/login
	$(Q)mkdir -p $(SYSROOT_SBIN)
	$(Q)cp getty.bin $(SYSROOT_SBIN)/getty
	$(MSG_STRIP)$(STRIP) -s $(SYSROOT_SBIN)/getty

clean:
	rm -f $(LOGIN_OBJS) $(GETTY_OBJS) login.bin getty.bin
