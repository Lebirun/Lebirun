#!/bin/sh
set -e
. ./config.sh

VERBOSE=0
for arg in "$@"; do
  case "$arg" in
    -v|--verbose) VERBOSE=1 ;;
  esac
done

CLEAN_START=$(date +%s)

TOTAL_STEPS=0
for PROJECT in $SYSTEM_HEADER_PROJECTS userprog; do
  [ -d "$PROJECT" ] && TOTAL_STEPS=$((TOTAL_STEPS + 1))
done
[ -d "userprog/lebutils" ] && TOTAL_STEPS=$((TOTAL_STEPS + 1))
[ -d "userprog/lsh" ] && TOTAL_STEPS=$((TOTAL_STEPS + 1))
[ -d "userprog/LebInit" ] && TOTAL_STEPS=$((TOTAL_STEPS + 1))
[ -d "userprog/login" ] && TOTAL_STEPS=$((TOTAL_STEPS + 1))
[ -d "libc/lib/ncurses-6.6" ] && TOTAL_STEPS=$((TOTAL_STEPS + 1))
[ -d "userprog/lebinstaller" ] && TOTAL_STEPS=$((TOTAL_STEPS + 1))
[ -d "userprog/liblebui" ] && TOTAL_STEPS=$((TOTAL_STEPS + 1))
TOTAL_STEPS=$((TOTAL_STEPS + 4))
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

run_clean() {
  _desc="$1"
  shift
  if [ "$VERBOSE" -eq 1 ]; then
    printf "  -> %s\n" "$*"
    "$@" 2>/dev/null || true
  else
    "$@" > /dev/null 2>&1 || true
  fi
}

remove_dir_contents() {
  _dir="$1"
  [ -d "$_dir" ] || return 0
  find "$_dir" -mindepth 1 -maxdepth 1 -exec rm -rf {} + 2>/dev/null || true
}

for PROJECT in $SYSTEM_HEADER_PROJECTS userprog; do
  if [ -d "$PROJECT" ]; then
    CURRENT_STEP=$((CURRENT_STEP + 1))
    bar_print "Cleaning $PROJECT..."
    progress_bar "$CURRENT_STEP" "$TOTAL_STEPS" "Cleaning $PROJECT"
    run_clean "Cleaning $PROJECT" sh -c "cd \"$PROJECT\" && $MAKE clean"
  fi
done

if [ -d "userprog/lebutils" ]; then
  CURRENT_STEP=$((CURRENT_STEP + 1))
  bar_print "Cleaning coreutils..."
  progress_bar "$CURRENT_STEP" "$TOTAL_STEPS" "Cleaning lebutils"
  run_clean "Cleaning lebutils" sh -c "cd userprog/lebutils && $MAKE clean"
fi

if [ -d "userprog/lsh" ]; then
  CURRENT_STEP=$((CURRENT_STEP + 1))
  bar_print "Cleaning lsh..."
  progress_bar "$CURRENT_STEP" "$TOTAL_STEPS" "Cleaning lsh"
  if [ -f "userprog/lsh/.build/config.mk" ]; then
    run_clean "Cleaning lsh" sh -c "cd userprog/lsh && $MAKE clean"
  fi
  find userprog/lsh -type f -name '*.o' -delete 2>/dev/null || true
fi

if [ -d "userprog/LebInit" ]; then
  CURRENT_STEP=$((CURRENT_STEP + 1))
  bar_print "Cleaning LebInit..."
  progress_bar "$CURRENT_STEP" "$TOTAL_STEPS" "Cleaning LebInit"
  run_clean "Cleaning LebInit" sh -c "cd userprog/LebInit && $MAKE clean"
fi

if [ -d "userprog/login" ]; then
  CURRENT_STEP=$((CURRENT_STEP + 1))
  bar_print "Cleaning login..."
  progress_bar "$CURRENT_STEP" "$TOTAL_STEPS" "Cleaning login"
  run_clean "Cleaning login" sh -c "cd userprog/login && $MAKE clean"
fi

if [ -d "libc/lib/ncurses-6.6" ]; then
  CURRENT_STEP=$((CURRENT_STEP + 1))
  bar_print "Cleaning ncurses..."
  progress_bar "$CURRENT_STEP" "$TOTAL_STEPS" "Cleaning ncurses"
  run_clean "Cleaning ncurses" sh -c "cd libc/lib/ncurses-6.6 && $MAKE clean"
  rm -rf libc/lib/ncurses-6.6/config.cache 2>/dev/null || true
  find libc/lib/ncurses-6.6 -type f -name '*.o' -delete 2>/dev/null || true
fi

if [ -d "userprog/lebinstaller" ]; then
  CURRENT_STEP=$((CURRENT_STEP + 1))
  bar_print "Cleaning lebinstaller..."
  progress_bar "$CURRENT_STEP" "$TOTAL_STEPS" "Cleaning lebinstaller"
  run_clean "Cleaning lebinstaller" sh -c "cd userprog/lebinstaller && $MAKE clean"
  rm -rf userprog/lebinstaller/bin
fi

if [ -d "userprog/liblebui" ]; then
  CURRENT_STEP=$((CURRENT_STEP + 1))
  bar_print "Cleaning liblebui..."
  progress_bar "$CURRENT_STEP" "$TOTAL_STEPS" "Cleaning liblebui"
  run_clean "Cleaning liblebui" sh -c "cd userprog/liblebui && $MAKE clean"
  rm -rf userprog/liblebui/lib userprog/liblebui/build
fi

CURRENT_STEP=$((CURRENT_STEP + 1))
bar_print "Removing binaries..."
progress_bar "$CURRENT_STEP" "$TOTAL_STEPS" "Removing binaries"
remove_dir_contents root/boot
remove_dir_contents root/bin
remove_dir_contents root/sbin
remove_dir_contents root/usr/bin
remove_dir_contents root/usr/sbin
find root/lib -mindepth 1 -maxdepth 1 ! -name lke -exec rm -rf {} + 2>/dev/null || true
find root/lib/lke -mindepth 1 -maxdepth 1 -exec rm -f {} + 2>/dev/null || true
if [ -d "modules" ]; then
  run_clean "Cleaning modules" sh -c "cd modules && $MAKE clean"
fi
remove_dir_contents root/usr/lib
remove_dir_contents root/usr/include
rm -rf root/usr/share/terminfo
rm -rf root/etc/lebpkg/index
rm -rf root/etc/lebpkg/installed
rm -rf userprog/lebutils/bin
rm -f userprog/lsh/lsh userprog/lsh/highlight userprog/lsh/lsh.bin
rm -f userprog/lsh/liblsh.gnu.sym userprog/lsh/liblsh.darwin.sym
rm -rf userprog/lsh/.build userprog/lsh/build-cross
rm -rf userprog/bin
rm -rf sysroot isodir lebirun.iso
rm -f initrd.img rootfs.img rootfs.squashfs
rm -rf include
rm -rf root/init
rm -rf userprog/lebinstaller/bin

CURRENT_STEP=$((CURRENT_STEP + 1))
bar_print "Cleaning object files..."
progress_bar "$CURRENT_STEP" "$TOTAL_STEPS" "Cleaning object files"
find userprog/lebutils -type f -name '*.o' -delete 2>/dev/null || true
find kernel -type f -name '*.o' -delete 2>/dev/null || true
find libc -type f -name '*.o' -delete 2>/dev/null || true
find userprog -type f -name '*.o' -delete 2>/dev/null || true
find kernel -type f -name '*.d' -delete 2>/dev/null || true
find libc -type f -name '*.d' -delete 2>/dev/null || true
find userprog -type f -name '*.d' -delete 2>/dev/null || true

CURRENT_STEP=$((CURRENT_STEP + 1))
bar_print "Cleaning kernel & libc artifacts..."
progress_bar "$CURRENT_STEP" "$TOTAL_STEPS" "Cleaning kernel & libc artifacts"
rm -f kernel/lebirun.kernel
rm -f kernel/arch/x86_64/user_shell.bin
rm -f kernel/arch/x86_64/user_shell_bin.o
rm -f kernel/arch/x86_64/unifont_bin.o
rm -rf libc/leblibc/build-x86_64
rm -f libc/lib/libc.a
rm -f libc/lib/crt*.o

CURRENT_STEP=$((CURRENT_STEP + 1))
progress_bar "$CURRENT_STEP" "$TOTAL_STEPS" "Done"

cleanup_bar
CLEAN_END=$(date +%s)
CLEAN_DUR=$((CLEAN_END - CLEAN_START))
printf "\033[1;32mClean complete in %ds!\033[0m\n" "$CLEAN_DUR"
