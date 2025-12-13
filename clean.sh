#!/bin/sh
set -e
. ./config.sh

for PROJECT in $PROJECTS; do
  (cd $PROJECT && $MAKE clean)
done

(cd userlibc && $MAKE clean) || true
(cd userprog && $MAKE clean) || true

rm -rf sysroot
rm -rf isodir
rm -rf lebirun.iso
