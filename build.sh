#!/bin/sh
set -e
. ./headers.sh

echo "Building userlibc..."
(cd userlibc && $MAKE)

echo "Building userprog..."
(cd userprog && $MAKE)

for PROJECT in $PROJECTS; do
  (cd $PROJECT && DESTDIR="$SYSROOT" $MAKE install)
done
