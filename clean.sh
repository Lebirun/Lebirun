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
[ -d "userprog/coreutils" ] && TOTAL_STEPS=$((TOTAL_STEPS + 1))
[ -d "userprog/lsh" ] && TOTAL_STEPS=$((TOTAL_STEPS + 1))
[ -d "userprog/LebInit" ] && TOTAL_STEPS=$((TOTAL_STEPS + 1))
[ -d "userprog/login" ] && TOTAL_STEPS=$((TOTAL_STEPS + 1))
[ -d "libc/lib/ncurses-6.6" ] && TOTAL_STEPS=$((TOTAL_STEPS + 1))
[ -d "ports/nano-8.7.1" ] && TOTAL_STEPS=$((TOTAL_STEPS + 1))
[ -d "ports/htop-3.4.1" ] && TOTAL_STEPS=$((TOTAL_STEPS + 1))
TOTAL_STEPS=$((TOTAL_STEPS + 4))
CURRENT_STEP=0

COLS=$(tput cols 2>/dev/null || echo 80)
ROWS=$(tput lines 2>/dev/null || echo 24)

setup_bottom_bar() {
  printf "\033[1;%dr" "$((ROWS - 2))"
  printf "\033[%d;1H\033[2K" "$((ROWS - 1))"
  printf "\033[%d;1H\033[2K" "$ROWS"
  printf "\033[%d;1H" "$((ROWS - 2))"
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

for PROJECT in $SYSTEM_HEADER_PROJECTS userprog; do
  if [ -d "$PROJECT" ]; then
    CURRENT_STEP=$((CURRENT_STEP + 1))
    progress_bar "$CURRENT_STEP" "$TOTAL_STEPS" "Cleaning $PROJECT"
    printf "Cleaning %s...\n" "$PROJECT"
    run_clean "Cleaning $PROJECT" sh -c "cd \"$PROJECT\" && $MAKE clean"
  fi
done

if [ -d "userprog/coreutils" ]; then
  CURRENT_STEP=$((CURRENT_STEP + 1))
  progress_bar "$CURRENT_STEP" "$TOTAL_STEPS" "Cleaning coreutils"
  printf "Cleaning coreutils...\n"
  run_clean "Cleaning coreutils" sh -c "cd userprog/coreutils && $MAKE clean"
fi

if [ -d "userprog/lsh" ]; then
  CURRENT_STEP=$((CURRENT_STEP + 1))
  progress_bar "$CURRENT_STEP" "$TOTAL_STEPS" "Cleaning lsh"
  printf "Cleaning lsh...\n"
  if [ -f "userprog/lsh/.build/config.mk" ]; then
    run_clean "Cleaning lsh" sh -c "cd userprog/lsh && $MAKE clean"
  fi
  find userprog/lsh -type f -name '*.o' -delete 2>/dev/null || true
fi

if [ -d "userprog/LebInit" ]; then
  CURRENT_STEP=$((CURRENT_STEP + 1))
  progress_bar "$CURRENT_STEP" "$TOTAL_STEPS" "Cleaning LebInit"
  printf "Cleaning LebInit...\n"
  run_clean "Cleaning LebInit" sh -c "cd userprog/LebInit && $MAKE clean"
fi

if [ -d "userprog/login" ]; then
  CURRENT_STEP=$((CURRENT_STEP + 1))
  progress_bar "$CURRENT_STEP" "$TOTAL_STEPS" "Cleaning login"
  printf "Cleaning login...\n"
  run_clean "Cleaning login" sh -c "cd userprog/login && $MAKE clean"
fi

if [ -d "libc/lib/ncurses-6.6" ]; then
  CURRENT_STEP=$((CURRENT_STEP + 1))
  progress_bar "$CURRENT_STEP" "$TOTAL_STEPS" "Cleaning ncurses"
  printf "Cleaning ncurses...\n"
  run_clean "Cleaning ncurses" sh -c "cd libc/lib/ncurses-6.6 && $MAKE clean"
  rm -rf libc/lib/ncurses-6.6/config.cache 2>/dev/null || true
  find libc/lib/ncurses-6.6 -type f -name '*.o' -delete 2>/dev/null || true
fi

CURRENT_STEP=$((CURRENT_STEP + 1))
progress_bar "$CURRENT_STEP" "$TOTAL_STEPS" "Removing binaries"
printf "Removing binaries...\n"
rm -f root/bin/*
rm -f root/sbin/*
rm -f root/usr/bin/*
rm -f root/usr/sbin/*
rm -f root/lib/*
rm -f root/usr/lib/*
rm -f userprog/coreutils/*.bin
rm -f userprog/lsh/lsh userprog/lsh/highlight userprog/lsh/lsh.bin
rm -f userprog/lsh/liblsh.gnu.sym userprog/lsh/liblsh.darwin.sym
rm -rf userprog/lsh/.build userprog/lsh/build-cross
rm -rf sysroot isodir lebirun.iso
rm -f initrd.img rootfs.img rootfs.squashfs
rm -rf include

CURRENT_STEP=$((CURRENT_STEP + 1))
progress_bar "$CURRENT_STEP" "$TOTAL_STEPS" "Cleaning object files"
printf "Cleaning object files...\n"
find userprog/coreutils -type f -name '*.o' -delete 2>/dev/null || true
find kernel -type f -name '*.o' -delete 2>/dev/null || true
find libc -type f -name '*.o' -delete 2>/dev/null || true
find userprog -type f -name '*.o' -delete 2>/dev/null || true
find kernel -type f -name '*.d' -delete 2>/dev/null || true
find libc -type f -name '*.d' -delete 2>/dev/null || true
find userprog -type f -name '*.d' -delete 2>/dev/null || true

CURRENT_STEP=$((CURRENT_STEP + 1))
progress_bar "$CURRENT_STEP" "$TOTAL_STEPS" "Cleaning kernel & libc artifacts"
printf "Cleaning kernel & libc artifacts...\n"
rm -f kernel/lebirun.kernel
rm -f kernel/arch/i386/user_shell.bin
rm -f kernel/arch/i386/user_shell_bin.o
rm -f kernel/arch/i386/unifont_bin.o
rm -rf libc/leblibc/build-i386
rm -f libc/lib/libc.a
rm -f libc/lib/crt*.o

CURRENT_STEP=$((CURRENT_STEP + 1))
progress_bar "$CURRENT_STEP" "$TOTAL_STEPS" "Done"

cleanup_bar
CLEAN_END=$(date +%s)
CLEAN_DUR=$((CLEAN_END - CLEAN_START))
printf "\033[1;32mClean complete in %ds!\033[0m\n" "$CLEAN_DUR"
