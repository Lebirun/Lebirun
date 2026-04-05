#!/bin/sh
set -e

VERBOSE=0
BUILD_ARGS=""
for arg in "$@"; do
  case "$arg" in
    -v|--verbose) VERBOSE=1; BUILD_ARGS="$BUILD_ARGS -v" ;;
  esac
done

. ./build.sh $BUILD_ARGS

TOTAL_STEPS=5
CURRENT_STEP=0

CURRENT_STEP=$((CURRENT_STEP + 1))
bar_print "$(printf '\033[1;36mPreparing ISO directory...\033[0m')"
progress_bar "$CURRENT_STEP" "$TOTAL_STEPS" "Preparing ISO directory"
rm -f isodir/boot/initrd.img isodir/boot/lebirun.kernel isodir/boot/rootfs.squashfs
mkdir -p isodir/boot/grub

KERNEL_BIN="sysroot/boot/lebirun.kernel"
if [ ! -f "$KERNEL_BIN" ]; then
    cleanup_bar
    printf "\033[1;31mError: %s not found. Did the build fail?\033[0m\n" "$KERNEL_BIN"
    exit 1
fi

CURRENT_STEP=$((CURRENT_STEP + 1))
bar_print "$(printf '\033[1;36mCopying kernel to ISO...\033[0m')"
progress_bar "$CURRENT_STEP" "$TOTAL_STEPS" "Copying kernel to ISO"
cp "$KERNEL_BIN" isodir/boot/lebirun.kernel

CURRENT_STEP=$((CURRENT_STEP + 1))
if [ -f rootfs.squashfs ]; then
    bar_print "$(printf '\033[1;36mCopying rootfs to ISO...\033[0m')"
    progress_bar "$CURRENT_STEP" "$TOTAL_STEPS" "Copying rootfs to ISO"
    cp rootfs.squashfs isodir/boot/rootfs.squashfs
else
    bar_print "$(printf '\033[0;33mSkipping rootfs (not found)\033[0m')"
    progress_bar "$CURRENT_STEP" "$TOTAL_STEPS" "Skipping rootfs (not found)"
fi

CURRENT_STEP=$((CURRENT_STEP + 1))
bar_print "$(printf '\033[1;36mWriting GRUB config...\033[0m')"
progress_bar "$CURRENT_STEP" "$TOTAL_STEPS" "Writing GRUB config"
cat > isodir/boot/grub/grub.cfg << EOF
set timeout=10
set default=0

menuentry "Lebirun" {
	multiboot2 /boot/lebirun.kernel
	module2 /boot/rootfs.squashfs
	boot
}
EOF

GRUB_MODULES="multiboot2 biosdisk part_msdos iso9660"

CURRENT_STEP=$((CURRENT_STEP + 1))
bar_print "$(printf '\033[1;36mCreating ISO image...\033[0m')"
progress_bar "$CURRENT_STEP" "$TOTAL_STEPS" "Creating ISO image"
if [ "$VERBOSE" -eq 1 ]; then
    grub-mkrescue --compress=xz --install-modules="$GRUB_MODULES" --fonts="" --locales="" --themes="" -o lebirun.iso isodir 2>&1 || \
    grub-mkrescue -o lebirun.iso isodir
else
    grub-mkrescue --compress=xz --install-modules="$GRUB_MODULES" --fonts="" --locales="" --themes="" -o lebirun.iso isodir 2>/dev/null || \
    grub-mkrescue -o lebirun.iso isodir 2>/dev/null
fi

cleanup_bar
printf "\033[1;32mISO created: lebirun.iso (%s bytes)\033[0m\n" "$(stat -c%s lebirun.iso)"
