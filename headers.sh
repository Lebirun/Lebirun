#!/bin/sh
set -e
. ./config.sh

mkdir -p "$SYSROOT"

for PROJECT in $SYSTEM_HEADER_PROJECTS; do
  if [ -d "$PROJECT" ]; then
    echo "Installing headers for $PROJECT..."
    if [ "$PROJECT" = "libc" ]; then
      (cd "$PROJECT" && DESTDIR="$SYSROOT" ARCH=i386 $MAKE install-headers)
    else
      (cd "$PROJECT" && DESTDIR="$SYSROOT" $MAKE install-headers)
    fi
  fi
done
