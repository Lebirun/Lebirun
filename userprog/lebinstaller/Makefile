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

CFLAGS = -Wall -Wextra -ffreestanding -fno-builtin -fno-stack-protector -fno-pic -Os -fomit-frame-pointer -nostdinc -ffunction-sections -fdata-sections
CPPFLAGS = -isystem $(LIBC_ABS)/leblibc/include -isystem $(LIBC_ABS)/leblibc/build-x86_64/obj/include -isystem $(LIBC_ABS)/leblibc/arch/x86_64 -isystem $(LIBC_ABS)/leblibc/arch/generic -I$(LIBC_ABS)/include -I$(LIBC_ABS)/src

CRT1 = $(LIBC)/leblibc/build-x86_64/lib/crt1.o
CRTI = $(LIBC)/leblibc/build-x86_64/lib/crti.o
CRTN = $(LIBC)/leblibc/build-x86_64/lib/crtn.o
LIBC_A = $(LIBC)/leblibc/build-x86_64/lib/libc.a

LD_SCRIPT = $(LIBC)/user.ld

SYSROOT_SBIN = ../../root/sbin

SRCS = src/lebinstaller.c
OBJS = $(SRCS:.c=.o)

LEB_SYSCALLS_SRC = $(LIBC)/src/syscalls.c
LEB_SYSCALLS_OBJ = src/syscalls_vfs.o
LEB_LSYSCALLS_SRC = $(LIBC)/src/leb_syscalls.c
LEB_LSYSCALLS_OBJ = src/leb_syscalls.o

.PHONY: all clean stage

all: lebinstaller.bin

%.o: %.c
	$(MSG_CC)$(CC) $(CFLAGS) $(CPPFLAGS) -c $< -o $@

$(LEB_SYSCALLS_OBJ): $(LEB_SYSCALLS_SRC)
	$(MSG_CC)$(CC) $(CFLAGS) $(CPPFLAGS) -c $< -o $@

$(LEB_LSYSCALLS_OBJ): $(LEB_LSYSCALLS_SRC)
	$(MSG_CC)$(CC) $(CFLAGS) $(CPPFLAGS) -c $< -o $@

lebinstaller.bin: $(OBJS) $(LEB_SYSCALLS_OBJ) $(LEB_LSYSCALLS_OBJ) $(CRT1) $(CRTI) $(CRTN) $(LIBC_A)
	$(MSG_LD)$(CC) -nostdlib -static -Wl,-z,noexecstack -Wl,--gc-sections -T $(LD_SCRIPT) -L$(LIBC)/leblibc/build-x86_64/lib -o $@ $(CRT1) $(CRTI) $(OBJS) $(LEB_SYSCALLS_OBJ) $(LEB_LSYSCALLS_OBJ) -lc $(CRTN) -lgcc

stage: all
	$(Q)mkdir -p $(SYSROOT_SBIN)
	$(Q)cp lebinstaller.bin $(SYSROOT_SBIN)/lebinstaller
	$(MSG_STRIP)$(STRIP) -s $(SYSROOT_SBIN)/lebinstaller

clean:
	rm -f $(OBJS) $(LEB_SYSCALLS_OBJ) $(LEB_LSYSCALLS_OBJ) lebinstaller.bin
