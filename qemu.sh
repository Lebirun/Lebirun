#!/bin/sh
set -e
. ./config.sh

VERBOSE=0
DO_BUILD=0
NO_BUILD=0
DISK=0
ISO_ARGS=""
for arg in "$@"; do
  case "$arg" in
    -v|--verbose) VERBOSE=1; ISO_ARGS="$ISO_ARGS -v" ;;
    -b|--build) DO_BUILD=1 ;;
    --no-build) NO_BUILD=1 ;;
    --disk) DISK=1 ;;
  esac
done

if [ "$NO_BUILD" -eq 0 ]; then
    if [ "$DO_BUILD" -eq 1 ]; then
        printf "\033[1;34mBuilding ISO before launch...\033[0m\n"
        ./iso.sh $ISO_ARGS
    elif [ ! -f lebirun.iso ]; then
        printf "\033[1;34mNo ISO found, building...\033[0m\n"
        ./iso.sh $ISO_ARGS
    fi
fi

if [ "$NO_BUILD" -eq 0 ] && [ ! -f lebirun.iso ]; then
    printf "\033[1;31mError: lebirun.iso was not created.\033[0m\n"
    exit 1
fi

printf "\033[1;34mStarting QEMU...\033[0m\n"

QEMU_CMD="qemu-system-x86_64"

if [ "$VERBOSE" -eq 1 ]; then
    printf "  -> %s %s\n" "$QEMU_CMD" "(see below)"
fi

if [ -t 0 ]; then
    _OLD_STTY="$(stty -g 2>/dev/null || true)"
    cleanup_tty() {
        if [ -n "$_OLD_STTY" ]; then
            stty "$_OLD_STTY" 2>/dev/null || stty sane 2>/dev/null || true
        else
            stty sane 2>/dev/null || true
        fi
    }
    trap cleanup_tty EXIT INT TERM HUP
fi

CDROM_ARGS=""
if [ "$NO_BUILD" -eq 0 ]; then
    CDROM_ARGS="-cdrom lebirun.iso"
fi

DISK_ARGS=""
if [ "$DISK" -eq 1 ]; then
    DISK_ARGS="-drive file=sata_disk.qcow2,if=none,id=sata0,format=qcow2 -device ide-hd,drive=sata0,bus=ahci0.0"
fi

$QEMU_CMD \
    -m 4G \
    -smp 4 \
    -cpu host \
    -vga qxl \
    $CDROM_ARGS \
    -s -S \
    -serial stdio \
    -device ahci,id=ahci0 \
    $DISK_ARGS \
    -netdev user,id=net0,hostfwd=tcp::5555-:80 \
    -device e1000,netdev=net0 \
    -accel kvm \
    -boot d
