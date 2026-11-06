HOST = x86_64-elf

CC = $(HOST)-gcc
LD = $(HOST)-ld

SYSROOT = ../sysroot
KERNEL_INCLUDE = ../kernel/include
KERNEL_ARCH = ../kernel/arch/x86_64
OUTDIR = out

CFLAGS = -Wall -Wextra -nostdlib -ffreestanding -fno-builtin \
         -fno-stack-protector -mcmodel=kernel -fno-pic -Os \
         -mno-red-zone -mno-mmx -mno-sse -mno-sse2 \
         -ffunction-sections -fdata-sections

CPPFLAGS = -nostdinc -isystem $(SYSROOT)/usr/include -I$(KERNEL_INCLUDE)

include ../kernel/lebconfig.mk

CONFIG_MODULE_LEBTLS := $(call read_bool,MODULE_LEBTLS)

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

DRV = $(OUTDIR)/kernel
ahci_OBJS = $(DRV)/drivers/sata/ahci.o
nvme_OBJS = $(DRV)/drivers/nvme/nvme.o
xhci_OBJS = $(DRV)/drivers/usb/xhci.o \
	$(if $(filter 1,$(CONFIG_DRIVER_USB_HID_KBD)),$(DRV)/drivers/usb/hid-kbd.o) \
	$(if $(filter 1,$(CONFIG_DRIVER_USB_HID_MOUSE)),$(DRV)/drivers/usb/hid-mouse.o) \
	$(if $(filter 1,$(CONFIG_DRIVER_USB_STORAGE)),$(DRV)/drivers/usb/usb-storage.o)
i2c_OBJS = $(DRV)/acpi_scan.o $(DRV)/drivers/i2c/dw-i2c.o $(DRV)/drivers/i2c/i2c-hid.o
e1000_OBJS = $(DRV)/drivers/net/e1000/e1000.o
virtio_net_OBJS = $(DRV)/drivers/net/virtio_net/virtio_net.o

DRIVER_MODULES = \
	$(if $(filter 1,$(CONFIG_DRIVER_AHCI_MODULE)),ahci) \
	$(if $(filter 1,$(CONFIG_DRIVER_NVME_MODULE)),nvme) \
	$(if $(filter 1,$(CONFIG_DRIVER_XHCI_MODULE)),xhci) \
	$(if $(filter 1,$(CONFIG_DRIVER_DW_I2C_MODULE)),$(if $(filter 1,$(CONFIG_DRIVER_I2C_HID)),i2c)) \
	$(if $(filter 1,$(CONFIG_DRIVER_NET_E1000_MODULE)),e1000) \
	$(if $(filter 1,$(CONFIG_DRIVER_NET_VIRTIO_MODULE)),virtio_net)
DRIVER_LKES = $(patsubst %,$(OUTDIR)/%.lke,$(strip $(DRIVER_MODULES)))
DRIVER_OBJS = $(foreach m,$(DRIVER_MODULES),$($(m)_OBJS))

MODULES =
ifeq ($(CONFIG_MODULE_IPV67),1)
MODULES += $(OUTDIR)/ipv67.lke
endif
ifeq ($(CONFIG_MODULE_LEBTLS),1)
MODULES += $(OUTDIR)/lebtls.lke
endif
MODULES += $(DRIVER_LKES)

ALL_LKES = ipv67 lebtls ahci nvme xhci i2c e1000 virtio_net

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

LEBTLS_SRC = \
	lebtls/lebtls.c \
	lebtls/record.c \
	lebtls/hkdf.c \
	lebtls/handshake.c \
	lebtls/traffic.c \
	lebtls/aead.c \
	lebtls/x25519.c \
	lebtls/p256.c \
	lebtls/p384.c \
	lebtls/verify.c \
	lebtls/ca.c

LEBTLS_OBJS = $(patsubst %.c,$(OUTDIR)/%.o,$(notdir $(LEBTLS_SRC)))

$(OUTDIR)/%.o: lebtls/%.c
	$(CC) $(CFLAGS) $(CPPFLAGS) -c $< -o $@

$(OUTDIR)/lebtls.lke: $(LEBTLS_OBJS)
	$(LD) -r -o $@ $(LEBTLS_OBJS)

$(DRV)/%.o: $(KERNEL_ARCH)/%.c $(CONFIG_FILE)
	@mkdir -p $(dir $@)
	$(CC) -MD $(CFLAGS) -Oz -fno-asynchronous-unwind-tables $(CPPFLAGS) -c $< -o $@

.SECONDEXPANSION:
$(DRIVER_LKES): $(OUTDIR)/%.lke: $$($$*_OBJS)
	$(LD) -r -o $@ $^

stage: all
	mkdir -p $(SYSROOT_LKE)
	rm -f $(patsubst %,$(SYSROOT_LKE)/%.lke,$(ALL_LKES))
ifneq ($(strip $(MODULES)),)
	cp $(MODULES) $(SYSROOT_LKE)/
endif

clean:
	rm -rf $(OUTDIR)

-include $(DRIVER_OBJS:.o=.d)

.PHONY: all stage clean
