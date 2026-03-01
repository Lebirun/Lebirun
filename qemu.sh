#!/bin/sh
set -e
. ./config.sh

VERBOSE=0
NO_BUILD=0
ISO_ARGS=""
for arg in "$@"; do
  case "$arg" in
    -v|--verbose) VERBOSE=1; ISO_ARGS="$ISO_ARGS -v" ;;
    --no-build) NO_BUILD=1 ;;
  esac
done

if [ "$NO_BUILD" -eq 0 ]; then
    printf "\033[1;34mBuilding ISO before launch...\033[0m\n"
    ./iso.sh $ISO_ARGS
fi

printf "\033[1;34mStarting QEMU...\033[0m\n"

QEMU_CMD="qemu-system-$(./target-triplet-to-arch.sh "$HOST")"
QEMU_ARGS="-m 4G -smp 4 -vga qxl -cdrom lebirun.iso -s -S -serial stdio -no-reboot"
QEMU_ARGS="$QEMU_ARGS -device ahci,id=ahci0"
QEMU_ARGS="$QEMU_ARGS -drive file=sata_disk.qcow2,if=none,id=sata0,format=qcow2"
QEMU_ARGS="$QEMU_ARGS -device ide-hd,drive=sata0,bus=ahci0.0"
QEMU_ARGS="$QEMU_ARGS -netdev user,id=net0,hostfwd=tcp::5555-:80"
QEMU_ARGS="$QEMU_ARGS -device e1000,netdev=net0,mac=52:54:00:12:34:56"
QEMU_ARGS="$QEMU_ARGS -accel kvm -boot d"

if [ "$VERBOSE" -eq 1 ]; then
    printf "  -> %s %s\n" "$QEMU_CMD" "$QEMU_ARGS"
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

$QEMU_CMD \
    -m 4G \
    -smp 4 \
    -vga qxl \
    -cdrom lebirun.iso \
    -s -S \
    -serial stdio \
    -no-reboot \
    -device ahci,id=ahci0 \
    -drive file=sata_disk.qcow2,if=none,id=sata0,format=qcow2 \
    -device ide-hd,drive=sata0,bus=ahci0.0 \
    -netdev user,id=net0,hostfwd=tcp::5555-:80 \
    -device e1000,netdev=net0,mac=52:54:00:12:34:56 \
    -accel kvm \
    -boot d
