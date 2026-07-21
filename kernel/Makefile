DEFAULT_HOST!=../default-host.sh
HOST?=$(DEFAULT_HOST)
HOSTARCH!=../target-triplet-to-arch.sh $(HOST)
CC=$(HOST)-gcc
LD:=$(HOST)-ld
OBJCOPY:=$(HOST)-objcopy

VERSION_FILE:=../VERSION
VERSION_HEADER:=include/lebirun/version.h
OS_VERSION:=$(strip $(shell sed -n '1{s/[[:space:]]//g;p;}' $(VERSION_FILE) 2>/dev/null))
ifeq ($(OS_VERSION),)
$(error VERSION is missing or empty)
endif

V ?= 0
ifeq ($(V),0)
  Q = @
  MSG_CC    = @printf '  CC      %s\n' $<;
  MSG_AS    = @printf '  AS      %s\n' $<;
  MSG_LD    = @printf '  LD      %s\n' $@;
  MSG_STRIP = @printf '  STRIP   %s\n' $@;
else
  Q =
  MSG_CC =
  MSG_AS =
  MSG_LD =
  MSG_STRIP =
endif

CFLAGS?=-Os -ffunction-sections -fdata-sections
CPPFLAGS?=
LDFLAGS?=-Wl,--gc-sections
LIBS?=

DESTDIR?=
PREFIX?=/usr/local
EXEC_PREFIX?=$(PREFIX)
BOOTDIR?=$(EXEC_PREFIX)/boot
INCLUDEDIR?=$(PREFIX)/include

CFLAGS:=$(CFLAGS) -falign-functions=1 -ffreestanding -Wall -Wextra -fdiagnostics-color=always
CPPFLAGS:=$(CPPFLAGS) -nostdinc -D__is_kernel -Iinclude -isystem ../sysroot/usr/include
LDFLAGS:=$(LDFLAGS)
LIBS:=$(LIBS) -nostdlib -lgcc

BUILD_TZ:=$(shell date +%z)
CPPFLAGS:=$(CPPFLAGS) -DKERNEL_BUILD_TZ=\"$(BUILD_TZ)\"

include lebconfig.mk

ARCHDIR=arch/$(HOSTARCH)

include $(ARCHDIR)/make.config

CFLAGS:=$(CFLAGS) $(KERNEL_ARCH_CFLAGS)
CPPFLAGS:=$(CPPFLAGS) $(KERNEL_ARCH_CPPFLAGS)
LDFLAGS:=$(LDFLAGS) $(KERNEL_ARCH_LDFLAGS)
LIBS:=$(LIBS) $(KERNEL_ARCH_LIBS)

KERNEL_SIZE_CFLAGS=-Os -ffunction-sections -fdata-sections

$(ARCHDIR)/fs/misc/procfs.o \
$(ARCHDIR)/fs/misc/sysfs.o \
$(ARCHDIR)/fs/misc/devfs.o \
$(ARCHDIR)/fs/misc/overlayfs.o \
$(ARCHDIR)/fs/squashfs.o \
$(ARCHDIR)/fs/iso9660.o \
$(ARCHDIR)/fs/partition.o \
$(ARCHDIR)/fs/ext4/ext4.o \
$(ARCHDIR)/fs/ext4/ext4_super.o \
$(ARCHDIR)/fs/ext4/ext4_block.o \
$(ARCHDIR)/fs/ext4/ext4_inode.o \
$(ARCHDIR)/fs/ext4/ext4_dir.o \
$(ARCHDIR)/fs/ext4/ext4_file.o \
$(ARCHDIR)/initrd.o \
$(ARCHDIR)/ramfs.o \
$(ARCHDIR)/vfs.o \
$(ARCHDIR)/elf.o \
$(ARCHDIR)/evdev.o \
$(ARCHDIR)/pty.o \
$(ARCHDIR)/framebuffer.o \
$(ARCHDIR)/psf.o \
$(ARCHDIR)/panic.o \
$(ARCHDIR)/vring.o \
$(ARCHDIR)/lke.o \
$(ARCHDIR)/vfl.o \
$(ARCHDIR)/xz/xz.o \
$(ARCHDIR)/xz/xz_crc32.o \
$(ARCHDIR)/xz/xz_dec_lzma2.o \
$(ARCHDIR)/xz/xz_dec_stream.o \
$(ARCHDIR)/drivers/sata/ahci.o \
$(ARCHDIR)/drivers/fb/bga.o \
$(ARCHDIR)/drivers/fb/vga_modes.o \
$(ARCHDIR)/syscalls/syscall_process.o \
$(ARCHDIR)/syscalls/syscall_time.o \
$(ARCHDIR)/syscalls/syscall_initrd.o \
$(ARCHDIR)/syscalls/syscall_fb.o \
$(ARCHDIR)/syscalls/syscall_console.o \
$(ARCHDIR)/syscalls/syscall_sata.o \
$(ARCHDIR)/syscalls/syscall_termios.o \
$(ARCHDIR)/syscalls/syscall_posix.o \
$(ARCHDIR)/syscalls/syscall_select.o \
$(ARCHDIR)/syscalls/syscall_vfs.o \
$(ARCHDIR)/syscalls/syscall_at.o \
$(ARCHDIR)/syscalls/syscall_signal.o \
$(ARCHDIR)/syscalls/syscall_ids.o \
$(ARCHDIR)/syscalls/syscall_misc.o \
$(ARCHDIR)/syscalls/syscall_epoll.o \
$(ARCHDIR)/syscalls/syscall_pthread.o \
$(ARCHDIR)/syscalls/syscall_shm.o \
$(ARCHDIR)/syscalls/syscall_dl.o \
$(ARCHDIR)/syscalls/syscall_power.o \
$(ARCHDIR)/syscalls/syscall_crypto.o \
$(ARCHDIR)/syscalls/syscall_regex.o: CFLAGS += $(KERNEL_SIZE_CFLAGS)

USER_SHELL_BIN=$(ARCHDIR)/user_shell.bin
USER_SHELL_OBJ=$(ARCHDIR)/user_shell_bin.o
CRTBEGIN:=$(shell $(CC) $(CFLAGS) $(LDFLAGS) -print-file-name=crtbegin.o)
CRTEND:=$(shell $(CC) $(CFLAGS) $(LDFLAGS) -print-file-name=crtend.o)

UNIFONT_BIN=fonts/unifont.psf
UNIFONT_OBJ=$(ARCHDIR)/unifont_bin.o

$(UNIFONT_OBJ): $(wildcard $(UNIFONT_BIN))
	@if [ -f "$(UNIFONT_BIN)" ]; then \
		echo "Embedding font from $(UNIFONT_BIN) -> $@"; \
		$(OBJCOPY) -I binary -O elf64-x86-64 -B i386:x86-64 \
			--rename-section .data=.rodata,alloc,load,readonly,data,contents \
			--redefine-sym _binary_fonts_unifont_psf_start=unifont_psf_start \
			--redefine-sym _binary_fonts_unifont_psf_end=unifont_psf_end \
			--redefine-sym _binary_fonts_unifont_psf_size=unifont_psf_size \
			$(UNIFONT_BIN) $@; \
	else \
		echo "Creating stub $@ (no font present)"; \
		printf '.section .rodata\n.global unifont_psf_start\nunifont_psf_start:\n.byte 0\n.global unifont_psf_end\nunifont_psf_end:\n' > $(ARCHDIR)/unifont_stub.S; \
		$(CC) -c $(ARCHDIR)/unifont_stub.S -o $@; \
		rm -f $(ARCHDIR)/unifont_stub.S; \
	fi

KERNEL_OBJS=\
$(KERNEL_ARCH_OBJS) \
$(UNIFONT_OBJ) \
kernel/kernel.o \
kernel/launch_user.o \
lib/string.o \
crypto/sha256.o \
crypto/sha512.o \
crypto/hmac.o \
crypto/rng.o

OBJS=\
$(ARCHDIR)/crti.o \
$(KERNEL_OBJS) \
$(ARCHDIR)/crtn.o

LINK_LIST=\
$(LDFLAGS) \
$(ARCHDIR)/crti.o \
$(CRTBEGIN) \
$(KERNEL_OBJS) \
$(LIBS) \
$(CRTEND) \
$(ARCHDIR)/crtn.o

.PHONY: all clean install install-headers install-kernel
.SUFFIXES: .o .c .S

NM:=$(HOST)-nm

all: lebirun.kernel

$(VERSION_HEADER): $(VERSION_FILE)
	$(Q)printf '#ifndef LEBIRUN_VERSION_H\n#define LEBIRUN_VERSION_H\n\n#define OS_VERSION "%s"\n\n#endif\n' "$(OS_VERSION)" > $@.tmp
	$(Q)if ! cmp -s $@.tmp $@; then mv $@.tmp $@; else rm -f $@.tmp; fi

lebirun.kernel: $(OBJS) $(ARCHDIR)/linker.ld $(CONFIG_FILE)
	$(MSG_LD)$(CC) -T $(ARCHDIR)/linker.ld -o $@ $(LINK_LIST)
	$(Q)cp $@ lebirun_debug.kernel
	$(MSG_STRIP)$(OBJCOPY) --strip-all $@
	$(Q)grub-file --is-x86-multiboot2 lebirun.kernel

.c.o:
	$(MSG_CC)$(CC) -MD -c $< -o $@ -std=gnu11 $(CFLAGS) $(CPPFLAGS)

.S.o:
	$(MSG_AS)$(CC) -MD -c $< -o $@ $(CFLAGS) $(CPPFLAGS)

$(ARCHDIR)/xz/xz_crc32.o $(ARCHDIR)/xz/xz_dec_lzma2.o $(ARCHDIR)/xz/xz_dec_stream.o: $(ARCHDIR)/xz/%.o: $(ARCHDIR)/xz/%.c
	$(MSG_CC)$(CC) -MD -c $< -o $@ -std=gnu11 $(CFLAGS) $(CPPFLAGS) -Wno-unused-parameter -Wno-sign-compare

$(OBJS): Makefile $(CONFIG_FILE) $(VERSION_HEADER)

$(USER_SHELL_BIN):
	$(MAKE) -C ../userprog
	@if [ ! -f "$@" ]; then \
		echo "Creating stub $@ (no user shell built)"; \
		printf '\0' > $@; \
	fi

$(USER_SHELL_OBJ): $(USER_SHELL_BIN)
	$(OBJCOPY) -I binary -O elf64-x86-64 -B i386:x86-64 \
		--rename-section .data=.rodata,alloc,load,readonly,data,contents \
		--redefine-sym _binary_arch_x86_64_user_shell_bin_start=user_shell_bin_start \
		--redefine-sym _binary_arch_x86_64_user_shell_bin_end=user_shell_bin_end \
		--redefine-sym _binary_arch_x86_64_user_shell_bin_size=user_shell_bin_size \
		$< $@

clean:
	rm -f lebirun.kernel lebirun_debug.kernel
	rm -f $(USER_SHELL_OBJ)
	rm -f $(USER_SHELL_BIN)
	rm -f $(UNIFONT_OBJ)
	rm -f $(VERSION_HEADER)
	rm -f $(OBJS) *.o */*.o */*/*.o
	rm -f $(OBJS:.o=.d) *.d */*.d */*/*.d
	$(MAKE) -C crypto clean

install-headers: $(VERSION_HEADER)
	@if [ -z "$(DESTDIR)" ]; then \
		echo "DESTDIR not set; skipping install-headers"; \
		exit 0; \
	fi; \
	mkdir -p $(DESTDIR)$(INCLUDEDIR); \
	cp -R include/. $(DESTDIR)$(INCLUDEDIR)/.

install: install-headers install-kernel

install-kernel: lebirun.kernel
	@if [ -z "$(DESTDIR)" ]; then \
		echo "DESTDIR not set; skipping install-kernel"; \
		exit 0; \
	fi
	mkdir -p $(DESTDIR)$(BOOTDIR)
	cp lebirun.kernel $(DESTDIR)$(BOOTDIR)

-include $(OBJS:.o=.d)
