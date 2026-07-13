#!/usr/bin/env bash
#
# Integration test for milestone v0.3: object index, cross-run dedup, and
# "skip already-packed objects" behavior of `tarpack scan` / `tarpack pack`.
#
# Covers:
#   - run1: scan+pack a fixture tree -> pack-000001, one objects.jsonl line
#     per unique object.
#   - run2 (fast mode): add a new file, modify one existing file's content
#     and mtime, cp an existing file to a new name, leave the rest
#     untouched. scan+pack -> pack-000002 contains EXACTLY the new file, the
#     modified file, and the copy (fast mode cannot detect copies by content,
#     so the copy is treated as new). Unchanged files' snapshot entries
#     reference the SAME object_ids as run1. objects.jsonl grows by exactly
#     3 lines, with no duplicate object_ids.
#   - run3: no changes at all -> `tarpack pack` prints "nothing to pack", no
#     pack-000003 file appears, exit 0.
#   - hash-mode branch: fresh repo; run1 with --hash; cp a file to a new name
#     (different mtime); scan run2 with --hash -> the copy's snapshot entry
#     reuses the original object_id; pack -> "nothing to pack" (the copy was
#     not re-stored); a genuinely new file DOES get packed.
#
# Usage: test_incremental.sh <path-to-tarpack-binary>

set -u

TARPACK_BIN="${1:?usage: test_incremental.sh <path-to-tarpack-binary>}"
FAIL_COUNT=0

pass() { printf 'PASS: %s\n' "$1"; }
fail() { printf 'FAIL: %s\n' "$1" >&2; FAIL_COUNT=$((FAIL_COUNT + 1)); }
check() {
    if [ "$1" -eq 0 ]; then pass "$2"; else fail "$2"; fi
}

WORKDIR="$(mktemp -d /tmp/tarpack_incremental_test.XXXXXX)"
cleanup() { rm -rf "$WORKDIR"; }
trap cleanup EXIT

# ===========================================================================
# Part 1: fast-mode incremental scan+pack across three runs
# ===========================================================================

ROOT="$WORKDIR/fixture"
REPO="$WORKDIR/repo"

mkdir -p "$ROOT/subdir"
printf 'file one contents\n' > "$ROOT/a.txt"
printf 'file two contents, a bit longer\n' > "$ROOT/subdir/b.txt"
printf 'file three, unchanged throughout\n' > "$ROOT/c.txt"

# --- run1: scan + pack ---
"$TARPACK_BIN" scan "$ROOT" --repo "$REPO" --label snap1 >"$WORKDIR/scan1.out" 2>"$WORKDIR/scan1.err"
check $? "run1: scan succeeds"

"$TARPACK_BIN" pack --repo "$REPO" --snapshot snap1 >"$WORKDIR/pack1.out" 2>"$WORKDIR/pack1.err"
check $? "run1: pack succeeds"

PACK1="$REPO/packs/pack-000001.tar.zst"
[ -f "$PACK1" ]; check $? "run1: pack-000001.tar.zst exists"

INDEX="$REPO/objects/objects.jsonl"
[ -f "$INDEX" ]; check $? "run1: objects.jsonl exists"

RUN1_INDEX_LINES=$(wc -l < "$INDEX" | tr -d ' ')
[ "$RUN1_INDEX_LINES" -eq 3 ]; check $? "run1: objects.jsonl has one line per unique object (3)"

# capture run1 object_ids for a.txt, subdir/b.txt, c.txt (needed for later comparisons)
SNAP1="$REPO/snapshots/snap1.jsonl"
python3 - "$SNAP1" "$WORKDIR/run1_ids.json" <<'PY'
import sys, json
snap, out = sys.argv[1], sys.argv[2]
ids = {}
with open(snap) as f:
    for line in f:
        line = line.strip()
        if not line:
            continue
        obj = json.loads(line)
        if obj.get("type") == "file":
            ids[obj["path"]] = obj["object_id"]
with open(out, "w") as f:
    json.dump(ids, f)
PY
check $? "run1: extracted object_ids from snapshot"

# --- modify fixture for run2 ---
# new file
printf 'a brand new file\n' > "$ROOT/new_file.txt"
# modify one existing file's content AND mtime
sleep 1.1
printf 'file one contents -- MODIFIED\n' > "$ROOT/a.txt"
touch "$ROOT/a.txt"
# copy an existing (untouched) file to a new name -- fast mode cannot detect
# this as a dedup because path differs (even though content is identical)
cp "$ROOT/subdir/b.txt" "$ROOT/subdir/b_copy.txt"
# c.txt is left completely untouched

# --- run2: scan + pack (fast mode) ---
"$TARPACK_BIN" scan "$ROOT" --repo "$REPO" --label snap2 >"$WORKDIR/scan2.out" 2>"$WORKDIR/scan2.err"
check $? "run2: scan succeeds"

"$TARPACK_BIN" pack --repo "$REPO" --snapshot snap2 >"$WORKDIR/pack2.out" 2>"$WORKDIR/pack2.err"
check $? "run2: pack succeeds"

PACK2="$REPO/packs/pack-000002.tar.zst"
[ -f "$PACK2" ]; check $? "run2: pack-000002.tar.zst exists (next free number, not an error)"

RUN2_INDEX_LINES=$(wc -l < "$INDEX" | tr -d ' ')
[ "$RUN2_INDEX_LINES" -eq 6 ]; check $? "run2: objects.jsonl grew by exactly 3 lines (3 -> 6)"

# decompress pack-000002 and check its member set
RAW2="$WORKDIR/raw2.tar"
if command -v zstd >/dev/null 2>&1; then
    zstd -dc "$PACK2" > "$RAW2"
else
    python3 - "$PACK2" "$RAW2" <<'PY'
import sys
import zstandard
with open(sys.argv[1],'rb') as f, open(sys.argv[2],'wb') as o:
    zstandard.ZstdDecompressor().copy_stream(f, o)
PY
fi
check $? "run2: decompress pack-000002 to raw tar"

python3 - "$RAW2" <<'PY'
import sys, tarfile
raw = sys.argv[1]
with tarfile.open(raw, "r:") as t:
    names = sorted(m.name for m in t.getmembers())
expected = sorted(["new_file.txt", "a.txt", "subdir/b_copy.txt"])
sys.exit(0 if names == expected else 1)
PY
check $? "run2: pack-000002 contains EXACTLY the new file, modified file, and the copy"

# --- object_id continuity + no duplicates + unchanged files reuse ids ---
SNAP2="$REPO/snapshots/snap2.jsonl"
python3 - "$WORKDIR/run1_ids.json" "$SNAP2" "$INDEX" <<'PY'
import sys, json

run1_ids_path, snap2_path, index_path = sys.argv[1], sys.argv[2], sys.argv[3]

with open(run1_ids_path) as f:
    run1_ids = json.load(f)

snap2_ids = {}
with open(snap2_path) as f:
    for line in f:
        line = line.strip()
        if not line:
            continue
        obj = json.loads(line)
        if obj.get("type") == "file":
            snap2_ids[obj["path"]] = obj["object_id"]

failures = []

def check(cond, label):
    if cond:
        print(f"PASS: {label}")
    else:
        print(f"FAIL: {label}", file=sys.stderr)
        failures.append(label)

# subdir/b.txt and c.txt are untouched -> same object_id as run1
check(snap2_ids.get("subdir/b.txt") == run1_ids.get("subdir/b.txt"),
      "run2: unchanged subdir/b.txt reuses run1's object_id")
check(snap2_ids.get("c.txt") == run1_ids.get("c.txt"),
      "run2: unchanged c.txt reuses run1's object_id")

# a.txt was modified -> must get a DIFFERENT object_id than run1
check(snap2_ids.get("a.txt") is not None and snap2_ids.get("a.txt") != run1_ids.get("a.txt"),
      "run2: modified a.txt gets a new object_id (different from run1)")

# new_file.txt and subdir/b_copy.txt must have ids not present in run1
run1_id_set = set(run1_ids.values())
check(snap2_ids.get("new_file.txt") not in run1_id_set,
      "run2: new_file.txt has a fresh object_id")
check(snap2_ids.get("subdir/b_copy.txt") not in run1_id_set,
      "run2: subdir/b_copy.txt (fast-mode-undetected copy) has a fresh object_id")

# no duplicate object_ids in the index
index_ids = []
with open(index_path) as f:
    for line in f:
        line = line.strip()
        if not line:
            continue
        index_ids.append(json.loads(line)["object_id"])
check(len(index_ids) == len(set(index_ids)), "index: no duplicate object_ids across all appended lines")

if failures:
    print(f"\n{len(failures)} check(s) failed", file=sys.stderr)
    sys.exit(1)
sys.exit(0)
PY
check $? "run2: object_id continuity / dedup / no-duplicate assertions"

# --- run3: no changes at all -> "nothing to pack", no pack-000003, exit 0 ---
"$TARPACK_BIN" scan "$ROOT" --repo "$REPO" --label snap3 >"$WORKDIR/scan3.out" 2>"$WORKDIR/scan3.err"
check $? "run3: scan succeeds"

"$TARPACK_BIN" pack --repo "$REPO" --snapshot snap3 >"$WORKDIR/pack3.out" 2>"$WORKDIR/pack3.err"
PACK3_RC=$?
check $((PACK3_RC == 0 ? 0 : 1)) "run3: pack exits 0 when there is nothing new to pack"

grep -qi 'nothing to pack' "$WORKDIR/pack3.out" "$WORKDIR/pack3.err"
check $? "run3: pack prints 'nothing to pack'"

[ ! -e "$REPO/packs/pack-000003.tar.zst" ]; check $? "run3: no pack-000003.tar.zst file appears"

# ===========================================================================
# Part 2: hash-mode dedup (renamed/copied file detection)
# ===========================================================================

HROOT="$WORKDIR/hfixture"
HREPO="$WORKDIR/hrepo"

mkdir -p "$HROOT"
printf 'hash mode original content\n' > "$HROOT/orig.txt"
printf 'another unrelated file\n' > "$HROOT/other.txt"

"$TARPACK_BIN" scan "$HROOT" --repo "$HREPO" --label hsnap1 --hash >"$WORKDIR/hscan1.out" 2>"$WORKDIR/hscan1.err"
check $? "hash-mode run1: scan --hash succeeds"

"$TARPACK_BIN" pack --repo "$HREPO" --snapshot hsnap1 >"$WORKDIR/hpack1.out" 2>"$WORKDIR/hpack1.err"
check $? "hash-mode run1: pack succeeds"

[ -f "$HREPO/packs/pack-000001.tar.zst" ]; check $? "hash-mode run1: pack-000001.tar.zst exists"

HSNAP1="$HREPO/snapshots/hsnap1.jsonl"
ORIG_ID=$(python3 -c "
import json
with open('$HSNAP1') as f:
    for line in f:
        line = line.strip()
        if not line: continue
        obj = json.loads(line)
        if obj.get('type') == 'file' and obj.get('path') == 'orig.txt':
            print(obj['object_id'])
            break
")
[ -n "$ORIG_ID" ]; check $? "hash-mode run1: captured orig.txt's object_id"

# copy orig.txt to a new name with a different mtime; also add a genuinely new file
sleep 1.1
cp "$HROOT/orig.txt" "$HROOT/renamed_copy.txt"
printf 'genuinely new content for hash mode\n' > "$HROOT/brand_new.txt"

"$TARPACK_BIN" scan "$HROOT" --repo "$HREPO" --label hsnap2 --hash >"$WORKDIR/hscan2.out" 2>"$WORKDIR/hscan2.err"
check $? "hash-mode run2: scan --hash succeeds"

HSNAP2="$HREPO/snapshots/hsnap2.jsonl"
python3 - "$HSNAP2" "$ORIG_ID" <<'PY'
import sys, json
snap, orig_id = sys.argv[1], sys.argv[2]
entries = {}
with open(snap) as f:
    for line in f:
        line = line.strip()
        if not line:
            continue
        obj = json.loads(line)
        if obj.get("type") == "file":
            entries[obj["path"]] = obj
failures = []
def check(cond, label):
    if cond:
        print(f"PASS: {label}")
    else:
        print(f"FAIL: {label}", file=sys.stderr)
        failures.append(label)

renamed = entries.get("renamed_copy.txt")
check(renamed is not None, "hash-mode run2: renamed_copy.txt present in snapshot")
check(renamed is not None and renamed.get("object_id") == orig_id,
      "hash-mode run2: renamed_copy.txt's snapshot entry reuses orig.txt's object_id")

brand_new = entries.get("brand_new.txt")
check(brand_new is not None, "hash-mode run2: brand_new.txt present in snapshot")
check(brand_new is not None and brand_new.get("object_id") != orig_id,
      "hash-mode run2: brand_new.txt gets a different (fresh) object_id")

if failures:
    sys.exit(1)
sys.exit(0)
PY
check $? "hash-mode run2: renamed/copied file dedup assertions"

"$TARPACK_BIN" pack --repo "$HREPO" --snapshot hsnap2 >"$WORKDIR/hpack2.out" 2>"$WORKDIR/hpack2.err"
HPACK2_RC=$?

# pack-000002 must exist (brand_new.txt needs storing) but must NOT contain
# renamed_copy.txt's content a second time.
[ -f "$HREPO/packs/pack-000002.tar.zst" ]
check $? "hash-mode run2: pack-000002.tar.zst exists (brand_new.txt needed storing)"
check $((HPACK2_RC == 0 ? 0 : 1)) "hash-mode run2: pack exits 0"

RAW_H2="$WORKDIR/raw_h2.tar"
if command -v zstd >/dev/null 2>&1; then
    zstd -dc "$HREPO/packs/pack-000002.tar.zst" > "$RAW_H2"
else
    python3 - "$HREPO/packs/pack-000002.tar.zst" "$RAW_H2" <<'PY'
import sys
import zstandard
with open(sys.argv[1],'rb') as f, open(sys.argv[2],'wb') as o:
    zstandard.ZstdDecompressor().copy_stream(f, o)
PY
fi
check $? "hash-mode run2: decompress pack-000002"

python3 - "$RAW_H2" <<'PY'
import sys, tarfile
with tarfile.open(sys.argv[1], "r:") as t:
    names = sorted(m.name for m in t.getmembers())
sys.exit(0 if names == ["brand_new.txt"] else 1)
PY
check $? "hash-mode run2: pack-000002 contains ONLY brand_new.txt (copy not re-stored)"

# --- run3 for hash-mode repo: no changes -> nothing to pack ---
"$TARPACK_BIN" scan "$HROOT" --repo "$HREPO" --label hsnap3 --hash >"$WORKDIR/hscan3.out" 2>"$WORKDIR/hscan3.err"
check $? "hash-mode run3: scan --hash succeeds"

"$TARPACK_BIN" pack --repo "$HREPO" --snapshot hsnap3 >"$WORKDIR/hpack3.out" 2>"$WORKDIR/hpack3.err"
HPACK3_RC=$?
check $((HPACK3_RC == 0 ? 0 : 1)) "hash-mode run3: pack exits 0 when nothing new to pack"
grep -qi 'nothing to pack' "$WORKDIR/hpack3.out" "$WORKDIR/hpack3.err"
check $? "hash-mode run3: pack prints 'nothing to pack'"
[ ! -e "$HREPO/packs/pack-000003.tar.zst" ]; check $? "hash-mode run3: no pack-000003.tar.zst appears"

if [ "$FAIL_COUNT" -ne 0 ]; then
    printf '\n%d check(s) failed\n' "$FAIL_COUNT" >&2
    exit 1
fi
printf '\nall incremental integration checks passed\n'
exit 0
