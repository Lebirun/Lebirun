#!/bin/sh
set -e
. ./build.sh

mkdir -p isodir
mkdir -p isodir/boot
mkdir -p isodir/boot/grub

cp sysroot/boot/lebirun.kernel isodir/boot/lebirun.kernel
cat > isodir/boot/grub/grub.cfg << EOF
menuentry "Lebirun" {
	multiboot /boot/lebirun.kernel
}
EOF
grub-mkrescue -o lebirun.iso isodir
