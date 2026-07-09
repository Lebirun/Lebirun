HOST = x86_64-elf

CC = $(HOST)-gcc
LD = $(HOST)-ld

SYSROOT = ../sysroot
KERNEL_INCLUDE = ../kernel/include
OUTDIR = out

CFLAGS = -Wall -Wextra -nostdlib -ffreestanding -fno-builtin \
         -fno-stack-protector -mcmodel=kernel -fno-pic -Os \
         -ffunction-sections -fdata-sections

CPPFLAGS = -nostdinc -isystem $(SYSROOT)/usr/include -I$(KERNEL_INCLUDE)

CONFIG_FILE := $(if $(wildcard ../.config),../.config,../.config.default)

define read_bool
$(if $(filter y yes 1 true enable enabled,$(shell if [ -f "$(CONFIG_FILE)" ] && grep -q '^$(1)=' "$(CONFIG_FILE)" 2>/dev/null; then grep '^$(1)=' "$(CONFIG_FILE)" 2>/dev/null | tail -n 1 | cut -d= -f2; elif [ -f "../.config.default" ]; then grep '^$(1)=' "../.config.default" 2>/dev/null | tail -n 1 | cut -d= -f2; fi)),1,0)
endef

CONFIG_MODULE_IPV67 := $(call read_bool,MODULE_IPV67)

IPV67_SRC = \
	ipv67/state.c \
	ipv67/crypto.c \
	ipv67/context.c \
	ipv67/address.c \
	ipv67/peers.c \
	ipv67/routing.c \
	ipv67/asn.c \
	ipv67/discovery.c \
	ipv67/rx4.c \
	ipv67/rx6.c \
	ipv67/control.c \
	ipv67/syscall.c \
	ipv67/module.c

IPV67_OBJS = $(patsubst %.c,$(OUTDIR)/%.o,$(notdir $(IPV67_SRC)))

MODULES =
ifeq ($(CONFIG_MODULE_IPV67),1)
MODULES += $(OUTDIR)/ipv67.lke
endif

SYSROOT_LKE = ../root/lib/lke

all: $(OUTDIR) $(MODULES)

$(OUTDIR):
	mkdir -p $(OUTDIR)

$(OUTDIR)/%.lke: %.c
	$(CC) $(CFLAGS) $(CPPFLAGS) -c $< -o $@

$(OUTDIR)/%.o: ipv67/%.c
	$(CC) $(CFLAGS) $(CPPFLAGS) -c $< -o $@

$(OUTDIR)/ipv67.lke: $(IPV67_OBJS)
	$(LD) -r -o $@ $(IPV67_OBJS)

stage: all
	mkdir -p $(SYSROOT_LKE)
ifneq ($(strip $(MODULES)),)
	cp $(MODULES) $(SYSROOT_LKE)/
endif

clean:
	rm -rf $(OUTDIR)

.PHONY: all stage clean
