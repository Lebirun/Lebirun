#!/usr/bin/env bash
set -euo pipefail

ROOT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
LEBCONFIG_DIR="$ROOT_DIR/lebconfig"
LEBCONFIG_BIN="$LEBCONFIG_DIR/lebconfig"
LEBCONFIG_FILE="$ROOT_DIR/Lebconfig"

usage() {
	echo "Usage: $0 [clean|help]"
	echo ""
	echo "Commands:"
	echo "  clean   Clean lebconfig build artifacts"
	echo "  help    Show this help message"
}

case "${1:-}" in
	clean)
		make -C "$LEBCONFIG_DIR" clean
		exit 0
		;;
	help|-h|--help)
		usage
		exit 0
		;;
	"")
		;;
	*)
		echo "Unknown command: $1" >&2
		usage
		exit 1
		;;
esac

if [[ ! -f "$LEBCONFIG_FILE" ]]; then
	echo "Error: Lebconfig file not found at $LEBCONFIG_FILE" >&2
	exit 1
fi

if [[ ! -x "$LEBCONFIG_BIN" ]]; then
	make -C "$LEBCONFIG_DIR"
fi

cd "$ROOT_DIR" && exec "$LEBCONFIG_BIN" "$LEBCONFIG_FILE"
