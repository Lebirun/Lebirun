#!/bin/sh
set -e

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
TOTAL_STEPS=$((TOTAL_STEPS + 1))
[ -d "initrd" ] && TOTAL_STEPS=$((TOTAL_STEPS + 1))
[ -d "root" ] && TOTAL_STEPS=$((TOTAL_STEPS + 1))
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
    _errfile=$(mktemp)
    _outfile=$(mktemp)
    "$@" >"$_outfile" 2>"$_errfile" || true
    if [ -s "$_outfile" ]; then
      printf "\r\033[2K" >&2
      grep -E '^\s*(CC|LD|AR|AS|STRIP|CCLD|HOSTCC)\s' "$_outfile" || true
      if [ -n "$_BAR_LAST" ]; then printf "\r%s" "$_BAR_LAST" >&2; fi
    fi
    if [ -s "$_errfile" ]; then
      printf "\r\033[2K" >&2
      sed \
        -e "s/\(error:\)/${ESC}[1;31m\1${ESC}[0m/g" \
        -e "s/\(warning:\)/${ESC}[1;33m\1${ESC}[0m/g" \
        "$_errfile"
      if [ -n "$_BAR_LAST" ]; then printf "\r%s" "$_BAR_LAST" >&2; fi
    fi
    rm -f "$_errfile" "$_outfile"
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
  if [ "$PROJECT" = "libc" ]; then
    run_cmd "Building $PROJECT" sh -c "cd \"$PROJECT\" && DESTDIR=\"$SYSROOT\" ARCH=x86_64 $MAKE install"
  else
    run_cmd "Building $PROJECT" sh -c "cd \"$PROJECT\" && DESTDIR=\"$SYSROOT\" $MAKE install"
  fi
done

CURRENT_STEP=$((CURRENT_STEP + 1))
bar_print "$(printf '\033[1;36mBuilding ncurses...\033[0m')"
progress_bar "$CURRENT_STEP" "$TOTAL_STEPS" "Building ncurses"
run_cmd "Building ncurses" sh -c "cd libc/lib && $MAKE ncurses"

CURRENT_STEP=$((CURRENT_STEP + 1))
bar_print "$(printf '\033[1;36mBuilding user programs...\033[0m')"
progress_bar "$CURRENT_STEP" "$TOTAL_STEPS" "Building user programs"
run_cmd "Building user programs" sh -c "cd userprog && $MAKE all"

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
    cat > "$GRUB_EARLY_CFG" <<'GRUBEOF'
set root=(hd0,msdos1)
set prefix=(hd0,msdos1)/boot/grub
insmod normal
normal
GRUBEOF
    grub-mkimage -O i386-pc -o root/boot/grub/i386-pc/core.img \
      -c "$GRUB_EARLY_CFG" \
      -p '(hd0,msdos1)/boot/grub' biosdisk part_msdos ext2 multiboot2 normal configfile
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

if [ -d "root" ]; then
  CURRENT_STEP=$((CURRENT_STEP + 1))
  if [ ! -f "rootfs.squashfs" ] || [ -n "$(find root -newer rootfs.squashfs 2>/dev/null | head -1)" ]; then
    bar_print "$(printf '\033[1;36mBuilding SquashFS rootfs...\033[0m')"
    progress_bar "$CURRENT_STEP" "$TOTAL_STEPS" "Building SquashFS rootfs"
    if command -v mksquashfs >/dev/null 2>&1; then
      rm -f rootfs.squashfs
      run_cmd "Building SquashFS" mksquashfs root rootfs.squashfs -noI -noD -noF -noX -no-xattrs -noappend -no-compression -quiet -no-progress
    else
      run_cmd "Building rootfs (fallback)" ./mkinitrd.sh root rootfs.squashfs
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
