DEFAULT_HOST!=../default-host.sh
HOST?=$(DEFAULT_HOST)
HOSTARCH!=../target-triplet-to-arch.sh $(HOST)
CC?=gcc
LD:=$(HOST)-ld
OBJCOPY:=$(HOST)-objcopy

CFLAGS?=-Os -ffunction-sections -fdata-sections
CPPFLAGS?=
LDFLAGS?=-Wl,--gc-sections -Wl,-s
LIBS?=

DESTDIR?=
PREFIX?=/usr/local
EXEC_PREFIX?=$(PREFIX)
BOOTDIR?=$(EXEC_PREFIX)/boot
INCLUDEDIR?=$(PREFIX)/include

CFLAGS:=$(CFLAGS) -ffreestanding -Wall -Wextra
CPPFLAGS:=$(CPPFLAGS) -D__is_kernel -Iinclude
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

USER_SHELL_BIN=$(ARCHDIR)/user_shell.bin
USER_SHELL_OBJ=$(ARCHDIR)/user_shell_bin.o

UNIFONT_BIN=fonts/unifont.psf
UNIFONT_OBJ=$(ARCHDIR)/unifont_bin.o

$(UNIFONT_OBJ): $(wildcard $(UNIFONT_BIN))
	@if [ -f "$(UNIFONT_BIN)" ]; then \
		echo "Embedding font from $(UNIFONT_BIN) -> $@"; \
		$(OBJCOPY) -I binary -O elf32-i386 -B i386 \
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
$(USER_SHELL_OBJ) \
$(UNIFONT_OBJ) \
kernel/kernel.o \
kernel/launch_user.o \
lib/string.o

OBJS=\
$(ARCHDIR)/crti.o \
$(ARCHDIR)/crtbegin.o \
$(KERNEL_OBJS) \
$(ARCHDIR)/crtend.o \
$(ARCHDIR)/crtn.o

LINK_LIST=\
$(LDFLAGS) \
$(ARCHDIR)/crti.o \
$(ARCHDIR)/crtbegin.o \
$(KERNEL_OBJS) \
$(LIBS) \
$(ARCHDIR)/crtend.o \
$(ARCHDIR)/crtn.o

.PHONY: all clean install install-headers install-kernel
.SUFFIXES: .o .c .S

all: lebirun.kernel

lebirun.kernel: $(OBJS) $(ARCHDIR)/linker.ld
	$(CC) -T $(ARCHDIR)/linker.ld -o $@ $(LINK_LIST)
	grub-file --is-x86-multiboot lebirun.kernel

$(ARCHDIR)/crtbegin.o $(ARCHDIR)/crtend.o:
	mkdir -p $(ARCHDIR)
	OBJ=`$(CC) $(CFLAGS) $(LDFLAGS) -print-file-name=$(@F)` && cp "$$OBJ" $@

.c.o:
	$(CC) -MD -c $< -o $@ -std=gnu11 $(CFLAGS) $(CPPFLAGS)

.S.o:
	$(CC) -MD -c $< -o $@ $(CFLAGS) $(CPPFLAGS)

$(USER_SHELL_BIN):
	$(MAKE) -C ../userprog
	@if [ ! -f "$@" ]; then \
		echo "Creating stub $@ (no user shell built)"; \
		printf '\0' > $@; \
	fi

$(USER_SHELL_OBJ): $(USER_SHELL_BIN)
	$(OBJCOPY) -I binary -O elf32-i386 -B i386 \
		--rename-section .data=.rodata,alloc,load,readonly,data,contents \
		--redefine-sym _binary_arch_i386_user_shell_bin_start=user_shell_bin_start \
		--redefine-sym _binary_arch_i386_user_shell_bin_end=user_shell_bin_end \
		--redefine-sym _binary_arch_i386_user_shell_bin_size=user_shell_bin_size \
		$< $@

clean:
	rm -f lebirun.kernel
	rm -f $(USER_SHELL_OBJ)
	rm -f $(USER_SHELL_BIN)
	rm -f $(UNIFONT_OBJ)
	rm -f $(OBJS) *.o */*.o */*/*.o
	rm -f $(OBJS:.o=.d) *.d */*.d */*/*.d

install-headers:
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