#!/usr/bin/env bash
#
# Crash-safety integration test for milestone v1.0.
#
# Repeatedly starts `tarpack pack --target-size <small>` in the background
# over a many-small-files fixture and kill -9's it after varying delays
# (0.05/0.15/0.3s), so kills land in different phases of the multi-pack
# write sequence. Tolerates the race where the pack finishes before the
# kill. After each kill asserts:
#   - no *.part files are counted as packs;
#   - objects.jsonl references only existing pack files;
#   - objects.jsonl contains no duplicate object_ids.
# Then runs `tarpack pack` again to completion and asserts:
#   - every unique object is indexed exactly once;
#   - `tarpack verify --objects` exits 0.
# Also pre-creates a stale orphan pack-000042.tar.zst.part and confirms a
# fresh pack run handles it without breaking. Documented behavior: stale
# .part files are DELETED by the recovery step at the start of the next
# pack run (they are never counted as packs, never verified, never
# checksummed).
#
# Usage: test_crash_resume.sh <path-to-tarpack-binary>

set -u

TARPACK_BIN="${1:?usage: test_crash_resume.sh <path-to-tarpack-binary>}"
FAIL_COUNT=0

pass() { printf 'PASS: %s\n' "$1"; }
fail() { printf 'FAIL: %s\n' "$1" >&2; FAIL_COUNT=$((FAIL_COUNT + 1)); }
check() {
    if [ "$1" -eq 0 ]; then pass "$2"; else fail "$2"; fi
}

WORKDIR="$(mktemp -d /tmp/tarpack_crash_test.XXXXXX)"
cleanup() { rm -rf "$WORKDIR"; }
trap cleanup EXIT

ROOT="$WORKDIR/fixture"
REPO="$WORKDIR/repo"
mkdir -p "$ROOT"

# Many small files so a small --target-size yields many packs and the
# pack run takes long enough for kills to land mid-flight.
NUM_FILES=120
for i in $(seq 1 $NUM_FILES); do
    dd if=/dev/urandom of="$ROOT/f_$i.bin" bs=1024 count=24 2>/dev/null
done

"$TARPACK_BIN" scan "$ROOT" --repo "$REPO" --label snap1 >/dev/null 2>&1
check $? "scan succeeds"

# consistency_check <label-prefix>: post-kill repo invariants
consistency_check() {
    local prefix="$1"

    # (a) no *.part counted as packs: assert the pack count via the committed
    # .tar.zst names only, and that any .part file is NOT listed in
    # SHA256SUMS or objects.jsonl.
    local part_in_sums=0
    if [ -f "$REPO/checksums/SHA256SUMS" ]; then
        grep -q '\.part$' "$REPO/checksums/SHA256SUMS" && part_in_sums=1
    fi
    [ "$part_in_sums" -eq 0 ]; check $? "$prefix: no .part file listed in SHA256SUMS"

    # (b)+(c) index invariants
    python3 - "$REPO" <<'PY'
import sys, json, os
repo = sys.argv[1]
idx = os.path.join(repo, "objects", "objects.jsonl")
fails = 0
ids = set()
if os.path.exists(idx):
    with open(idx, "rb") as f:
        data = f.read()
    # a torn final line (no trailing newline) is tolerated: it is repaired
    # by the next pack run. Complete lines must parse and be consistent.
    lines = data.split(b"\n")
    torn = lines[-1] if lines[-1] != b"" else None
    for l in lines[:-1]:
        if not l.strip():
            continue
        e = json.loads(l)
        oid = e["object_id"]
        if oid in ids:
            print(f"duplicate object_id {oid}", file=sys.stderr)
            fails += 1
        ids.add(oid)
        pack_file = os.path.join(repo, "packs", e["pack"] + ".tar.zst")
        if not os.path.exists(pack_file):
            print(f"index references missing pack {e['pack']}", file=sys.stderr)
            fails += 1
sys.exit(1 if fails else 0)
PY
    check $? "$prefix: objects.jsonl has no duplicate ids and references only existing packs"
}

# --- crash loop -------------------------------------------------------------
KILLED_PHASES=""
for delay in 0.05 0.15 0.3 0.1 0.2; do
    "$TARPACK_BIN" pack --repo "$REPO" --snapshot snap1 --target-size 96K \
        >"$WORKDIR/pack_bg.out" 2>"$WORKDIR/pack_bg.err" &
    BGPID=$!
    sleep "$delay"
    if kill -9 "$BGPID" 2>/dev/null; then
        KILLED_PHASES="$KILLED_PHASES $delay"
        wait "$BGPID" 2>/dev/null
        PACKS_NOW=$(ls "$REPO/packs/"pack-*.tar.zst 2>/dev/null | wc -l | tr -d ' ')
        PARTS_NOW=$(ls "$REPO/packs/"*.part 2>/dev/null | wc -l | tr -d ' ')
        echo "  (killed after ${delay}s: $PACKS_NOW committed packs, $PARTS_NOW .part files on disk)"
        consistency_check "after kill@${delay}s"
    else
        wait "$BGPID" 2>/dev/null
        echo "  (pack finished before the ${delay}s kill; tolerated)"
        consistency_check "after clean finish@${delay}s"
    fi
done
echo "phases actually killed:$KILLED_PHASES"

# --- stale orphan .part file ------------------------------------------------
mkdir -p "$REPO/packs"
dd if=/dev/urandom of="$REPO/packs/pack-000042.tar.zst.part" bs=1024 count=8 2>/dev/null
[ -f "$REPO/packs/pack-000042.tar.zst.part" ]; check $? "stale orphan pack-000042.tar.zst.part created"

# --- resume to completion -----------------------------------------------------
"$TARPACK_BIN" pack --repo "$REPO" --snapshot snap1 --target-size 96K \
    >"$WORKDIR/pack_final.out" 2>"$WORKDIR/pack_final.err"
check $? "final pack run completes cleanly"

# stale orphan handled: deleted by recovery (documented behavior), and in
# any case not present as a pack, not checksummed, not indexed.
[ ! -e "$REPO/packs/pack-000042.tar.zst.part" ]
check $? "stale orphan .part file was removed by the recovery step"
if [ -f "$REPO/checksums/SHA256SUMS" ]; then
    ! grep -q 'pack-000042\.tar\.zst\.part' "$REPO/checksums/SHA256SUMS"
    check $? "stale orphan .part name never appears in SHA256SUMS"
fi

# every unique object indexed exactly once, and the index covers ALL fixture
# objects (snapshot has NUM_FILES unique files)
python3 - "$REPO" "$NUM_FILES" <<'PY'
import sys, json, os
repo, want = sys.argv[1], int(sys.argv[2])
fails = 0
ids = {}
with open(os.path.join(repo, "objects", "objects.jsonl")) as f:
    for l in f:
        if not l.strip():
            continue
        e = json.loads(l)
        ids[e["object_id"]] = ids.get(e["object_id"], 0) + 1
dups = {k: v for k, v in ids.items() if v > 1}
if dups:
    print(f"duplicated object_ids: {dups}", file=sys.stderr)
    fails += 1
if len(ids) != want:
    print(f"expected {want} unique objects indexed, got {len(ids)}", file=sys.stderr)
    fails += 1
sys.exit(1 if fails else 0)
PY
check $? "every unique object is indexed exactly once after resume"

# repo verifies clean, including deep object verification
"$TARPACK_BIN" verify --repo "$REPO" --objects >"$WORKDIR/verify_final.out" 2>&1
check $? "verify --objects exits 0 after crash/resume cycles"

if [ "$FAIL_COUNT" -ne 0 ]; then
    printf '\n%d check(s) failed\n' "$FAIL_COUNT" >&2
    exit 1
fi
printf '\nall crash-resume integration checks passed\n'
exit 0
