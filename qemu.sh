#!/bin/sh
set -e
. ./iso.sh
qemu-system-$(./target-triplet-to-arch.sh $HOST) \
    -m 4G \
    -cdrom lebirun.iso \
    -s -S \
    -serial stdio \
    -no-reboot \
    -device ahci,id=ahci0 \
    -drive file=sata_disk.qcow2,if=none,id=sata0,format=qcow2 \
    -device ide-hd,drive=sata0,bus=ahci0.0 \
    -boot d

# Add "-d int,cpu_reset" for spammy QEMU built-in debugging output