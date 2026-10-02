#!/bin/sh
set -e
. ./config.sh

VERBOSE=0
DO_BUILD=0
NO_BUILD=0
DISK=0
DEBUG=0
UEFI=0
ISO_ARGS=""
for arg in "$@"; do
  case "$arg" in
    -v|--verbose) VERBOSE=1; ISO_ARGS="$ISO_ARGS -v" ;;
    -b|--build) DO_BUILD=1 ;;
    --no-build) NO_BUILD=1 ;;
    --disk) DISK=1 ;;
    --debug) DEBUG=1 ;;
    --uefi) UEFI=1 ;;
  esac
done

if [ "$NO_BUILD" -eq 0 ]; then
    if [ "$DO_BUILD" -eq 1 ]; then
        printf "\033[1;34mBuilding ISO before launch...\033[0m\n"
        ./iso.sh $ISO_ARGS
    elif [ ! -f lebirun.iso ]; then
        printf "\033[1;34mNo ISO found, building...\033[0m\n"
        ./iso.sh $ISO_ARGS
    fi
fi

if [ "$NO_BUILD" -eq 0 ] && [ ! -f lebirun.iso ]; then
    printf "\033[1;31mError: lebirun.iso was not created.\033[0m\n"
    exit 1
fi

printf "\033[1;34mStarting QEMU...\033[0m\n"

QEMU_CMD="qemu-system-x86_64"

if [ "$VERBOSE" -eq 1 ]; then
    printf "  -> %s %s\n" "$QEMU_CMD" "(see below)"
fi

if [ -t 0 ]; then
    _OLD_STTY="$(stty -g 2>/dev/null || true)"
    cleanup_tty() {
        if [ -n "$_OLD_STTY" ]; then
            stty "$_OLD_STTY" 2>/dev/null || stty sane 2>/dev/null || true
        else
            stty sane 2>/dev/null || true
        fi
        if [ -n "$UEFI_VARS" ]; then
            rm -f "$UEFI_VARS" 2>/dev/null || true
        fi
    }
    trap cleanup_tty EXIT INT TERM HUP
fi

UEFI_VARS=""
UEFI_ARGS=""
if [ "$UEFI" -eq 1 ]; then
    OVMF_CODE=""
    OVMF_VARS_TMPL=""
    for _c in /usr/share/OVMF/OVMF_CODE_4M.fd /usr/share/ovmf/OVMF_CODE.fd; do
        if [ -f "$_c" ]; then OVMF_CODE="$_c"; break; fi
    done
    for _v in /usr/share/OVMF/OVMF_VARS_4M.fd /usr/share/ovmf/OVMF_VARS.fd; do
        if [ -f "$_v" ]; then OVMF_VARS_TMPL="$_v"; break; fi
    done
    if [ -n "$OVMF_CODE" ] && [ -n "$OVMF_VARS_TMPL" ]; then
        UEFI_VARS="$(mktemp /tmp/lebirun-ovmf-vars-XXXXXX.fd)"
        cp "$OVMF_VARS_TMPL" "$UEFI_VARS"
        UEFI_ARGS="-drive if=pflash,format=raw,unit=0,file=$OVMF_CODE,readonly=on -drive if=pflash,format=raw,unit=1,file=$UEFI_VARS"
    elif [ -f /usr/share/ovmf/OVMF.fd ]; then
        UEFI_ARGS="-bios /usr/share/ovmf/OVMF.fd"
    else
        printf "\033[1;31mError: OVMF firmware not found. Install ovmf.\033[0m\n"
        exit 1
    fi
    if [ ! -t 0 ] && [ -n "$UEFI_VARS" ]; then
        trap 'rm -f "$UEFI_VARS" 2>/dev/null || true' EXIT INT TERM HUP
    fi
    printf "\033[1;34mUsing UEFI firmware (%s)...\033[0m\n" "${OVMF_CODE:-/usr/share/ovmf/OVMF.fd}"
fi

CDROM_ARGS=""
if [ "$NO_BUILD" -eq 0 ]; then
    CDROM_ARGS="-cdrom lebirun.iso"
fi

DISK_ARGS=""
if [ "$DISK" -eq 1 ]; then
    DISK_ARGS="-drive file=sata_disk.qcow2,if=none,id=sata0,format=qcow2 -device ide-hd,drive=sata0,bus=ahci0.0"
fi

DEBUG_ARGS=""
if [ "$DEBUG" -eq 1 ]; then
    DEBUG_ARGS="-s -S"
fi

$QEMU_CMD \
    -m 4G \
    -smp 4 \
    -cpu host \
    -vga qxl \
    $CDROM_ARGS \
    $DEBUG_ARGS \
    $UEFI_ARGS \
    -serial stdio \
    -device ahci,id=ahci0 \
    $DISK_ARGS \
    -netdev user,id=net0,hostfwd=tcp::5555-:80 \
    -device virtio-net,netdev=net0 \
    -accel kvm \
    -boot d
