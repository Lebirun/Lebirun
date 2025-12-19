#!/bin/sh
set -e
. ./headers.sh

for PROJECT in $PROJECTS; do
  echo "Building and installing $PROJECT..."
  (cd "$PROJECT" && DESTDIR="$SYSROOT" $MAKE install)
done

echo "Building user programs..."
(cd userprog && $MAKE all)

if [ -d "initrd" ]; then
  echo "Building initrd..."
  chmod +x mkinitrd.sh
  ./mkinitrd.sh initrd initrd.img
fi
