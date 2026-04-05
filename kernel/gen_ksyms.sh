#!/bin/sh
set -e

KERNEL_ELF="$1"
OUTPUT="$2"
NM="${3:-nm}"

if [ -z "$KERNEL_ELF" ] || [ -z "$OUTPUT" ]; then
    echo "Usage: $0 <kernel.elf> <output.c> [nm]" >&2
    exit 1
fi

TMP=$(mktemp)

$NM --defined-only -g "$KERNEL_ELF" | awk '$2 == "T" || $2 == "t" { print $3 }' | \
    grep -v '^\.' | grep -v '^_' | sort -u > "$TMP"

cat > "$OUTPUT" <<'HDR'
#include <stdint.h>

typedef struct {
    const char *name;
    uint64_t addr;
} lke_ksym_auto_t;

HDR

while IFS= read -r sym; do
    printf 'extern char %s[];\n' "$sym" >> "$OUTPUT"
done < "$TMP"

printf '\nconst lke_ksym_auto_t ksym_auto_table[] = {\n' >> "$OUTPUT"

while IFS= read -r sym; do
    printf '    {"%s", (uint64_t)%s},\n' "$sym" "$sym" >> "$OUTPUT"
done < "$TMP"

printf '    {0, 0}\n};\n' >> "$OUTPUT"

COUNT=$(wc -l < "$TMP")
printf 'const int ksym_auto_count = %d;\n' "$COUNT" >> "$OUTPUT"

rm -f "$TMP"
