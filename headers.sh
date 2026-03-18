#!/bin/sh
set -e
. ./config.sh

mkdir -p "$SYSROOT"

for PROJECT in $SYSTEM_HEADER_PROJECTS; do
  if [ -d "$PROJECT" ]; then
    printf "\033[1;36mInstalling headers for %s...\033[0m\n" "$PROJECT"
    if [ "$PROJECT" = "libc" ]; then
      (cd "$PROJECT" && DESTDIR="$SYSROOT" ARCH=x86_64 $MAKE install-headers)
    else
      (cd "$PROJECT" && DESTDIR="$SYSROOT" $MAKE install-headers)
    fi
  fi
done
