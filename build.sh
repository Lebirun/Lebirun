#!/bin/sh
set -e

VERBOSE="${VERBOSE:-0}"
for arg in "$@"; do
  case "$arg" in
    -v|--verbose) VERBOSE=1 ;;
  esac
done

. ./headers.sh

MAKEFLAGS="${MAKEFLAGS:--j$(nproc 2>/dev/null || echo 4)}"
export MAKEFLAGS

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

_BAR_SETUP="${_BAR_SETUP:-0}"
setup_bottom_bar() {
  if [ "$_BAR_SETUP" -eq 0 ]; then
    printf "\033[1;%dr" "$((ROWS - 2))"
    printf "\033[%d;1H\033[2K" "$((ROWS - 1))"
    printf "\033[%d;1H\033[2K" "$ROWS"
    printf "\033[%d;1H" "$((ROWS - 2))"
    _BAR_SETUP=1
  fi
}

cleanup_bar() {
  printf "\033[1;%dr" "$ROWS"
  printf "\033[%d;1H\033[2K" "$((ROWS - 1))"
  printf "\033[%d;1H\033[2K" "$ROWS"
  printf "\033[%d;1H" "$((ROWS - 1))"
}

trap 'cleanup_bar; exit 1' INT TERM

progress_bar() {
  _step="$1"
  _total="$2"
  _msg="$3"
  _pct=$((_step * 100 / _total))
  _bar_w=$((COLS - 2))
  if [ "$_bar_w" -lt 10 ]; then
    _bar_w=10
  fi
  _filled=$((_pct * _bar_w / 100))
  _empty=$((_bar_w - _filled))
  _bar=""
  _i=0
  while [ "$_i" -lt "$_filled" ]; do
    _bar="${_bar}="
    _i=$((_i + 1))
  done
  if [ "$_filled" -lt "$_bar_w" ]; then
    _bar="${_bar}>"
    _empty=$((_empty - 1))
  fi
  _i=0
  while [ "$_i" -lt "$_empty" ]; do
    _bar="${_bar} "
    _i=$((_i + 1))
  done
  _label=$(printf "%3d%%  %s" "$_pct" "$_msg")
  printf "\033[s"
  printf "\033[%d;1H\033[2K%s" "$((ROWS - 1))" "$_label"
  printf "\033[%d;1H\033[2K[%s]" "$ROWS" "$_bar"
  printf "\033[u"
}

setup_bottom_bar

if [ "$VERBOSE" -eq 1 ]; then
  export V=1
else
  export V=0
fi

run_cmd() {
  _desc="$1"
  shift
  if [ "$VERBOSE" -eq 1 ]; then
    printf "  -> %s\n" "$*"
    "$@"
  else
    "$@" 2>&1 | grep -E '^\s*(CC|LD|AS|AR|STRIP|OBJCOPY)\s' || true
  fi
}

for PROJECT in $PROJECTS; do
  CURRENT_STEP=$((CURRENT_STEP + 1))
  progress_bar "$CURRENT_STEP" "$TOTAL_STEPS" "Building $PROJECT"
  printf "Building %s...\n" "$PROJECT"
  if [ "$PROJECT" = "libc" ]; then
    run_cmd "Building $PROJECT" sh -c "cd \"$PROJECT\" && DESTDIR=\"$SYSROOT\" ARCH=i386 $MAKE install"
  else
    run_cmd "Building $PROJECT" sh -c "cd \"$PROJECT\" && DESTDIR=\"$SYSROOT\" $MAKE install"
  fi
done

CURRENT_STEP=$((CURRENT_STEP + 1))
progress_bar "$CURRENT_STEP" "$TOTAL_STEPS" "Building ncurses"
printf "Building ncurses...\n"
run_cmd "Building ncurses" sh -c "cd libc/lib && $MAKE ncurses"

CURRENT_STEP=$((CURRENT_STEP + 1))
progress_bar "$CURRENT_STEP" "$TOTAL_STEPS" "Building user programs"
printf "Building user programs...\n"
run_cmd "Building user programs" sh -c "cd userprog && $MAKE all"

chmod +x mkinitrd.sh

CURRENT_STEP=$((CURRENT_STEP + 1))
if [ -f terminfo/linux.ti ] && command -v tic >/dev/null 2>&1; then
  progress_bar "$CURRENT_STEP" "$TOTAL_STEPS" "Compiling terminfo database"
  printf "Compiling terminfo database...\n"
  mkdir -p root/usr/share/terminfo
  run_cmd "Compiling terminfo" sh -c "TERMINFO=root/usr/share/terminfo tic -o root/usr/share/terminfo terminfo/linux.ti 2>/dev/null || true"
else
  progress_bar "$CURRENT_STEP" "$TOTAL_STEPS" "Skipping terminfo (not available)"
  printf "Skipping terminfo (not available)\n"
fi

if [ -d "initrd" ]; then
  CURRENT_STEP=$((CURRENT_STEP + 1))
  if [ ! -f "initrd.img" ] || [ -n "$(find initrd -newer initrd.img 2>/dev/null | head -1)" ]; then
    progress_bar "$CURRENT_STEP" "$TOTAL_STEPS" "Building initrd image"
    printf "Building initrd image...\n"
    run_cmd "Building initrd" ./mkinitrd.sh initrd initrd.img
  else
    progress_bar "$CURRENT_STEP" "$TOTAL_STEPS" "Skipping initrd (up to date)"
    printf "Skipping initrd (up to date)\n"
  fi
fi

if [ -d "root" ]; then
  CURRENT_STEP=$((CURRENT_STEP + 1))
  if [ ! -f "rootfs.squashfs" ] || [ -n "$(find root -newer rootfs.squashfs 2>/dev/null | head -1)" ]; then
    progress_bar "$CURRENT_STEP" "$TOTAL_STEPS" "Building SquashFS rootfs"
    printf "Building SquashFS rootfs...\n"
    if command -v mksquashfs >/dev/null 2>&1; then
      rm -f rootfs.squashfs
      run_cmd "Building SquashFS" mksquashfs root rootfs.squashfs -noI -noD -noF -noX -no-xattrs -noappend -no-compression -quiet
    else
      run_cmd "Building rootfs (fallback)" ./mkinitrd.sh root rootfs.squashfs
    fi
  else
    progress_bar "$CURRENT_STEP" "$TOTAL_STEPS" "Skipping rootfs (up to date)"
    printf "Skipping rootfs (up to date)\n"
  fi
fi

case "$0" in
  *build.sh)
    cleanup_bar
    printf "\033[1;32mBuild complete!\033[0m\n"
    ;;
esac
