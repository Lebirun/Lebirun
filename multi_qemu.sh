#!/bin/sh
set -e
. ./config.sh

VERBOSE=0
DO_BUILD=0
NO_BUILD=0
MEMORY=1G
SMP=2
LAN_ADDR=230.0.0.1
LAN_PORT=12345

for arg in "$@"; do
  case "$arg" in
    -v|--verbose) VERBOSE=1 ;;
    -b|--build) DO_BUILD=1 ;;
    --no-build) NO_BUILD=1 ;;
    --lan=*) LAN_ADDR="${arg#--lan=}" ;;
    --lan-port=*) LAN_PORT="${arg#--lan-port=}" ;;
    --mem=*) MEMORY="${arg#--mem=}" ;;
    --smp=*) SMP="${arg#--smp=}" ;;
    -h|--help)
      echo "Usage: ./multi_qemu.sh [--build] [--no-build] [--lan=230.0.0.1] [--lan-port=12345] [--mem=1G] [--smp=2] [-v]"
      exit 0
      ;;
  esac
done

if [ "$NO_BUILD" -eq 0 ]; then
    if [ "$DO_BUILD" -eq 1 ] || [ ! -f lebirun.iso ]; then
        printf "\033[1;34mBuilding before launch...\033[0m\n"
        ./build.sh
    fi
fi

MAC_SEED=$$
MAC_A=$(( (MAC_SEED >> 16) & 255 ))
MAC_B=$(( (MAC_SEED >> 8) & 255 ))
MAC_C=$(( MAC_SEED & 255 ))
MAC=$(printf '52:54:00:%02x:%02x:%02x' "$MAC_A" "$MAC_B" "$MAC_C")

printf "\033[1;34mStarting QEMU on LAN %s:%s with MAC %s...\033[0m\n" "$LAN_ADDR" "$LAN_PORT" "$MAC"

QEMU_CMD="qemu-system-x86_64"

if [ "$VERBOSE" -eq 1 ]; then
    printf "  -> %s\n" "$QEMU_CMD"
fi

if [ -t 0 ]; then
    _OLD_STTY="$(stty -g 2>/dev/null || true)"
    cleanup_tty() {
        if [ -n "$_OLD_STTY" ]; then
            stty "$_OLD_STTY" 2>/dev/null || stty sane 2>/dev/null || true
        else
            stty sane 2>/dev/null || true
        fi
    }
    trap cleanup_tty EXIT INT TERM HUP
fi

CDROM_ARGS=""
if [ -f lebirun.iso ]; then
    CDROM_ARGS="-cdrom lebirun.iso"
fi

$QEMU_CMD \
    -m "$MEMORY" \
    -smp "$SMP" \
    -vga qxl \
    $CDROM_ARGS \
    -serial stdio \
    -netdev socket,id=net0,mcast="$LAN_ADDR:$LAN_PORT" \
    -device e1000,netdev=net0,mac="$MAC" \
    -accel kvm \
    -boot d
