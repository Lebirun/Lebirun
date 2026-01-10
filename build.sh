#!/bin/sh
set -e
. ./headers.sh

MAKEFLAGS="${MAKEFLAGS:--j$(nproc 2>/dev/null || echo 4)}"
export MAKEFLAGS

for PROJECT in $PROJECTS; do
  echo "Building and installing $PROJECT..."
  if [ "$PROJECT" = "libc" ]; then
    (cd "$PROJECT" && DESTDIR="$SYSROOT" ARCH=i386 $MAKE install)
  else
    (cd "$PROJECT" && DESTDIR="$SYSROOT" $MAKE install)
  fi
done

echo "Building user programs..."
(cd userprog && $MAKE all)

rebuild_initrd() {
  local DIR="$1"
  local OUT="$2"
  if [ ! -f "$OUT" ] || [ -n "$(find "$DIR" -newer "$OUT" 2>/dev/null | head -1)" ]; then
    echo "Building $OUT..."
    ./mkinitrd.sh "$DIR" "$OUT"
  else
    echo "Skipping $OUT (up to date)"
  fi
}

chmod +x mkinitrd.sh
[ -d "initrd" ] && rebuild_initrd initrd initrd.img
[ -d "root" ] && rebuild_initrd root rootfs.img
