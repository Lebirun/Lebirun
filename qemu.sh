#!/bin/sh
set -e
. ./iso.sh

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

qemu-system-$(./target-triplet-to-arch.sh $HOST) \
    -m 4G \
    -smp 4 \
    -vga cirrus \
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

# Add "-d int,cpu_reset" for spammy QEMU built-in debugging output