#!/bin/sh
set -e
. ./config.sh

for PROJECT in $SYSTEM_HEADER_PROJECTS userprog; do
  if [ -d "$PROJECT" ]; then
    (cd "$PROJECT" && $MAKE clean)
  fi
done

if [ -d "userprog/coreutils" ]; then
  echo "Cleaning coreutils..."
  (cd userprog/coreutils && $MAKE clean)
fi

if [ -d "userprog/lsh" ]; then
  echo "Cleaning lsh..."
  if [ -f "userprog/lsh/.build/config.mk" ]; then
    (cd userprog/lsh && $MAKE clean)
  fi
  find userprog/lsh -type f -name '*.o' -delete
fi

rm -f root/bin/lsh
rm -f root/bin/sh
rm -f root/bin/lebcu
rm -f root/bin/echo
rm -f root/bin/pwd
rm -f root/bin/ls
rm -f root/bin/cat
rm -f root/bin/touch
rm -f root/bin/mkdir
rm -f root/bin/rm
rm -f root/bin/write
rm -f root/bin/ticks
rm -f root/bin/cres
rm -f root/bin/df
rm -f root/bin/free
rm -f root/bin/uname
rm -f root/bin/date
rm -f userprog/coreutils/*.bin
find userprog/coreutils -type f -name '*.o' -delete
rm -f userprog/lsh/lsh
rm -f userprog/lsh/highlight
rm -f userprog/lsh/lsh.bin
rm -f userprog/lsh/liblsh.gnu.sym
rm -f userprog/lsh/liblsh.darwin.sym
rm -rf userprog/lsh/.build
rm -rf userprog/lsh/build-cross
rm -rf sysroot
rm -rf isodir
rm -rf lebirun.iso
rm -f initrd.img
rm -rf include
rm -rf rootfs.img
rm -rf rootfs.squashfs

echo "Cleaning remaining .o files..."
find kernel -type f -name '*.o' -delete 2>/dev/null || true
find libc -type f -name '*.o' -delete 2>/dev/null || true
find userprog -type f -name '*.o' -delete 2>/dev/null || true

echo "Cleaning .d dependency files..."
find kernel -type f -name '*.d' -delete 2>/dev/null || true
find libc -type f -name '*.d' -delete 2>/dev/null || true
find userprog -type f -name '*.d' -delete 2>/dev/null || true

echo "Cleaning kernel artifacts..."
rm -f kernel/lebirun.kernel
rm -f kernel/arch/i386/user_shell.bin
rm -f kernel/arch/i386/user_shell_bin.o
rm -f kernel/arch/i386/unifont_bin.o

echo "Cleaning libc/leblibc build artifacts..."
rm -rf libc/leblibc/build-i386
rm -f libc/lib/libc.a
rm -f libc/lib/crt*.o

echo "Clean complete."
