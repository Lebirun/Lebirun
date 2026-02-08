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

build_squashfs() {
  local DIR="$1"
  local OUT="$2"
  if [ ! -f "$OUT" ] || [ -n "$(find "$DIR" -newer "$OUT" 2>/dev/null | head -1)" ]; then
    echo "Building SquashFS image $OUT from $DIR..."
    if command -v mksquashfs >/dev/null 2>&1; then
      rm -f "$OUT"
      mksquashfs "$DIR" "$OUT" -noI -noD -noF -noX -no-xattrs -noappend -no-compression -quiet
      echo "SquashFS image created: $OUT ($(stat -c%s "$OUT") bytes)"
    else
      echo "Warning: mksquashfs not found, falling back to initrd format"
      ./mkinitrd.sh "$DIR" "$OUT"
    fi
  else
    echo "Skipping $OUT (up to date)"
  fi
}

chmod +x mkinitrd.sh
[ -d "initrd" ] && rebuild_initrd initrd initrd.img
[ -d "root" ] && build_squashfs root rootfs.squashfs
