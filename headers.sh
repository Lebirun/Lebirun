#!/bin/sh
set -e
. ./config.sh

mkdir -p "$SYSROOT"

for PROJECT in $SYSTEM_HEADER_PROJECTS; do
  if [ -d "$PROJECT" ]; then
    echo "Installing headers for $PROJECT..."
    (cd "$PROJECT" && DESTDIR="$SYSROOT" $MAKE install-headers)
  fi
done
