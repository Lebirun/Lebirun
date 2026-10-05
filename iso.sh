#!/bin/sh
set -e

VERBOSE=0
BUILD_ARGS=""
KERNEL_CMDLINE="${KERNEL_CMDLINE:-}"
for arg in "$@"; do
  case "$arg" in
    -v|--verbose) VERBOSE=1; BUILD_ARGS="$BUILD_ARGS -v" ;;
  esac
done

bar_print() {
    printf "%s\n" "$1"
}

progress_bar() {
    return 0
}

cleanup_bar() {
    return 0
}

./build.sh $BUILD_ARGS

TOTAL_STEPS=5
CURRENT_STEP=0

CURRENT_STEP=$((CURRENT_STEP + 1))
bar_print "$(printf '\033[1;36mPreparing ISO directory...\033[0m')"
progress_bar "$CURRENT_STEP" "$TOTAL_STEPS" "Preparing ISO directory"
rm -f isodir/boot/initrd.img isodir/boot/lebirun.kernel isodir/boot/rootfs.squashfs
mkdir -p isodir/boot/grub

if [ -f initrd.img ]; then
    cp initrd.img isodir/boot/initrd.img
fi

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
    ROOTFS_MAGIC="$(dd if=rootfs.squashfs bs=4 count=1 2>/dev/null | od -An -tx4 | tr -d ' \n')"
    if [ "$ROOTFS_MAGIC" != "73717368" ]; then
        printf "\033[1;31mError: rootfs.squashfs is not SquashFS; remove it and install squashfs-tools.\033[0m\n"
        exit 1
    fi
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

EFI_SYSTEM_DIR="/usr/lib/grub/x86_64-efi"
EFI_LOCAL_DIR="${GRUB_EFI_DIRECTORY:-$HOME/.local/share/lebirun/grub/x86_64-efi}"
EFI_MODULE_DIR=""
if [ -d "$EFI_SYSTEM_DIR" ]; then
    EFI_MODULE_DIR="$EFI_SYSTEM_DIR"
elif [ -f "$EFI_LOCAL_DIR/modinfo.sh" ]; then
    EFI_MODULE_DIR="$EFI_LOCAL_DIR"
fi

GRUB_DIRECTORY="${GRUB_DIRECTORY:-/usr/lib/grub/i386-pc}"
GRUB_ISO_DIRECTORY="$(mktemp -d)"
EFI_WORK_DIRECTORY="$(mktemp -d)"
trap 'rm -rf -- "$GRUB_ISO_DIRECTORY" "$EFI_WORK_DIRECTORY"' EXIT HUP INT TERM
cp -a "$GRUB_DIRECTORY/." "$GRUB_ISO_DIRECTORY/"
printf '%s\n' part_msdos > "$GRUB_ISO_DIRECTORY/partmap.lst"

HYBRID=0
if [ -n "$EFI_MODULE_DIR" ] && command -v grub-mkimage >/dev/null 2>&1 && command -v mformat >/dev/null 2>&1 && command -v mmd >/dev/null 2>&1 && command -v mcopy >/dev/null 2>&1; then
    cat > "$EFI_WORK_DIRECTORY/early.cfg" << EOF
search --set=root --file /boot/grub/grub.cfg
set prefix=(\$root)/boot/grub
configfile /boot/grub/grub.cfg
EOF
    if grub-mkimage -O x86_64-efi -d "$EFI_MODULE_DIR" -o "$EFI_WORK_DIRECTORY/BOOTX64.EFI" -p /boot/grub -c "$EFI_WORK_DIRECTORY/early.cfg" efi_gop normal multiboot2 iso9660 part_msdos part_gpt configfile search && \
       dd if=/dev/zero of="$EFI_WORK_DIRECTORY/efi.img" bs=1K count=1024 status=none && \
       mformat -i "$EFI_WORK_DIRECTORY/efi.img" -v EFI :: >/dev/null && \
       mmd -i "$EFI_WORK_DIRECTORY/efi.img" ::EFI ::EFI/BOOT >/dev/null && \
       mcopy -i "$EFI_WORK_DIRECTORY/efi.img" "$EFI_WORK_DIRECTORY/BOOTX64.EFI" ::EFI/BOOT/BOOTX64.EFI; then
        cp "$EFI_WORK_DIRECTORY/efi.img" isodir/efi.img
        mkdir -p isodir/boot/grub/x86_64-efi
        for _pm in part_acorn part_amiga part_bsd part_dfly part_dvh part_plan part_sun part_sunpc; do
            cp "$EFI_MODULE_DIR/$_pm.mod" isodir/boot/grub/x86_64-efi/
        done
        HYBRID=1
    else
        printf "\033[0;33mWarning: EFI image build failed; building BIOS-only ISO.\033[0m\n"
    fi
fi

if [ -n "$EFI_MODULE_DIR" ] && command -v grub-mkimage >/dev/null 2>&1; then
    if grub-mkimage -O x86_64-efi -d "$EFI_MODULE_DIR" -o "$EFI_WORK_DIRECTORY/BOOTX64DISK.EFI" -p /boot/grub -c "$EFI_WORK_DIRECTORY/early.cfg" efi_gop normal multiboot2 fat ext2 part_msdos part_gpt configfile search search_label; then
        mkdir -p isodir/boot/grub
        cp "$EFI_WORK_DIRECTORY/BOOTX64DISK.EFI" isodir/boot/grub/BOOTX64.EFI
    else
        printf "\033[0;33mWarning: disk EFI image build failed; UEFI disk installs will lack a bootloader.\033[0m\n"
    fi
fi

if [ "$HYBRID" -eq 1 ]; then
    GRUB_MODULES="multiboot2 biosdisk part_msdos part_gpt iso9660"
    EXTRA_BOOT_ARGS="-eltorito-alt-boot -e efi.img -no-emul-boot"
    cat > isodir/boot/grub/grub.cfg << EOF
set timeout=10
set default=0
set gfxpayload=keep

menuentry "Lebirun" {
	multiboot2 /boot/lebirun.kernel $KERNEL_CMDLINE
	module2 /boot/rootfs.squashfs
	boot
}
EOF
else
    GRUB_MODULES="multiboot2 biosdisk part_msdos iso9660"
    EXTRA_BOOT_ARGS=""
    cat > isodir/boot/grub/grub.cfg << EOF
set timeout=10
set default=0

menuentry "Lebirun" {
	multiboot2 /boot/lebirun.kernel $KERNEL_CMDLINE
	module2 /boot/rootfs.squashfs
	boot
}
EOF
    if [ -z "$EFI_MODULE_DIR" ]; then
        printf "\033[0;33mWarning: UEFI modules not found; building BIOS-only ISO. Install grub-efi-amd64-bin, or: apt-get download grub-efi-amd64-bin && mkdir -p $HOME/.local/share/lebirun/grub/x86_64-efi && dpkg-deb --fsys-tarfile grub-efi-amd64-bin_*.deb | tar -x -C $HOME/.local/share/lebirun/grub/x86_64-efi --strip-components=5 ./usr/lib/grub/x86_64-efi\033[0m\n"
    fi
fi

CURRENT_STEP=$((CURRENT_STEP + 1))
bar_print "$(printf '\033[1;36mCreating ISO image...\033[0m')"
progress_bar "$CURRENT_STEP" "$TOTAL_STEPS" "Creating ISO image"
if [ "$VERBOSE" -eq 1 ]; then
    grub-mkrescue -d "$GRUB_ISO_DIRECTORY" --compress=xz --install-modules="$GRUB_MODULES" --fonts="" --locales="" --themes="" -o lebirun.iso isodir $EXTRA_BOOT_ARGS 2>&1 || \
    grub-mkrescue -d "$GRUB_ISO_DIRECTORY" --install-modules="$GRUB_MODULES" --fonts="" --locales="" --themes="" -o lebirun.iso isodir $EXTRA_BOOT_ARGS
else
    grub-mkrescue -d "$GRUB_ISO_DIRECTORY" --compress=xz --install-modules="$GRUB_MODULES" --fonts="" --locales="" --themes="" -o lebirun.iso isodir $EXTRA_BOOT_ARGS || \
    grub-mkrescue -d "$GRUB_ISO_DIRECTORY" --install-modules="$GRUB_MODULES" --fonts="" --locales="" --themes="" -o lebirun.iso isodir $EXTRA_BOOT_ARGS
fi

cleanup_bar
printf "\033[1;32mISO created: lebirun.iso (%s bytes)\033[0m\n" "$(stat -c%s lebirun.iso)"
