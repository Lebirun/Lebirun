DEFAULT_HOST!=../default-host.sh
HOST?=$(DEFAULT_HOST)
HOSTARCH!=../target-triplet-to-arch.sh $(HOST)
CC?=gcc
LD:=$(HOST)-ld
OBJCOPY:=$(HOST)-objcopy

CFLAGS?=-O2 -g
CPPFLAGS?=
LDFLAGS?=
LIBS?=

DESTDIR?=
PREFIX?=/usr/local
EXEC_PREFIX?=$(PREFIX)
BOOTDIR?=$(EXEC_PREFIX)/boot
INCLUDEDIR?=$(PREFIX)/include

LIBC_COMMON=../libc-common

CFLAGS:=$(CFLAGS) -ffreestanding -Wall -Wextra
CPPFLAGS:=$(CPPFLAGS) -D__is_kernel -Iinclude -I$(LIBC_COMMON)/include
LDFLAGS:=$(LDFLAGS)
LIBS:=$(LIBS) -nostdlib -lk -lgcc

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
$(LIBC_COMMON)/string/memset.o \
$(LIBC_COMMON)/string/memcpy.o \
$(LIBC_COMMON)/string/memmove.o \
$(LIBC_COMMON)/string/memcmp.o \
$(LIBC_COMMON)/string/strlen.o \
$(LIBC_COMMON)/string/strcmp.o \
$(LIBC_COMMON)/string/strncmp.o

$(LIBC_COMMON)/string/memset.o:
	$(MAKE) -C $(LIBC_COMMON) string/memset.o

$(LIBC_COMMON)/string/memcpy.o:
	$(MAKE) -C $(LIBC_COMMON) string/memcpy.o

$(LIBC_COMMON)/string/memmove.o:
	$(MAKE) -C $(LIBC_COMMON) string/memmove.o

$(LIBC_COMMON)/string/memcmp.o:
	$(MAKE) -C $(LIBC_COMMON) string/memcmp.o

$(LIBC_COMMON)/string/strlen.o:
	$(MAKE) -C $(LIBC_COMMON) string/strlen.o

$(LIBC_COMMON)/string/strcmp.o:
	$(MAKE) -C $(LIBC_COMMON) string/strcmp.o

$(LIBC_COMMON)/string/strncmp.o:
	$(MAKE) -C $(LIBC_COMMON) string/strncmp.o

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

install: install-headers install-kernel

install-headers:
	mkdir -p $(DESTDIR)$(INCLUDEDIR)
	cp -R --preserve=timestamps include/. $(DESTDIR)$(INCLUDEDIR)/.

install-kernel: lebirun.kernel
	mkdir -p $(DESTDIR)$(BOOTDIR)
	cp lebirun.kernel $(DESTDIR)$(BOOTDIR)

KERNEL_OWN_OBJS = $(filter-out $(LIBC_COMMON)/%,$(OBJS))
-include $(KERNEL_OWN_OBJS:.o=.d)