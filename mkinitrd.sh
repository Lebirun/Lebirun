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
HEADER_SIZE=16
FILE_HEADER_SIZE=88

TYPE_FILE=0
TYPE_DIR=1

PERM_READ=4
PERM_WRITE=2
PERM_EXEC=1

TMPDIR=$(mktemp -d)
trap "rm -rf $TMPDIR" EXIT

find "$INITRD_DIR" -mindepth 1 \( -type f -o -type d \) ! -name '.gitkeep' | sort > "$TMPDIR/all_entries.txt"
NUM_ENTRIES=$(wc -l < "$TMPDIR/all_entries.txt" | tr -d ' ')

if [ "$NUM_ENTRIES" -eq 0 ]; then
    echo "Warning: No entries found in $INITRD_DIR, creating empty initrd"
fi

echo "Creating initrd with $NUM_ENTRIES entries..."

> "$TMPDIR/path_to_index.txt"
INDEX=0
while IFS= read -r FILEPATH; do
    RELPATH="${FILEPATH#$INITRD_DIR/}"
    echo "$INDEX $RELPATH" >> "$TMPDIR/path_to_index.txt"
    INDEX=$((INDEX + 1))
done < "$TMPDIR/all_entries.txt"

lookup_parent_index() {
    local RELPATH="$1"
    local PARENT_DIR=$(dirname "$RELPATH")
    
    if [ "$PARENT_DIR" = "." ] || [ -z "$PARENT_DIR" ]; then
        echo "65535"
        return
    fi
    
    local PARENT_INDEX=$(grep -E "^[0-9]+ ${PARENT_DIR}$" "$TMPDIR/path_to_index.txt" | head -1 | cut -d' ' -f1)
    
    if [ -z "$PARENT_INDEX" ]; then
        echo "65535"
    else
        echo "$PARENT_INDEX"
    fi
}

rm -f "$OUTPUT"
touch "$OUTPUT"

perl -e 'print pack("V", 0x4452544E)' >> "$OUTPUT"
perl -e 'print pack("V", 2)' >> "$OUTPUT"
perl -e 'print pack("V", $ARGV[0])' "$NUM_ENTRIES" >> "$OUTPUT"
perl -e 'print pack("V", 0)' >> "$OUTPUT"

DATA_OFFSET=$((HEADER_SIZE + NUM_ENTRIES * FILE_HEADER_SIZE))
CURRENT_OFFSET=$DATA_OFFSET

> "$TMPDIR/file_data_list.txt"
while IFS= read -r FILEPATH; do
    NAME=$(basename "$FILEPATH")
    RELPATH="${FILEPATH#$INITRD_DIR/}"
    
    PARENT_INDEX=$(lookup_parent_index "$RELPATH")
    
    if [ -d "$FILEPATH" ]; then
        PERM=$((PERM_READ | PERM_WRITE | PERM_EXEC))
        perl -e 'print substr(pack("a64", $ARGV[0]),0,64)' "$NAME" >> "$OUTPUT"
        perl -e 'print pack("V", 0)' >> "$OUTPUT"  
        perl -e 'print pack("V", 0)' >> "$OUTPUT" 
        perl -e 'print pack("C", $ARGV[0])' "$TYPE_DIR" >> "$OUTPUT"
        perl -e 'print pack("C", $ARGV[0])' "$PERM" >> "$OUTPUT"
        perl -e 'print pack("v", $ARGV[0])' "$PARENT_INDEX" >> "$OUTPUT" 
        perl -e 'print pack("V", 0)' >> "$OUTPUT" 
        perl -e 'print pack("V", 0)' >> "$OUTPUT"  
        perl -e 'print pack("V", 0)' >> "$OUTPUT"  
        echo "  Added DIR: $RELPATH (parent=$PARENT_INDEX)"
    else
        FILE_TO_ADD="$FILEPATH"

        IS_GZ=$(od -An -N2 -tx1 "$FILEPATH" | tr -d ' ' | grep -q "1f8b" && echo "yes" || echo "no")
        if [ "$IS_GZ" = "yes" ]; then
            DECOMP="$TMPDIR/${NAME}.decompressed"
            gunzip -c "$FILEPATH" > "$DECOMP"
            FILE_TO_ADD="$DECOMP"
        fi

        SIZE=$(stat -c%s "$FILE_TO_ADD")

        IS_ELF="no"
        if [ "$(dd if="$FILE_TO_ADD" bs=1 count=4 2>/dev/null | od -An -tx1 | tr -d ' \n')" = "7f454c46" ]; then
            IS_ELF="yes"
        fi
        IS_SHEBANG="no"
        if [ "$(dd if="$FILE_TO_ADD" bs=1 count=2 2>/dev/null | od -An -tx1 | tr -d ' \n')" = "2321" ]; then
            IS_SHEBANG="yes"
        fi
        IN_BIN_DIR="no"
        case "$RELPATH" in
            bin/*|sbin/*|usr/bin/*|usr/sbin/*)
                IN_BIN_DIR="yes"
                ;;
        esac

        if [ -x "$FILEPATH" ] || [ "$IS_ELF" = "yes" ] || [ "$IS_SHEBANG" = "yes" ] || [ "$IN_BIN_DIR" = "yes" ]; then
            PERM=$((PERM_READ | PERM_EXEC))
        else
            PERM=$PERM_READ
        fi
        
        perl -e 'print substr(pack("a64", $ARGV[0]),0,64)' "$NAME" >> "$OUTPUT"
        perl -e 'print pack("V", $ARGV[0])' "$CURRENT_OFFSET" >> "$OUTPUT"
        perl -e 'print pack("V", $ARGV[0])' "$SIZE" >> "$OUTPUT"
        perl -e 'print pack("C", $ARGV[0])' "$TYPE_FILE" >> "$OUTPUT"
        perl -e 'print pack("C", $ARGV[0])' "$PERM" >> "$OUTPUT"
        perl -e 'print pack("v", $ARGV[0])' "$PARENT_INDEX" >> "$OUTPUT"
        perl -e 'print pack("V", 0)' >> "$OUTPUT"
        perl -e 'print pack("V", 0)' >> "$OUTPUT"
        perl -e 'print pack("V", 0)' >> "$OUTPUT"

        echo "  Added FILE: $RELPATH ($SIZE bytes) perm=$PERM parent=$PARENT_INDEX"
        CURRENT_OFFSET=$((CURRENT_OFFSET + SIZE))

        echo "$FILE_TO_ADD" >> "$TMPDIR/file_data_list.txt"
    fi
done < "$TMPDIR/all_entries.txt"

while IFS= read -r RESOLVED; do
    cat "$RESOLVED" >> "$OUTPUT"
done < "$TMPDIR/file_data_list.txt"

TOTAL_SIZE=$(stat -c%s "$OUTPUT")
echo "Successfully created $OUTPUT ($TOTAL_SIZE bytes)"
