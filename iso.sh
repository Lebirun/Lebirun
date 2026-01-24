#!/bin/sh
set -e
. ./build.sh

mkdir -p isodir/boot/grub

KERNEL_BIN="sysroot/boot/lebirun.kernel"
if [ ! -f "$KERNEL_BIN" ]; then
    echo "Error: $KERNEL_BIN not found. Did the build fail?"
    exit 1
fi

cp "$KERNEL_BIN" isodir/boot/lebirun.kernel

if [ -f initrd.img ]; then
    cp initrd.img isodir/boot/initrd.img
    echo "Copied initrd.img to ISO ($(stat -c%s initrd.img) bytes)"
else
    echo "Warning: no initrd.img found; continuing without initrd module"
fi

if [ -f rootfs.img ]; then
    cp rootfs.img isodir/boot/rootfs.img
    echo "Copied rootfs.img to ISO ($(stat -c%s rootfs.img) bytes)"
else
    echo "Warning: no rootfs.img found; continuing without rootfs module"
fi

cat > isodir/boot/grub/grub.cfg << EOF
set timeout=20
set default=0

menuentry "Lebirun" {
	multiboot /boot/lebirun.kernel
	module /boot/initrd.img
	module /boot/rootfs.img
	boot
}
EOF

# Optimize GRUB with minimal modules for smaller ISO
# Only include essential modules for multiboot booting
GRUB_MODULES="multiboot biosdisk part_msdos iso9660"

grub-mkrescue --compress=xz --install-modules="$GRUB_MODULES" --fonts="" --locales="" --themes="" -o lebirun.iso isodir 2>/dev/null || \
grub-mkrescue -o lebirun.iso isodir

echo "ISO image created: lebirun.iso ($(stat -c%s lebirun.iso) bytes)"
