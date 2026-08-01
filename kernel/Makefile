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

CFLAGS?=-Oz
CPPFLAGS?=
LDFLAGS?=-Wl,--gc-sections
LIBS?=

DESTDIR?=
PREFIX?=/usr/local
EXEC_PREFIX?=$(PREFIX)
BOOTDIR?=$(EXEC_PREFIX)/boot
INCLUDEDIR?=$(PREFIX)/include

CFLAGS:=$(CFLAGS) -ffunction-sections -fdata-sections -falign-functions=1 -ffreestanding -Wall -Wextra -fdiagnostics-color=always
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

KERNEL_SENSITIVE_OBJS=\
$(ARCHDIR)/tty.o \
$(ARCHDIR)/idt.o \
$(ARCHDIR)/gdt.o \
$(ARCHDIR)/pit.o \
$(ARCHDIR)/rtc.o \
$(ARCHDIR)/timekeeping.o \
$(ARCHDIR)/mutex.o \
$(ARCHDIR)/keyboard.o \
$(ARCHDIR)/mouse.o \
$(ARCHDIR)/mem/init.o \
$(ARCHDIR)/mem/pmm.o \
$(ARCHDIR)/mem/vmm.o \
$(ARCHDIR)/mem/pt.o \
$(ARCHDIR)/mem/vmm_map.o \
$(ARCHDIR)/mem/vmm_pd.o \
$(ARCHDIR)/mem/heap.o \
$(ARCHDIR)/mem/slab.o \
$(ARCHDIR)/mem/demand.o \
$(ARCHDIR)/mem/kstack.o \
$(ARCHDIR)/mem/temp_map.o \
$(ARCHDIR)/uaccess.o \
$(ARCHDIR)/task.o \
$(ARCHDIR)/smp.o \
$(ARCHDIR)/multithread.o \
$(ARCHDIR)/syscalls/syscall_main.o \
$(ARCHDIR)/syscalls/syscall_core.o \
$(ARCHDIR)/syscalls/syscall_mem.o \
$(ARCHDIR)/syscalls/syscall_net.o \
$(ARCHDIR)/syscalls/syscall_socket.o \
$(ARCHDIR)/console.o \
$(ARCHDIR)/drivers/net/net.o \
$(ARCHDIR)/drivers/net/netif.o \
$(ARCHDIR)/drivers/net/ethernet.o \
$(ARCHDIR)/drivers/net/arp.o \
$(ARCHDIR)/drivers/net/ipv4.o \
$(ARCHDIR)/drivers/net/ipv6.o \
$(ARCHDIR)/drivers/net/icmp.o \
$(ARCHDIR)/drivers/net/icmpv6.o \
$(ARCHDIR)/drivers/net/udp.o \
$(ARCHDIR)/drivers/net/tcp.o \
$(ARCHDIR)/drivers/net/dhcp.o \
$(ARCHDIR)/drivers/net/dns.o \
$(ARCHDIR)/drivers/net/http.o \
$(ARCHDIR)/drivers/net/e1000/e1000.o \
$(ARCHDIR)/watchdog.o \
kernel/kernel.o \
kernel/launch_user.o \
lib/string.o

$(KERNEL_SENSITIVE_OBJS): CFLAGS += -Os

USER_SHELL_BIN=$(ARCHDIR)/user_shell.bin
USER_SHELL_OBJ=$(ARCHDIR)/user_shell_bin.o
CRTBEGIN:=$(shell $(CC) $(CFLAGS) $(LDFLAGS) -print-file-name=crtbegin.o)
CRTEND:=$(shell $(CC) $(CFLAGS) $(LDFLAGS) -print-file-name=crtend.o)

UNIFONT_BIN=fonts/unifont.psf
UNIFONT_GLYPHS=$(ARCHDIR)/unifont_glyphs.bin
UNIFONT_OBJ=$(ARCHDIR)/unifont_bin.o

$(UNIFONT_GLYPHS): $(wildcard $(UNIFONT_BIN))
	@if [ -f "$(UNIFONT_BIN)" ]; then \
		set -- $$(od -An -tu1 -N4 "$(UNIFONT_BIN)"); \
		test "$$1" = 54 -a "$$2" = 4 -a $$(( $$3 & 1 )) = 1 -a "$$4" = 16; \
		dd if="$(UNIFONT_BIN)" of="$@" bs=1 skip=4 count=8192 status=none; \
		test "$$(wc -c < "$@")" = 8192; \
	else \
		printf '\0' > "$@"; \
	fi

$(UNIFONT_OBJ): $(UNIFONT_GLYPHS)
	@if [ -f "$(UNIFONT_BIN)" ]; then \
		echo "Embedding font glyphs from $(UNIFONT_BIN) -> $@"; \
		$(OBJCOPY) -I binary -O elf64-x86-64 -B i386:x86-64 \
			--rename-section .data=.rodata.font,alloc,load,readonly,data,contents \
			--redefine-sym _binary_arch_x86_64_unifont_glyphs_bin_start=unifont_glyphs_start \
			--redefine-sym _binary_arch_x86_64_unifont_glyphs_bin_end=unifont_glyphs_end \
			--redefine-sym _binary_arch_x86_64_unifont_glyphs_bin_size=unifont_glyphs_size \
			$(UNIFONT_GLYPHS) $@; \
	else \
		echo "Creating stub $@ (no font present)"; \
		printf '.section .rodata.font,"a"\n.global unifont_glyphs_start\nunifont_glyphs_start:\n.byte 0\n.global unifont_glyphs_end\nunifont_glyphs_end:\n' > $(ARCHDIR)/unifont_stub.S; \
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

$(ARCHDIR)/xz/xz_crc32.o $(ARCHDIR)/xz/xz_dec_bcj.o $(ARCHDIR)/xz/xz_dec_lzma2.o $(ARCHDIR)/xz/xz_dec_stream.o: $(ARCHDIR)/xz/%.o: $(ARCHDIR)/xz/%.c
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
	rm -f $(UNIFONT_GLYPHS)
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
