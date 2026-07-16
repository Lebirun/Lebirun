#!/bin/sh
set -e

if [ ! -r ./VERSION ]; then
  printf 'Error: VERSION is missing\n' >&2
  exit 1
fi

OS_VERSION=$(sed -n '1{s/[[:space:]]//g;p;}' ./VERSION)
case "$OS_VERSION" in
  ""|*[!0-9A-Za-z._+-]*)
    printf 'Error: invalid VERSION value: %s\n' "$OS_VERSION" >&2
    exit 1
    ;;
esac

VERBOSE="${VERBOSE:-0}"
for arg in "$@"; do
  case "$arg" in
    -v|--verbose) VERBOSE=1 ;;
  esac
done

. ./headers.sh

MAKEFLAGS="${MAKEFLAGS:--j$(nproc 2>/dev/null || echo 4) --output-sync=recurse}"
export MAKEFLAGS

BUILD_START=$(date +%s)

TOTAL_STEPS=0
for PROJECT in $PROJECTS; do
  TOTAL_STEPS=$((TOTAL_STEPS + 1))
done
TOTAL_STEPS=$((TOTAL_STEPS + 1))
TOTAL_STEPS=$((TOTAL_STEPS + 1))
[ -d "initrd" ] && TOTAL_STEPS=$((TOTAL_STEPS + 1))
[ -d "root" ] && TOTAL_STEPS=$((TOTAL_STEPS + 1))
[ -d "modules" ] && TOTAL_STEPS=$((TOTAL_STEPS + 1))
CURRENT_STEP=0

COLS=$(tput cols 2>/dev/null || echo 80)
ROWS=$(tput lines 2>/dev/null || echo 24)

_BAR_ACTIVE=0
_BAR_LAST=""
setup_bar() {
  _BAR_ACTIVE=1
}

cleanup_bar() {
  if [ "$_BAR_ACTIVE" -eq 1 ]; then
    printf "\r\033[2K" >&2
    _BAR_ACTIVE=0
  fi
}

trap cleanup_bar EXIT INT TERM HUP

progress_bar() {
  _step="$1"
  _total="$2"
  _msg="$3"
  COLS=$(tput cols 2>/dev/null || echo 80)
  _pct=$((_step * 100 / _total))
  _bar_w=$((COLS - 8))
  if [ "$_bar_w" -lt 10 ]; then
    _bar_w=10
  fi
  _filled=$((_pct * _bar_w / 100))
  _empty=$((_bar_w - _filled))
  _bar=""
  _i=0
  while [ "$_i" -lt "$_filled" ]; do
    _bar="${_bar}#"
    _i=$((_i + 1))
  done
  _i=0
  while [ "$_i" -lt "$_empty" ]; do
    _bar="${_bar}."
    _i=$((_i + 1))
  done
  _BAR_LAST=$(printf "%3d%% [%s]" "$_pct" "$_bar")
  printf "\r\033[2K%s" "$_BAR_LAST" >&2
}

bar_print() {
  printf "\r\033[2K" >&2
  printf "%s\n" "$1"
  if [ -n "$_BAR_LAST" ]; then
    printf "\r%s" "$_BAR_LAST" >&2
  fi
}

setup_bar

if [ "$VERBOSE" -eq 1 ]; then
  export V=1
else
  export V=0
fi

ESC=$(printf '\033')

run_cmd() {
  _desc="$1"
  shift
  _step_start=$(date +%s)
  if [ "$VERBOSE" -eq 1 ]; then
    printf "  -> %s\n" "$*"
    "$@"
  else
    _outpipe=$(mktemp)
    rm -f "$_outpipe"
    mkfifo "$_outpipe"
    _errfile=$(mktemp)
    while IFS= read -r _line; do
      case "$_line" in
        *'  CC '*|*'  LD '*|*'  AR '*|*'  AS '*|*'  STRIP '*|*'  CCLD '*|*'  HOSTCC '*)
          printf "\r\033[2K%s\n" "$_line"
          if [ -n "$_BAR_LAST" ]; then printf "\r%s" "$_BAR_LAST" >&2; fi
          ;;
      esac
    done <"$_outpipe" &
    _filter_pid=$!
    if "$@" >"$_outpipe" 2>"$_errfile"; then
      _status=0
    else
      _status=$?
    fi
    wait "$_filter_pid"
    if [ -s "$_errfile" ]; then
      printf "\r\033[2K" >&2
      sed \
        -e "s/\(error:\)/${ESC}[1;31m\1${ESC}[0m/g" \
        -e "s/\(warning:\)/${ESC}[1;33m\1${ESC}[0m/g" \
        "$_errfile"
      if [ -n "$_BAR_LAST" ]; then printf "\r%s" "$_BAR_LAST" >&2; fi
    fi
    rm -f "$_outpipe" "$_errfile"
    if [ "$_status" -ne 0 ]; then
      return "$_status"
    fi
  fi
  _step_end=$(date +%s)
  _step_dur=$((_step_end - _step_start))
  bar_print "$(printf "  \033[0;32mdone in %ds\033[0m" "$_step_dur")"
  if [ "$_BAR_ACTIVE" -eq 1 ]; then
    progress_bar "$CURRENT_STEP" "$TOTAL_STEPS" "$_desc"
  fi
}

for PROJECT in $PROJECTS; do
  CURRENT_STEP=$((CURRENT_STEP + 1))
  bar_print "$(printf '\033[1;36mBuilding %s...\033[0m' "$PROJECT")"
  progress_bar "$CURRENT_STEP" "$TOTAL_STEPS" "Building $PROJECT"
  if [ "$PROJECT" = "libc/leblibc" ]; then
    run_cmd "Building $PROJECT" sh -c "cd \"$PROJECT\" && $MAKE DESTDIR=\"$SYSROOT\" ARCH=x86_64 CROSS_COMPILE=x86_64-elf- CC=x86_64-elf-gcc prefix=/usr includedir=/usr/include libdir=/usr/lib install-libs install-headers"
  else
    run_cmd "Building $PROJECT" sh -c "cd \"$PROJECT\" && DESTDIR=\"$SYSROOT\" $MAKE install"
  fi
done

CURRENT_STEP=$((CURRENT_STEP + 1))
bar_print "$(printf '\033[1;36mBuilding user programs...\033[0m')"
progress_bar "$CURRENT_STEP" "$TOTAL_STEPS" "Building user programs"
run_cmd "Building user programs" sh -c "cd userprog && $MAKE all"

if [ -d "modules" ]; then
  CURRENT_STEP=$((CURRENT_STEP + 1))
  bar_print "$(printf '\033[1;36mBuilding modules...\033[0m')"
  progress_bar "$CURRENT_STEP" "$TOTAL_STEPS" "Building modules"
  run_cmd "Building modules" sh -c "cd modules && $MAKE stage"
fi

for _required in root/init; do
  if [ ! -f "$_required" ]; then
    cleanup_bar
    printf "\033[1;31mError: required rootfs file missing: %s\033[0m\n" "$_required"
    exit 1
  fi
done
chmod +x mkinitrd.sh

CURRENT_STEP=$((CURRENT_STEP + 1))
if [ -f terminfo/linux.ti ] && command -v tic >/dev/null 2>&1; then
  bar_print "$(printf '\033[1;36mCompiling terminfo database...\033[0m')"
  progress_bar "$CURRENT_STEP" "$TOTAL_STEPS" "Compiling terminfo database"
  mkdir -p root/usr/share/terminfo
  run_cmd "Compiling terminfo" sh -c "TERMINFO=root/usr/share/terminfo tic -o root/usr/share/terminfo terminfo/linux.ti 2>/dev/null || true"
else
  bar_print "$(printf '\033[0;33mSkipping terminfo (not available)\033[0m')"
  progress_bar "$CURRENT_STEP" "$TOTAL_STEPS" "Skipping terminfo (not available)"
fi

if [ -d "initrd" ]; then
  CURRENT_STEP=$((CURRENT_STEP + 1))
  if [ ! -f "initrd.img" ] || [ -n "$(find initrd -newer initrd.img 2>/dev/null | head -1)" ]; then
    bar_print "$(printf '\033[1;36mBuilding initrd image...\033[0m')"
    progress_bar "$CURRENT_STEP" "$TOTAL_STEPS" "Building initrd image"
    run_cmd "Building initrd" ./mkinitrd.sh initrd initrd.img
  else
    bar_print "$(printf '\033[0;33mSkipping initrd (up to date)\033[0m')"
    progress_bar "$CURRENT_STEP" "$TOTAL_STEPS" "Skipping initrd (up to date)"
  fi
fi

if command -v grub-mkimage >/dev/null 2>&1; then
  mkdir -p root/boot/grub/i386-pc
  if [ ! -f root/boot/grub/i386-pc/core.img ]; then
    GRUB_EARLY_CFG=$(mktemp)
    GRUB_CORE_TMP=root/boot/grub/i386-pc/core.img.tmp
    cat > "$GRUB_EARLY_CFG" <<'GRUBEOF'
set root=(hd0,msdos1)
set prefix=(hd0,msdos1)/boot/grub
insmod normal
normal
GRUBEOF
    if grub-mkimage -O i386-pc -o "$GRUB_CORE_TMP" \
      -c "$GRUB_EARLY_CFG" \
      -p '(hd0,msdos1)/boot/grub' biosdisk part_msdos ext2 multiboot2 normal configfile; then
      mv "$GRUB_CORE_TMP" root/boot/grub/i386-pc/core.img
    else
      rm -f "$GRUB_CORE_TMP"
      printf "\033[0;33mWarning: skipping installed-system GRUB core image; install grub-pc-bin if needed.\033[0m\n"
    fi
    rm -f "$GRUB_EARLY_CFG"
  fi
  if [ -f /usr/lib/grub/i386-pc/boot.img ]; then
    cp /usr/lib/grub/i386-pc/boot.img root/boot/grub/i386-pc/boot.img
  fi
fi

KERNEL_BIN="sysroot/boot/lebirun.kernel"
if [ -f "$KERNEL_BIN" ]; then
  mkdir -p root/boot
  cp "$KERNEL_BIN" root/boot/lebirun.kernel
fi

TOOLCHAIN_HOST="${TOOLCHAIN_HOST:-$(./default-host.sh)}"
TOOLCHAIN_CC="${TOOLCHAIN_CC:-$TOOLCHAIN_HOST-gcc}"
TOOLCHAIN_STRIP="${TOOLCHAIN_STRIP:-$TOOLCHAIN_HOST-strip}"

if [ -d "sysroot/usr/lib" ]; then
  mkdir -p root/usr/lib
  for f in crt1.o crti.o crtn.o libc.a libm.a libpthread.a libdl.a librt.a libcrypt.a libresolv.a libutil.a libxnet.a; do
    [ -f "sysroot/usr/lib/$f" ] && cp "sysroot/usr/lib/$f" "root/usr/lib/$f"
  done

  for f in libgcc.a crtbegin.o crtend.o; do
    _src=$("$TOOLCHAIN_CC" -print-file-name="$f")
    if [ "$_src" != "$f" ] && [ -f "$_src" ]; then
      rm -f "root/usr/lib/$f"
      cp "$_src" "root/usr/lib/$f"
    fi
  done
  for f in root/usr/lib/*.a; do
    [ -f "$f" ] && "$TOOLCHAIN_STRIP" --strip-debug "$f" 2>/dev/null || true
  done
  for f in root/usr/lib/*.o; do
    [ -f "$f" ] && "$TOOLCHAIN_STRIP" --strip-debug "$f" 2>/dev/null || true
  done
fi
if [ -d "sysroot/usr/include" ]; then
  mkdir -p root/usr/include
  cp -R --preserve=timestamps sysroot/usr/include/. root/usr/include/.
fi
if [ -f "libc/user.ld" ]; then
  mkdir -p root/usr/lib
  cp libc/user.ld root/usr/lib/user.ld
fi
for f in root/bin/* root/sbin/*; do
  [ -f "$f" ] && "$TOOLCHAIN_STRIP" --strip-debug "$f" 2>/dev/null || true
done

if [ -d "root" ]; then
  get_define_version() {
    _file="$1"
    _macro="$2"
    _fallback="$3"
    _ver=$(grep "define $_macro" "$_file" 2>/dev/null | sed 's/.*"\(.*\)".*/\1/')
    if [ -z "$_ver" ]; then _ver="$_fallback"; fi
    printf '%s\n' "$_ver"
  }
  _lebutils_ver=$(get_define_version userprog/lebutils/src/about.h LEBUTILS_VERSION "$OS_VERSION")
  _lebinit_ver=$(get_define_version userprog/LebInit/src/about.h LEBINIT_VERSION "$OS_VERSION")
  mkdir -p root/etc/lebpkg/installed
  write_pkg_db() {
    _pkg="$1"
    _ver="$2"
    _desc="$3"
    _depends="$4"
    _out="root/etc/lebpkg/installed/$_pkg"
    shift 4
    {
      printf 'Package: %s\n' "$_pkg"
      printf 'Status: install ok installed\n'
      printf 'Version: %s\n' "$_ver"
      if [ -n "$_depends" ]; then printf 'Depends: %s\n' "$_depends"; fi
      printf 'Description: %s\n' "$_desc"
      printf 'Files:\n'
      for _f in "$@"; do
        [ -n "$_f" ] && printf '%s\n' "$_f"
      done
    } > "$_out"
  }
  _base_files=$(
    {
      find root/boot -type f 2>/dev/null
      find root/usr/include -type f 2>/dev/null
      [ -f root/etc/lke.autostart ] && printf '%s\n' root/etc/lke.autostart
      [ -f root/etc/motd ] && printf '%s\n' root/etc/motd
      [ -f root/etc/other/license.txt ] && printf '%s\n' root/etc/other/license.txt
      [ -f root/etc/ssl/certs/ca-certificates.crt ] && printf '%s\n' root/etc/ssl/certs/ca-certificates.crt
    } | sed 's#^root##' | sort
  )
  _lebutils_files=$(
    {
      [ -f root/bin/lebu ] && printf '%s\n' root/bin/lebu
      find root/bin root/sbin -maxdepth 1 -type l 2>/dev/null
    } | sed 's#^root##' | grep -v '^/bin/sh$' | sort
  )
  _lebinit_files=$(
    {
      [ -f root/init ] && printf '%s\n' root/init
      [ -f root/sbin/lebinit ] && printf '%s\n' root/sbin/lebinit
      find root/etc/lebinit/services -type f 2>/dev/null
    } | sed 's#^root##' | sort
  )
  write_pkg_db lebirun-base "$OS_VERSION" "Lebirun base system, bootloader, kernel and headers" "" $_base_files
  write_pkg_db lebutils "$_lebutils_ver" "Lebirun core command utilities" "lebirun-base" $_lebutils_files
  write_pkg_db lebinit "$_lebinit_ver" "Lebirun init system" "lebirun-base" $_lebinit_files
fi

if [ -d "root" ]; then
  CURRENT_STEP=$((CURRENT_STEP + 1))
  if [ ! -f "rootfs.squashfs" ] || [ build.sh -nt rootfs.squashfs ] || [ -n "$(find root -newer rootfs.squashfs 2>/dev/null | head -1)" ]; then
    bar_print "$(printf '\033[1;36mBuilding SquashFS rootfs...\033[0m')"
    progress_bar "$CURRENT_STEP" "$TOTAL_STEPS" "Building SquashFS rootfs"
    if command -v mksquashfs >/dev/null 2>&1; then
      rm -f rootfs.squashfs
      run_cmd "Building SquashFS" mksquashfs root rootfs.squashfs -comp xz -b 131072 -no-xattrs -noappend -quiet -no-progress \
        -e usr/include/c++ usr/lib/libstdc++.a usr/lib/libsupc++.a usr/lib/libgcc.a
    else
      cleanup_bar
      printf "\033[1;31mError: mksquashfs not found; install squashfs-tools.\033[0m\n"
      exit 1
    fi
  else
    bar_print "$(printf '\033[0;33mSkipping rootfs (up to date)\033[0m')"
    progress_bar "$CURRENT_STEP" "$TOTAL_STEPS" "Skipping rootfs (up to date)"
  fi
fi

case "$0" in
  *build.sh)
    cleanup_bar
    BUILD_END=$(date +%s)
    BUILD_DUR=$((BUILD_END - BUILD_START))
    BUILD_MIN=$((BUILD_DUR / 60))
    BUILD_SEC=$((BUILD_DUR % 60))
    if [ "$BUILD_MIN" -gt 0 ]; then
      printf "\033[1;32mBuild complete in %dm%ds!\033[0m\n" "$BUILD_MIN" "$BUILD_SEC"
    else
      printf "\033[1;32mBuild complete in %ds!\033[0m\n" "$BUILD_SEC"
    fi
    ;;
esac
