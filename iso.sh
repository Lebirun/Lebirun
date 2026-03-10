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
progress_bar "$CURRENT_STEP" "$TOTAL_STEPS" "Preparing ISO directory"
printf "\033[1;36mPreparing ISO directory...\033[0m\n"
mkdir -p isodir/boot/grub

KERNEL_BIN="sysroot/boot/lebirun.kernel"
if [ ! -f "$KERNEL_BIN" ]; then
    cleanup_bar
    printf "\033[1;31mError: %s not found. Did the build fail?\033[0m\n" "$KERNEL_BIN"
    exit 1
fi

CURRENT_STEP=$((CURRENT_STEP + 1))
progress_bar "$CURRENT_STEP" "$TOTAL_STEPS" "Copying kernel to ISO"
printf "\033[1;36mCopying kernel to ISO...\033[0m\n"
cp "$KERNEL_BIN" isodir/boot/lebirun.kernel

CURRENT_STEP=$((CURRENT_STEP + 1))
if [ -f initrd.img ]; then
    progress_bar "$CURRENT_STEP" "$TOTAL_STEPS" "Copying initrd to ISO"
    printf "\033[1;36mCopying initrd to ISO...\033[0m\n"
    cp initrd.img isodir/boot/initrd.img
else
    progress_bar "$CURRENT_STEP" "$TOTAL_STEPS" "Skipping initrd (not found)"
    printf "\033[0;33mSkipping initrd (not found)\033[0m\n"
fi

CURRENT_STEP=$((CURRENT_STEP + 1))
if [ -f rootfs.squashfs ]; then
    progress_bar "$CURRENT_STEP" "$TOTAL_STEPS" "Copying rootfs to ISO"
    printf "\033[1;36mCopying rootfs to ISO...\033[0m\n"
    cp rootfs.squashfs isodir/boot/rootfs.squashfs
else
    progress_bar "$CURRENT_STEP" "$TOTAL_STEPS" "Skipping rootfs (not found)"
    printf "\033[0;33mSkipping rootfs (not found)\033[0m\n"
fi

cat > isodir/boot/grub/grub.cfg << EOF
set timeout=10
set default=0

menuentry "Lebirun" {
	multiboot /boot/lebirun.kernel
	module /boot/rootfs.squashfs
	boot
}
EOF

GRUB_MODULES="multiboot biosdisk part_msdos iso9660"

CURRENT_STEP=$((CURRENT_STEP + 1))
progress_bar "$CURRENT_STEP" "$TOTAL_STEPS" "Creating ISO image"
printf "\033[1;36mCreating ISO image...\033[0m\n"
if [ "$VERBOSE" -eq 1 ]; then
    grub-mkrescue --compress=xz --install-modules="$GRUB_MODULES" --fonts="" --locales="" --themes="" -o lebirun.iso isodir 2>&1 || \
    grub-mkrescue -o lebirun.iso isodir
else
    grub-mkrescue --compress=xz --install-modules="$GRUB_MODULES" --fonts="" --locales="" --themes="" -o lebirun.iso isodir 2>/dev/null || \
    grub-mkrescue -o lebirun.iso isodir 2>/dev/null
fi

cleanup_bar
printf "\033[1;32mISO created: lebirun.iso (%s bytes)\033[0m\n" "$(stat -c%s lebirun.iso)"
