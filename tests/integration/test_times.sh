#!/usr/bin/env bash
#
# Integration test for secondary-timestamp capture and restore (v1.3).
#
# atime is recorded at scan time and restored via utimensat; the creation
# time (btime) is recorded where the OS exposes one (st_birthtime on
# macOS/BSD, statx on Linux) and restored on macOS/BSD only -- Linux has no
# API to set it. ctime is recorded as informational. Asserts:
#   - the snapshot manifest carries atime_sec/ctime_sec (and btime_sec where
#     the platform provides it);
#   - a restored file's atime and mtime equal the values captured before the
#     scan (whole seconds; nanosecond mtime precision is covered by
#     test_roundtrip);
#   - on macOS/BSD, the restored file's creation date equals the source's;
#   - a restored directory keeps its mtime.
#
# Usage: test_times.sh <path-to-tarpack-binary>

set -u

TARPACK_BIN="${1:?usage: test_times.sh <path-to-tarpack-binary>}"
FAIL_COUNT=0

pass() { printf 'PASS: %s\n' "$1"; }
fail() { printf 'FAIL: %s\n' "$1" >&2; FAIL_COUNT=$((FAIL_COUNT + 1)); }
check() {
    if [ "$1" -eq 0 ]; then pass "$2"; else fail "$2"; fi
}

WORKDIR="$(mktemp -d /tmp/tarpack_times_test.XXXXXX)"
cleanup() { rm -rf "$WORKDIR"; }
trap cleanup EXIT

ROOT="$WORKDIR/fixture"
REPO="$WORKDIR/repo"
DEST="$WORKDIR/out"
mkdir -p "$ROOT/sub"

printf 'hello timestamps\n' > "$ROOT/f.txt"

# distinct, known mtime and atime (mtime first: touch -t sets both)
touch -t 202001011200.30 "$ROOT/f.txt"
touch -a -t 202106050830.15 "$ROOT/f.txt"
touch -t 202001020304.05 "$ROOT/sub"

# stat flavor detection (BSD vs GNU); only BSD stat exposes the birth time
if stat -f '%m' "$ROOT/f.txt" >/dev/null 2>&1; then
    stat_at() { stat -f '%a' "$1"; }
    stat_mt() { stat -f '%m' "$1"; }
    stat_bt() { stat -f '%B' "$1"; }
    HAVE_BT=1
else
    stat_at() { stat -c '%X' "$1"; }
    stat_mt() { stat -c '%Y' "$1"; }
    stat_bt() { echo 0; }
    HAVE_BT=0
fi

SRC_AT=$(stat_at "$ROOT/f.txt")
SRC_MT=$(stat_mt "$ROOT/f.txt")
SRC_BT=$(stat_bt "$ROOT/f.txt")
SUB_MT=$(stat_mt "$ROOT/sub")

"$TARPACK_BIN" scan "$ROOT" --repo "$REPO" --label snap >/dev/null 2>&1
check $? "scan succeeds"

MANIFEST="$REPO/snapshots/snap.jsonl"
grep -q '"atime_sec"' "$MANIFEST"
check $? "manifest records atime"
grep -q '"ctime_sec"' "$MANIFEST"
check $? "manifest records ctime"
if [ "$HAVE_BT" -eq 1 ]; then
    grep -q '"btime_sec"' "$MANIFEST"
    check $? "manifest records btime (platform exposes creation time)"
fi

"$TARPACK_BIN" pack --repo "$REPO" >/dev/null 2>&1
check $? "pack succeeds"
"$TARPACK_BIN" restore --repo "$REPO" --dest "$DEST" >/dev/null 2>&1
check $? "restore succeeds"

REST_AT=$(stat_at "$DEST/f.txt")
REST_MT=$(stat_mt "$DEST/f.txt")
[ "$REST_MT" = "$SRC_MT" ]
check $? "restored mtime matches source ($REST_MT vs $SRC_MT)"
[ "$REST_AT" = "$SRC_AT" ]
check $? "restored atime matches source ($REST_AT vs $SRC_AT)"

if [ "$HAVE_BT" -eq 1 ]; then
    REST_BT=$(stat_bt "$DEST/f.txt")
    [ "$REST_BT" = "$SRC_BT" ]
    check $? "restored creation date matches source ($REST_BT vs $SRC_BT)"
fi

REST_SUB_MT=$(stat_mt "$DEST/sub")
[ "$REST_SUB_MT" = "$SUB_MT" ]
check $? "restored directory mtime matches source"

if [ "$FAIL_COUNT" -gt 0 ]; then
    printf '%d check(s) failed\n' "$FAIL_COUNT" >&2
    exit 1
fi
printf 'all checks passed\n'
exit 0
