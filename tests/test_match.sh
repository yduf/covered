#!/bin/bash
set -e

SCRIPT_DIR="$(cd "$(dirname "$0")" && pwd)"
SRC="/tmp/covered_match_test_src"
BKP="/tmp/covered_match_test_bkp"

# Clean up previous runs
rm -rf "$SRC" "$BKP" covered_tmp_covered_match_test_*

# Create test directories
mkdir -p "$SRC/sub" "$BKP/sub"

# same content in both
printf "hello world same content" > "$SRC/same.txt"
printf "hello world same content" > "$BKP/same.txt"

# unique to source
printf "unique source" > "$SRC/unique.txt"

# different content, same size as unique (actually, let's make it different)
printf "different backup" > "$BKP/different.txt"

# nested same
printf "nested same" > "$SRC/sub/nested.txt"
printf "nested same" > "$BKP/sub/nested.txt"

# Scan both
"$SCRIPT_DIR/../build/covered_scan_size" "$SRC"
"$SCRIPT_DIR/../build/covered_scan_size" "$BKP"

# Run match
"$SCRIPT_DIR/../build/covered_match" covered_tmp_covered_match_test_src covered_tmp_covered_match_test_bkp

# Verify results via sqlite3
RESULT=$(sqlite3 covered_tmp_covered_match_test_src/filesize.db "SELECT files.name, covered FROM files JOIN dirs ON files.dir_inode = dirs.inode ORDER BY files.name;")

# Check expected outcomes
if ! echo "$RESULT" | grep -q "nested.txt|1"; then
    echo "FAIL: nested.txt should be covered"
    exit 1
fi
if ! echo "$RESULT" | grep -q "same.txt|1"; then
    echo "FAIL: same.txt should be covered"
    exit 1
fi
if ! echo "$RESULT" | grep -q "unique.txt|0"; then
    echo "FAIL: unique.txt should NOT be covered"
    exit 1
fi

echo "PASS: all match assertions passed"

# Cleanup
rm -rf "$SRC" "$BKP" covered_tmp_covered_match_test_*
