#!/bin/sh
set -e

INITRD_DIR="$1"
OUTPUT="$2"

if [ -z "$INITRD_DIR" ] || [ -z "$OUTPUT" ]; then
    echo "Usage: $0 <directory> <output.img>"
    exit 1
fi

if [ ! -d "$INITRD_DIR" ]; then
    echo "Error: Directory '$INITRD_DIR' does not exist"
    exit 1
fi

MAGIC=0x4452544E
VERSION=2
HEADER_SIZE=16
FILE_HEADER_SIZE=84

TYPE_FILE=0
TYPE_DIR=1

PERM_READ=4
PERM_EXEC=1

TMPDIR=$(mktemp -d)
trap "rm -rf $TMPDIR" EXIT

find "$INITRD_DIR" -mindepth 1 -type f | sort > "$TMPDIR/all_files.txt"
NUM_FILES=$(wc -l < "$TMPDIR/all_files.txt" | tr -d ' ')

if [ "$NUM_FILES" -eq 0 ]; then
    echo "No files found in $INITRD_DIR"
    exit 1
fi

echo "Creating initrd v2 with $NUM_FILES files..."

rm -f "$OUTPUT"
touch "$OUTPUT"

perl -e 'print pack("V", 0x4452544E)' >> "$OUTPUT"
perl -e 'print pack("V", $ARGV[0])' "$VERSION" >> "$OUTPUT"
perl -e 'print pack("V", $ARGV[0])' "$NUM_FILES" >> "$OUTPUT"
perl -e 'print pack("V", 0)' >> "$OUTPUT"

DATA_OFFSET=$((HEADER_SIZE + NUM_FILES * FILE_HEADER_SIZE))
CURRENT_OFFSET=$DATA_OFFSET

while IFS= read -r FILEPATH; do
    NAME=$(basename "$FILEPATH")
    SIZE=$(stat -c%s "$FILEPATH")
    
    if [ -x "$FILEPATH" ]; then
        PERM=$((PERM_READ | PERM_EXEC))
    else
        PERM=$PERM_READ
    fi
    
    perl -e 'print substr(pack("a64", $ARGV[0]),0,64)' "$NAME" >> "$OUTPUT"
    perl -e 'print pack("V", $ARGV[0])' "$CURRENT_OFFSET" >> "$OUTPUT"
    perl -e 'print pack("V", $ARGV[0])' "$SIZE" >> "$OUTPUT"
    perl -e 'print pack("C", $ARGV[0])' "$TYPE_FILE" >> "$OUTPUT"
    perl -e 'print pack("C", $ARGV[0])' "$PERM" >> "$OUTPUT"
    perl -e 'print pack("v", 65535)' >> "$OUTPUT"
    perl -e 'print pack("V", 0)' >> "$OUTPUT"
    perl -e 'print pack("V", 0)' >> "$OUTPUT"
    perl -e 'print pack("V", 0)' >> "$OUTPUT"
    
    echo "  Added: $NAME ($SIZE bytes) perm=$PERM"
    CURRENT_OFFSET=$((CURRENT_OFFSET + SIZE))
done < "$TMPDIR/all_files.txt"

while IFS= read -r FILEPATH; do
    cat "$FILEPATH" >> "$OUTPUT"
done < "$TMPDIR/all_files.txt"

TOTAL_SIZE=$(stat -c%s "$OUTPUT")
echo "Created $OUTPUT ($TOTAL_SIZE bytes)"
