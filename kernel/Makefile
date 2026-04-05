DEFAULT_HOST!=../default-host.sh
HOST?=$(DEFAULT_HOST)
HOSTARCH!=../target-triplet-to-arch.sh $(HOST)
CC?=gcc
LD:=$(HOST)-ld
OBJCOPY:=$(HOST)-objcopy

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

CFLAGS:=$(CFLAGS) -ffreestanding -Wall -Wextra -fdiagnostics-color=always
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
KSYM_AUTO_C=$(ARCHDIR)/ksym_auto.c
KSYM_AUTO_O=$(ARCHDIR)/ksym_auto.o
KSYM_STUB_O=$(ARCHDIR)/ksym_stub.o

all: lebirun.kernel

lebirun.kernel: $(OBJS) $(KSYM_STUB_O) $(ARCHDIR)/linker.ld $(CONFIG_FILE)
	$(Q)$(CC) -T $(ARCHDIR)/linker.ld -o lebirun_pass1.elf $(LINK_LIST) $(KSYM_STUB_O)
	$(Q)./gen_ksyms.sh lebirun_pass1.elf $(KSYM_AUTO_C) $(NM)
	$(Q)$(CC) -MD -c $(KSYM_AUTO_C) -o $(KSYM_AUTO_O) -std=gnu11 $(CFLAGS) $(CPPFLAGS)
	$(MSG_LD)$(CC) -T $(ARCHDIR)/linker.ld -o $@ $(LINK_LIST) $(KSYM_AUTO_O)
	$(Q)cp $@ lebirun_debug.kernel
	$(MSG_STRIP)$(OBJCOPY) --strip-all $@
	$(Q)grub-file --is-x86-multiboot2 lebirun.kernel

.c.o:
	$(MSG_CC)$(CC) -MD -c $< -o $@ -std=gnu11 $(CFLAGS) $(CPPFLAGS)

.S.o:
	$(MSG_AS)$(CC) -MD -c $< -o $@ $(CFLAGS) $(CPPFLAGS)

$(WOLFSSL_DIR)/%.o: $(WOLFSSL_DIR)/%.c
	@mkdir -p $(dir $@)
	$(MSG_CC)$(CC) -MD -c $< -o $@ -std=gnu11 $(CFLAGS) $(CPPFLAGS) $(WOLFSSL_CPPFLAGS)

$(ARCHDIR)/drivers/net/tls.o: $(ARCHDIR)/drivers/net/tls.c
	$(MSG_CC)$(CC) -MD -c $< -o $@ -std=gnu11 $(CFLAGS) $(CPPFLAGS) $(WOLFSSL_CPPFLAGS)

$(ARCHDIR)/xz/xz_crc32.o $(ARCHDIR)/xz/xz_dec_lzma2.o $(ARCHDIR)/xz/xz_dec_stream.o: $(ARCHDIR)/xz/%.o: $(ARCHDIR)/xz/%.c
	$(MSG_CC)$(CC) -MD -c $< -o $@ -std=gnu11 $(CFLAGS) $(CPPFLAGS) -Wno-unused-parameter -Wno-sign-compare

$(ARCHDIR)/wolfssl_glue.o: $(ARCHDIR)/wolfssl_glue.c
	$(MSG_CC)$(CC) -MD -c $< -o $@ -std=gnu11 $(CFLAGS) $(CPPFLAGS) $(WOLFSSL_CPPFLAGS)

$(OBJS): $(CONFIG_FILE)

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
	rm -f lebirun.kernel lebirun_debug.kernel lebirun_pass1.elf
	rm -f $(KSYM_AUTO_C) $(KSYM_AUTO_O) $(KSYM_STUB_O)
	rm -f $(USER_SHELL_OBJ)
	rm -f $(USER_SHELL_BIN)
	rm -f $(UNIFONT_OBJ)
	rm -f $(OBJS) *.o */*.o */*/*.o
	rm -f $(OBJS:.o=.d) *.d */*.d */*/*.d
	rm -f $(WOLFSSL_DIR)/wolfcrypt/src/*.o $(WOLFSSL_DIR)/wolfcrypt/src/*.d
	rm -f $(WOLFSSL_DIR)/src/*.o $(WOLFSSL_DIR)/src/*.d
	$(MAKE) -C crypto clean

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