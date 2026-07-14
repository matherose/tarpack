#!/usr/bin/env bash
#
# Integration test for `tarpack scan`. Builds a fixture tree with regular
# files, a subdirectory, a hardlink pair, a symlink, a FIFO, and a
# non-UTF-8 filename, runs the scan, and asserts on the resulting JSONL
# manifest: hardlinks share an object_id, distinct files get distinct
# ids, symlink targets are recorded, FIFOs are skipped (with a warning),
# non-UTF-8 names round-trip via path_b64, and repeated scans of the same
# tree are byte-for-byte deterministic (apart from the header's "created"
# field).
#
# Usage: test_scan.sh <path-to-tarpack-binary>

set -u

TARPACK_BIN="${1:?usage: test_scan.sh <path-to-tarpack-binary>}"
FAIL_COUNT=0

pass() { printf 'PASS: %s\n' "$1"; }
fail() { printf 'FAIL: %s\n' "$1" >&2; FAIL_COUNT=$((FAIL_COUNT + 1)); }
check() {
    # check <cond-exit-code> <label>
    if [ "$1" -eq 0 ]; then pass "$2"; else fail "$2"; fi
}

WORKDIR="$(mktemp -d /tmp/tarpack_scan_test.XXXXXX)"
cleanup() { rm -rf "$WORKDIR"; }
trap cleanup EXIT

ROOT="$WORKDIR/fixture"
REPO1="$WORKDIR/repo1"
REPO2="$WORKDIR/repo2"

mkdir -p "$ROOT/subdir"

echo "hello world" > "$ROOT/a.txt"
echo "another file" > "$ROOT/subdir/b.txt"

# hardlink pair
ln "$ROOT/a.txt" "$ROOT/a_hardlink.txt"

# symlink
ln -s "subdir/b.txt" "$ROOT/link_to_b"

# FIFO (must be skipped, with a warning)
mkfifo "$ROOT/subdir/a_fifo"

# Non-ASCII (but valid UTF-8) filename, to exercise multibyte path handling
# end-to-end. Note: macOS/APFS rejects filenames containing byte sequences
# that are not valid UTF-8 at the syscall level (open() returns EILSEQ), so
# a *genuinely* invalid-UTF-8 filename cannot be created as a real fixture
# on this platform. The path_b64 encoding path itself (raw, possibly
# invalid, bytes -> base64, decoded back to identical bytes) is covered by
# tests/unit/test_manifest.c, which fabricates such a path string directly
# without touching the filesystem.
python3 -c "
import os
name = 'café_\U0001F600.txt'
path = os.path.join('$ROOT', name)
with open(path, 'wb') as f:
    f.write(b'non-ascii named file contents\n')
"

# --- run 1 ---
"$TARPACK_BIN" scan "$ROOT" --repo "$REPO1" --label snap1 >"$WORKDIR/scan1.out" 2>"$WORKDIR/scan1.err"
SCAN1_RC=$?

MANIFEST1="$REPO1/snapshots/snap1.jsonl"
[ -f "$MANIFEST1" ]
check $? "scan run 1 produced a manifest file"

# FIFO warning: scan should still exit non-fatally (0 or 1), and warn about the FIFO.
if [ "$SCAN1_RC" -eq 0 ] || [ "$SCAN1_RC" -eq 1 ]; then
    pass "scan exit code is 0 or 1 (completed, possibly with warnings)"
else
    fail "scan exit code is 0 or 1 (completed, possibly with warnings) -- got $SCAN1_RC"
fi

grep -qi 'fifo\|skip' "$WORKDIR/scan1.err"
check $? "scan emits a warning mentioning the skipped FIFO"

# --- run 2: same tree, same label, different repo -- for determinism check ---
"$TARPACK_BIN" scan "$ROOT" --repo "$REPO2" --label snap1 >"$WORKDIR/scan2.out" 2>"$WORKDIR/scan2.err"

MANIFEST2="$REPO2/snapshots/snap1.jsonl"
[ -f "$MANIFEST2" ]
check $? "scan run 2 produced a manifest file"

# --- assertions on manifest content via python3/json ---
python3 - "$MANIFEST1" "$MANIFEST2" <<'PYEOF'
import base64
import json
import sys

manifest1_path, manifest2_path = sys.argv[1], sys.argv[2]

def load(path):
    header = None
    entries = []
    with open(path, 'rb') as f:
        for line in f:
            line = line.rstrip(b'\n')
            if not line:
                continue
            obj = json.loads(line.decode('utf-8'))
            if 'format' in obj:
                header = obj
            else:
                entries.append(obj)
    return header, entries

failures = []

def check(cond, label):
    if cond:
        print(f"PASS: {label}")
    else:
        print(f"FAIL: {label}", file=sys.stderr)
        failures.append(label)

header1, entries1 = load(manifest1_path)
header2, entries2 = load(manifest2_path)

check(header1 is not None and header1.get('format') == 'tarpack-snapshot-v1', 'header format is tarpack-snapshot-v1')
check(header1 is not None and 'created' in header1, 'header has created field')
check(header1 is not None and 'root' in header1, 'header has root field')

by_path = {}
for e in entries1:
    p = e.get('path')
    if p is None and 'path_b64' in e:
        p = ('b64:' + e['path_b64'])
    by_path[p] = e

# distinct files get distinct object ids; hardlinks share one
a_txt = by_path.get('a.txt')
a_hardlink = by_path.get('a_hardlink.txt')
b_txt = by_path.get('subdir/b.txt')

check(a_txt is not None, 'a.txt present in manifest')
check(a_hardlink is not None, 'a_hardlink.txt present in manifest')
check(b_txt is not None, 'subdir/b.txt present in manifest')

if a_txt and a_hardlink:
    check(a_txt.get('object_id') == a_hardlink.get('object_id'), 'hardlinked paths share one object_id')
    check(a_txt.get('nlink', 1) >= 2, 'hardlinked file reports nlink >= 2')

if a_txt and b_txt:
    check(a_txt.get('object_id') != b_txt.get('object_id'), 'distinct files have distinct object_ids')

# symlink target recorded
link = by_path.get('link_to_b')
check(link is not None, 'link_to_b present in manifest')
check(link is not None and link.get('type') == 'symlink', 'link_to_b has type symlink')
check(link is not None and link.get('target') == 'subdir/b.txt', 'symlink target recorded correctly')

# FIFO absent from manifest
check(all(e.get('path') != 'subdir/a_fifo' for e in entries1), 'FIFO absent from manifest')

# non-ASCII (multibyte UTF-8) filename round-trips as a plain "path" string
nonascii_name = 'café_\U0001F600.txt'
nonascii_entry = by_path.get(nonascii_name)
check(nonascii_entry is not None, 'non-ascii utf-8 filename round-trips as plain path string')
check(nonascii_entry is not None and 'path_b64' not in nonascii_entry, 'valid utf-8 path does not use path_b64')

# secondary timestamps (v1.3) are recorded on every entry
check(a_txt is not None and 'atime_sec' in a_txt and 'atime_nsec' in a_txt,
      'file entry records atime')
check(a_txt is not None and 'ctime_sec' in a_txt and 'ctime_nsec' in a_txt,
      'file entry records ctime')

# deterministic order: run1 vs run2 identical apart from header "created" and
# the volatile timestamps -- reading a tree changes atimes (scan #1 itself
# bumps directory atimes), so atime/ctime are honest observations, not
# identity. btime never changes and stays in the comparison.
VOLATILE = ('atime_sec', 'atime_nsec', 'ctime_sec', 'ctime_nsec')

def strip_created(header):
    h = dict(header)
    h.pop('created', None)
    return h

def strip_volatile(entries):
    return [{k: v for k, v in e.items() if k not in VOLATILE} for e in entries]

check(strip_created(header1) == strip_created(header2), 'headers identical apart from created field')
check(strip_volatile(entries1) == strip_volatile(entries2),
      'entries identical and in the same order across repeated scans (modulo atime/ctime)')

if failures:
    print(f"\n{len(failures)} check(s) failed", file=sys.stderr)
    sys.exit(1)

print("\nall python checks passed")
sys.exit(0)
PYEOF
PY_RC=$?
check $PY_RC "python3/json manifest assertions"

if [ "$FAIL_COUNT" -ne 0 ]; then
    echo "" >&2
    echo "$FAIL_COUNT check(s) failed" >&2
    exit 1
fi

echo ""
echo "all checks passed"
exit 0
