#!/bin/sh
set -e
. ./config.sh

for PROJECT in $SYSTEM_HEADER_PROJECTS userprog; do
  if [ -d "$PROJECT" ]; then
    (cd "$PROJECT" && $MAKE clean)
  fi
done

rm -rf sysroot
rm -rf isodir
rm -rf lebirun.iso
rm -f initrd.img
rm -rf include
rm -rf rootfs.img
