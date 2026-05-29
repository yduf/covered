#!/bin/bash
set -e

SCRIPT_DIR="$(cd "$(dirname "$0")" && pwd)"
SRC="/tmp/covered_match_test_src spaces"
BKP="/tmp/covered_match_test_bkp spaces"

# Clean up previous runs
rm -rf "$SRC" "$BKP" "coverdb/tmp_covered_match_test_src spaces" "coverdb/tmp_covered_match_test_bkp spaces"

# Create test directories with spaces in names
mkdir -p "$SRC/sub folder" "$BKP/sub folder"
mkdir -p "$SRC/sub folder/nested dir" "$BKP/sub folder/nested dir"

# same content in both
printf "hello world same content" > "$SRC/same.txt"
printf "hello world same content" > "$BKP/same.txt"

# file with space in name
printf "file with spaces content" > "$SRC/file with spaces.txt"
printf "file with spaces content" > "$BKP/file with spaces.txt"

# unique to source
printf "unique source" > "$SRC/unique.txt"

# different content, same size as unique
printf "different backup" > "$BKP/different.txt"

# nested same with space in filename
printf "nested same" > "$SRC/sub folder/nested.txt"
printf "nested same" > "$BKP/sub folder/nested.txt"

# deeply nested with spaces
printf "deep nested" > "$SRC/sub folder/nested dir/deep file.txt"
printf "deep nested" > "$BKP/sub folder/nested dir/deep file.txt"

# Scan both
"$SCRIPT_DIR/../build/covered_scan_size" "$SRC"
"$SCRIPT_DIR/../build/covered_scan_size" "$BKP"

# Run match
"$SCRIPT_DIR/../build/covered_match" "coverdb/tmp_covered_match_test_src spaces" "coverdb/tmp_covered_match_test_bkp spaces"

# Verify results via sqlite3
SRC_DB="coverdb/tmp_covered_match_test_src spaces/filesize.db"
RESULT=$(sqlite3 "$SRC_DB" "SELECT files.name, covered FROM files JOIN dirs ON files.dir_inode = dirs.inode ORDER BY files.name;")

echo "Results:"
echo "$RESULT"

# Check expected outcomes
if ! echo "$RESULT" | grep -q "nested.txt|1"; then
    echo "FAIL: nested.txt should be covered"
    exit 1
fi
if ! echo "$RESULT" | grep -q "same.txt|1"; then
    echo "FAIL: same.txt should be covered"
    exit 1
fi
if ! echo "$RESULT" | grep -q "file with spaces.txt|1"; then
    echo "FAIL: 'file with spaces.txt' should be covered"
    exit 1
fi
if ! echo "$RESULT" | grep -q "deep file.txt|1"; then
    echo "FAIL: 'deep file.txt' should be covered"
    exit 1
fi
if ! echo "$RESULT" | grep -q "unique.txt|0"; then
    echo "FAIL: unique.txt should NOT be covered"
    exit 1
fi

echo "PASS: all match assertions passed with spaces"

# Cleanup
rm -rf "$SRC" "$BKP" "coverdb/tmp_covered_match_test_src spaces" "coverdb/tmp_covered_match_test_bkp spaces"