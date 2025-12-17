#!/bin/sh
set -e
. ./iso.sh
qemu-system-$(./target-triplet-to-arch.sh $HOST) -m 4G -cdrom lebirun.iso -s -S -serial stdio -no-reboot
# Add "-d int,cpu_reset" for spammy QEMU built-in debugging output