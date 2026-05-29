#!/bin/bash
set -e

SCRIPT_DIR="$(cd "$(dirname "$0")" && pwd)"
SRC="/tmp/covered_fuse_test_src"
BKP="/tmp/covered_fuse_test_bkp"
MNT="/tmp/covered_fuse_test_mnt"

# Clean up previous runs
rm -rf "$SRC" "$BKP" "$MNT" coverdb/tmp_covered_fuse_test_src coverdb/tmp_covered_fuse_test_bkp
mkdir -p "$SRC/sub/deep" "$BKP/sub/deep" "$MNT"

# Create test files
printf "hello world same content"  > "$SRC/same.txt"
printf "hello world same content"  > "$BKP/same.txt"
printf "unique source file"        > "$SRC/unique.txt"
printf "nested covered file"       > "$SRC/sub/nested.txt"
printf "nested covered file"       > "$BKP/sub/nested.txt"
printf "deep covered file"         > "$SRC/sub/deep/deep.txt"
printf "deep covered file"         > "$BKP/sub/deep/deep.txt"

# Scan both
"$SCRIPT_DIR/../build/covered_scan_size" "$SRC"
"$SCRIPT_DIR/../build/covered_scan_size" "$BKP"

# Match
"$SCRIPT_DIR/../build/covered_match" coverdb/tmp_covered_fuse_test_src coverdb/tmp_covered_fuse_test_bkp

# Run FUSE in background
"$SCRIPT_DIR/../build/cover_fuse" coverdb/tmp_covered_fuse_test_src "$MNT" -f &
FUSE_PID=$!

# Wait for mount to be ready (poll until root xattr is readable)
for i in $(seq 1 30); do
    if getfattr -n user.covered "$MNT" >/dev/null 2>&1; then
        break
    fi
    sleep 0.1
done

PASS=0
FAIL=0

check_xattr() {
    local filepath="$1"
    local xattr_name="$2"
    local expected="$3"
    local desc="$4"
    local full="$MNT/$filepath"

    local val
    val=$(getfattr --only-values -n "$xattr_name" "$full" 2>/dev/null || echo "ERROR")
    if [ "$val" = "$expected" ]; then
        echo "  OK: $desc ($xattr_name=$val)"
        PASS=$((PASS + 1))
    else
        echo "  FAIL: $desc ($xattr_name: expected='$expected', got='$val')"
        FAIL=$((FAIL + 1))
    fi
}

check_noattr() {
    local filepath="$1"
    local xattr_name="$2"
    local desc="$3"
    local full="$MNT/$filepath"

    if getfattr -n "$xattr_name" "$full" >/dev/null 2>&1; then
        echo "  FAIL: $desc (xattr $xattr_name should NOT exist but was found)"
        FAIL=$((FAIL + 1))
    else
        echo "  OK: $desc (no $xattr_name)"
        PASS=$((PASS + 1))
    fi
}


echo ""
echo "--- Testing FUSE xattrs ---"

# Root directory
check_xattr "" "user.covered"      "partial"      "root dir coverage"
check_noattr "" "user.covered_backup"              "root dir has no backup"

# Covered file: same.txt
check_xattr "same.txt" "user.covered"       "covered"                       "covered file coverage"
check_xattr "same.txt" "user.covered_backup" "/tmp/covered_fuse_test_bkp"    "covered file backup path"
check_xattr "same.txt" "user.covered_at"    "/tmp/covered_fuse_test_bkp/same.txt" "covered file covered_at"

# Covered nested file
check_xattr "sub/nested.txt" "user.covered"       "covered"                       "nested covered file coverage"
check_xattr "sub/nested.txt" "user.covered_backup" "/tmp/covered_fuse_test_bkp"    "nested file backup path"
check_xattr "sub/nested.txt" "user.covered_at"    "/tmp/covered_fuse_test_bkp/sub/nested.txt" "nested covered file covered_at"

# Uncovered file
check_xattr "unique.txt" "user.covered" "uncovered"           "uncovered file coverage"
check_noattr "unique.txt" "user.covered_backup"                "uncovered file has no backup"
check_noattr "unique.txt" "user.covered_at"                    "uncovered file has no covered_at"

# Unmount
fusermount3 -u "$MNT" 2>/dev/null || true
kill $FUSE_PID 2>/dev/null || true
wait $FUSE_PID 2>/dev/null || true

# Cleanup
rm -rf "$SRC" "$BKP" "$MNT" coverdb/tmp_covered_fuse_test_src coverdb/tmp_covered_fuse_test_bkp

echo ""
echo "--- Results: $PASS passed, $FAIL failed ---"
if [ "$FAIL" -gt 0 ]; then
    exit 1
fi
echo "PASS: all FUSE xattr assertions passed"