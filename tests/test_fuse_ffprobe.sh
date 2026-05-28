#!/bin/bash
# Test: FUSE filesystem must support video probing via ffprobe.
set -eu

SCRIPT_DIR="$(cd "$(dirname "$0")" && pwd)"
SRC="/tmp/covered_fuse_ffprobe_test_src"
BKP="/tmp/covered_fuse_ffprobe_test_bkp"
MNT="/tmp/covered_fuse_ffprobe_test_mnt"

# Clean up previous runs
fusermount3 -uz "$MNT" 2>/dev/null || true
rm -rf "$SRC" "$BKP" "$MNT" covered_tmp_covered_fuse_ffprobe_test_*
mkdir -p "$SRC" "$BKP" "$MNT"

# Check prerequisites
if ! command -v ffprobe &>/dev/null; then
    echo "SKIP: ffprobe not installed"
    rm -rf "$SRC" "$BKP" "$MNT" covered_tmp_covered_fuse_ffprobe_test_*
    exit 77
fi

if ! command -v ffmpeg &>/dev/null; then
    echo "SKIP: ffmpeg not installed (needed to generate test video)"
    rm -rf "$SRC" "$BKP" "$MNT" covered_tmp_covered_fuse_ffprobe_test_*
    exit 77
fi

# Generate a short MPEG file (timeout guards against hung ffmpeg)
VIDEO_FILE="$SRC/video.mpg"
if ! timeout 10 ffmpeg -y \
    -f lavfi -i "color=c=red:size=32x32:duration=0.1" \
    -c:v mpeg1video -b:v 50k \
    -f mpeg "$VIDEO_FILE" 2>/dev/null; then
    echo "SKIP: ffmpeg could not generate test video"
    rm -rf "$SRC" "$BKP" "$MNT" covered_tmp_covered_fuse_ffprobe_test_*
    exit 77
fi

# Verify the generated file is parseable by ffprobe on real FS
if ! timeout 10 ffprobe "$VIDEO_FILE" >/dev/null 2>&1; then
    echo "SKIP: generated video is not parseable by ffprobe"
    rm -rf "$SRC" "$BKP" "$MNT" covered_tmp_covered_fuse_ffprobe_test_*
    exit 77
fi

FILESIZE=$(stat -c%s "$VIDEO_FILE")
echo "Generated test MPEG: $FILESIZE bytes"

# Copy to backup so it gets marked as "covered"
cp "$VIDEO_FILE" "$BKP/video.mpg"

# Also create an uncovered file (not in backup)
printf "just a text file" > "$SRC/notes.txt"

# Scan both
"$SCRIPT_DIR/../build/covered_scan_size" "$SRC"
"$SCRIPT_DIR/../build/covered_scan_size" "$BKP"

# Match
"$SCRIPT_DIR/../build/covered_match" covered_tmp_covered_fuse_ffprobe_test_src covered_tmp_covered_fuse_ffprobe_test_bkp

# Run cover_report to compute dir coverage
"$SCRIPT_DIR/../build/cover_report" covered_tmp_covered_fuse_ffprobe_test_src

# Run FUSE in background
"$SCRIPT_DIR/../build/cover_fuse" covered_tmp_covered_fuse_ffprobe_test_src "$MNT" &
FUSE_PID=$!

# Wait for mount to be ready
for i in $(seq 1 30); do
    if ls "$MNT" >/dev/null 2>&1; then
        break
    fi
    sleep 0.1
done

PASS=0
FAIL=0

echo ""
echo "--- Testing FUSE ffprobe compatibility ---"

# Test 1: ffprobe can open the video file through FUSE
if timeout 10 ffprobe "$MNT/video.mpg" >/dev/null 2>&1; then
    echo "  OK: ffprobe opens video through FUSE"
    PASS=$((PASS + 1))
else
    echo "  FAIL: ffprobe cannot open video through FUSE"
    FAIL=$((FAIL + 1))
fi

# Test 2: FUSE reports correct file size
SIZE_FUSE=$(stat -c%s "$MNT/video.mpg" 2>/dev/null || echo 0)
if [ "$SIZE_FUSE" -eq "$FILESIZE" ] && [ "$SIZE_FUSE" -gt 0 ]; then
    echo "  OK: FUSE reports correct file size ($SIZE_FUSE bytes)"
    PASS=$((PASS + 1))
else
    echo "  FAIL: size mismatch (FUSE=$SIZE_FUSE, expected=$FILESIZE)"
    FAIL=$((FAIL + 1))
fi

# Test 3: md5sum matches through FUSE
MD5_FUSE=$(md5sum "$MNT/video.mpg" | cut -d' ' -f1)
MD5_REAL=$(md5sum "$VIDEO_FILE" | cut -d' ' -f1)
if [ "$MD5_FUSE" = "$MD5_REAL" ]; then
    echo "  OK: FUSE passes through correct content (md5=$MD5_FUSE)"
    PASS=$((PASS + 1))
else
    echo "  FAIL: content mismatch (FUSE=$MD5_FUSE, real=$MD5_REAL)"
    FAIL=$((FAIL + 1))
fi

# Unmount
fusermount3 -uz "$MNT" 2>/dev/null || true
kill $FUSE_PID 2>/dev/null || true
wait $FUSE_PID 2>/dev/null || true

# Cleanup
rm -rf "$SRC" "$BKP" "$MNT" covered_tmp_covered_fuse_ffprobe_test_*

echo ""
echo "--- Results: $PASS passed, $FAIL failed ---"
if [ "$FAIL" -gt 0 ]; then
    exit 1
fi
echo "PASS: all FUSE ffprobe assertions passed"