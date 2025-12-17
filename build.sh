#!/bin/sh
set -e
. ./headers.sh

echo "Building libc-common..."
(cd libc-common && $MAKE)

echo "Building userprog..."
(cd userprog && $MAKE)

for PROJECT in $PROJECTS; do
  (cd $PROJECT && DESTDIR="$SYSROOT" $MAKE install)
done

echo "Building initrd..."
if [ -d "initrd" ]; then
  chmod +x mkinitrd.sh
  ./mkinitrd.sh initrd initrd.img
fi
