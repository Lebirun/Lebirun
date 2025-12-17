#!/bin/sh
set -e
. ./build.sh

mkdir -p isodir
mkdir -p isodir/boot
mkdir -p isodir/boot/grub

cp sysroot/boot/lebirun.kernel isodir/boot/lebirun.kernel

if [ -d initrd ]; then
    echo "Building initrd.img from initrd/ directory..."
    chmod +x mkinitrd.sh
    ./mkinitrd.sh initrd initrd.img
fi

if [ -f initrd.img ]; then
    cp initrd.img isodir/boot/initrd.img
    echo "Copied initrd.img to ISO ($(stat -c%s initrd.img) bytes)"
else
    echo "Warning: no initrd.img found; continuing without initrd module"
fi
cat > isodir/boot/grub/grub.cfg << EOF
menuentry "Lebirun" {
	multiboot /boot/lebirun.kernel
	module /boot/initrd.img
}
EOF
grub-mkrescue -o lebirun.iso isodir
