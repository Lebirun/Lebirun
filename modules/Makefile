HOST = x86_64-elf

CC = $(HOST)-gcc

SYSROOT = ../sysroot
KERNEL_INCLUDE = ../kernel/include
OUTDIR = out

CFLAGS = -Wall -Wextra -nostdlib -ffreestanding -fno-builtin \
         -fno-stack-protector -mcmodel=kernel -fno-pic -Os \
         -ffunction-sections -fdata-sections

CPPFLAGS = -nostdinc -isystem $(SYSROOT)/usr/include -I$(KERNEL_INCLUDE)

MODULES = $(OUTDIR)/LkeTest.lke

SYSROOT_LKE = ../root/lib/lke

all: $(OUTDIR) $(MODULES)

$(OUTDIR):
	mkdir -p $(OUTDIR)

$(OUTDIR)/%.lke: %.c
	$(CC) $(CFLAGS) $(CPPFLAGS) -c $< -o $@

stage: all
	mkdir -p $(SYSROOT_LKE)
	cp $(MODULES) $(SYSROOT_LKE)/

clean:
	rm -rf $(OUTDIR)

.PHONY: all stage clean
