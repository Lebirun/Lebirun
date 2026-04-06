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

CFLAGS = -Wall -Wextra -ffreestanding -fno-builtin -fno-stack-protector -fno-pic -Os -fomit-frame-pointer -nostdinc -ffunction-sections -fdata-sections
CPPFLAGS = -isystem $(LIBC_ABS)/leblibc/include -isystem $(abspath $(SYSROOT))/usr/include -isystem $(LIBC_ABS)/leblibc/arch/x86_64 -isystem $(LIBC_ABS)/leblibc/arch/generic -I$(LIBC_ABS)/include -I$(LIBC_ABS)/src

CRT1 = $(SYSROOT)/usr/lib/crt1.o
CRTI = $(SYSROOT)/usr/lib/crti.o
CRTN = $(SYSROOT)/usr/lib/crtn.o
LIBC_A = $(SYSROOT)/usr/lib/libc.a

LD_SCRIPT = $(LIBC)/user.ld

SYSROOT_SBIN = ../../root/sbin

SRCS = src/lebinstaller.c
OBJS = $(patsubst src/%.c,build/%.o,$(SRCS))

LEB_SYSCALLS_SRC = $(LIBC)/src/syscalls.c
LEB_SYSCALLS_OBJ = build/syscalls_vfs.o
LEB_LSYSCALLS_SRC = $(LIBC)/src/leb_syscalls.c
LEB_LSYSCALLS_OBJ = build/leb_syscalls.o

BINDIR = bin

.PHONY: all clean stage

all: $(BINDIR)/lebinstaller.bin

build/%.o: src/%.c
	$(Q)mkdir -p $(dir $@)
	$(MSG_CC)$(CC) $(CFLAGS) $(CPPFLAGS) -c $< -o $@

$(LEB_SYSCALLS_OBJ): $(LEB_SYSCALLS_SRC)
	$(Q)mkdir -p $(dir $@)
	$(MSG_CC)$(CC) $(CFLAGS) $(CPPFLAGS) -c $< -o $@

$(LEB_LSYSCALLS_OBJ): $(LEB_LSYSCALLS_SRC)
	$(Q)mkdir -p $(dir $@)
	$(MSG_CC)$(CC) $(CFLAGS) $(CPPFLAGS) -c $< -o $@

$(BINDIR)/lebinstaller.bin: $(OBJS) $(LEB_SYSCALLS_OBJ) $(LEB_LSYSCALLS_OBJ) $(CRT1) $(CRTI) $(CRTN) $(LIBC_A)
	$(Q)mkdir -p $(BINDIR)
	$(MSG_LD)$(CC) -nostdlib -static -Wl,-z,noexecstack -Wl,--gc-sections -T $(LD_SCRIPT) -L$(SYSROOT)/usr/lib -o $@ $(CRT1) $(CRTI) $(OBJS) $(LEB_SYSCALLS_OBJ) $(LEB_LSYSCALLS_OBJ) -lc $(CRTN) -lgcc

stage: all
	$(Q)mkdir -p $(SYSROOT_SBIN)
	$(Q)cp $(BINDIR)/lebinstaller.bin $(SYSROOT_SBIN)/lebinstaller
	$(MSG_STRIP)$(STRIP) -s $(SYSROOT_SBIN)/lebinstaller

clean:
	rm -rf build $(BINDIR)
