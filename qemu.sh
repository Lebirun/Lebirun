#!/bin/sh
set -e
. ./iso.sh
qemu-system-$(./target-triplet-to-arch.sh $HOST) -m 4G -cdrom lebirun.iso -s -S -serial stdio -no-reboot -d int,cpu_reset