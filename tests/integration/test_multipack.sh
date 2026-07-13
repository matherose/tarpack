#!/usr/bin/env bash
#
# Integration test for milestone v1.0 multi-pack splitting + verify.
#
# Fixture: ~10 files of known sizes (100-300 KB, from /dev/urandom) plus one
# 700 KB file that exceeds the 512K target and must land ALONE in its own
# pack. Asserts:
#   - `tarpack pack --target-size 512K` produces multiple packs;
#   - every object appears in exactly one pack manifest;
#   - per-pack sum of estimated entry costs <= target, except the
#     singleton-oversized pack;
#   - the 700K file is alone in its pack;
#   - objects.jsonl references correct packs/offsets (byte-compare via
#     zstd -dc + python3 seek/read, as in test_pack.sh);
#   - checksums/SHA256SUMS lists every pack + manifest;
#   - `tarpack verify` exits 0; after corrupting one pack (dd conv=notrunc)
#     it exits 1 and names the corrupted file; restoring the byte returns it
#     to 0;
#   - `tarpack verify --objects` exits 0 clean and 1 after re-corrupting.
#
# Usage: test_multipack.sh <path-to-tarpack-binary>

set -u

TARPACK_BIN="${1:?usage: test_multipack.sh <path-to-tarpack-binary>}"
FAIL_COUNT=0

pass() { printf 'PASS: %s\n' "$1"; }
fail() { printf 'FAIL: %s\n' "$1" >&2; FAIL_COUNT=$((FAIL_COUNT + 1)); }
check() {
    if [ "$1" -eq 0 ]; then pass "$2"; else fail "$2"; fi
}

WORKDIR="$(mktemp -d /tmp/tarpack_multipack_test.XXXXXX)"
cleanup() { rm -rf "$WORKDIR"; }
trap cleanup EXIT

ROOT="$WORKDIR/fixture"
REPO="$WORKDIR/repo"
mkdir -p "$ROOT"

# --- fixture: 10 files of 100-300 KB + one oversized 700 KB file ---------
SIZES=(102400 131072 150000 180224 200000 220000 245760 262144 280000 307200)
i=0
for sz in "${SIZES[@]}"; do
    dd if=/dev/urandom of="$ROOT/file_$i.bin" bs=1 count=0 seek="$sz" 2>/dev/null
    dd if=/dev/urandom of="$ROOT/file_$i.bin" bs=1024 count=$((sz / 1024)) 2>/dev/null
    # top off the non-1024-multiple remainder so sizes are exact
    rem=$((sz % 1024))
    if [ "$rem" -ne 0 ]; then
        dd if=/dev/urandom of="$ROOT/file_$i.bin" bs=1 count="$rem" seek=$((sz - rem)) conv=notrunc 2>/dev/null
    fi
    i=$((i + 1))
done
dd if=/dev/urandom of="$ROOT/oversized.bin" bs=1024 count=700 2>/dev/null

# --- scan + pack with a 512K target --------------------------------------
"$TARPACK_BIN" scan "$ROOT" --repo "$REPO" --label snap1 >"$WORKDIR/scan.out" 2>"$WORKDIR/scan.err"
check $? "scan succeeds"

"$TARPACK_BIN" pack --repo "$REPO" --snapshot snap1 --target-size 512K \
    >"$WORKDIR/pack.out" 2>"$WORKDIR/pack.err"
check $? "pack --target-size 512K succeeds"

PACK_COUNT=$(ls "$REPO/packs/"pack-*.tar.zst 2>/dev/null | wc -l | tr -d ' ')
[ "$PACK_COUNT" -ge 2 ]; check $? "multiple packs produced (got $PACK_COUNT)"

ls "$REPO/packs/"*.part >/dev/null 2>&1
[ $? -ne 0 ]; check $? "no .part scratch files remain"

# --- manifest-level assertions --------------------------------------------
python3 - "$REPO" 524288 <<'PY'
import sys, json, glob, os
repo, target = sys.argv[1], int(sys.argv[2])

def cost(size):
    return 512 + ((size + 511) // 512) * 512 + 1536

fails = 0
seen = {}
oversized_ok = False
for mpath in sorted(glob.glob(os.path.join(repo, "packs", "pack-*.json"))):
    with open(mpath) as f:
        doc = json.load(f)
    entries = doc["entries"]
    total = sum(cost(e["size"]) for e in entries)
    names = [e.get("path", e.get("path_b64")) for e in entries]
    for e in entries:
        oid = e["object_id"]
        if oid in seen:
            print(f"DUPLICATE object {oid} in {mpath} and {seen[oid]}", file=sys.stderr)
            fails += 1
        seen[oid] = mpath
    if total > target:
        # allowed only for a singleton pack whose lone entry is oversized
        if len(entries) == 1 and cost(entries[0]["size"]) > target:
            oversized_ok = True
        else:
            print(f"PACK OVER TARGET {mpath}: {total} > {target} entries={names}", file=sys.stderr)
            fails += 1
    # the oversized 700K file must be alone wherever it is
    for e in entries:
        if e.get("path") == "oversized.bin" and len(entries) != 1:
            print(f"oversized.bin not alone in {mpath}", file=sys.stderr)
            fails += 1

if len(seen) != 11:
    print(f"expected 11 unique objects across manifests, got {len(seen)}", file=sys.stderr)
    fails += 1
if not oversized_ok:
    print("no singleton over-target pack found (oversized.bin should form one)", file=sys.stderr)
    fails += 1
sys.exit(1 if fails else 0)
PY
check $? "every object in exactly one manifest; per-pack cost <= target except oversized singleton"

# --- objects.jsonl references correct packs/offsets (byte-compare) -------
python3 - "$REPO" "$ROOT" "$WORKDIR" <<'PY'
import sys, json, os, subprocess, base64
repo, root, workdir = sys.argv[1], sys.argv[2], sys.argv[3]

fails = 0
raw_cache = {}
def raw_tar(pack):
    if pack not in raw_cache:
        out = os.path.join(workdir, pack + ".tar")
        with open(out, "wb") as f:
            subprocess.run(["zstd", "-dc", os.path.join(repo, "packs", pack + ".tar.zst")],
                           stdout=f, check=True)
        raw_cache[pack] = out
    return raw_cache[pack]

with open(os.path.join(repo, "objects", "objects.jsonl")) as f:
    lines = [json.loads(l) for l in f if l.strip()]

if len(lines) != 11:
    print(f"expected 11 index lines, got {len(lines)}", file=sys.stderr)
    fails += 1

for e in lines:
    relpath = e.get("path") or base64.b64decode(e["path_b64"]).decode("utf-8", "surrogateescape")
    src = os.path.join(root, relpath)
    with open(src, "rb") as sf:
        src_bytes = sf.read()
    if len(src_bytes) != e["size"]:
        print(f"SIZE MISMATCH {relpath}", file=sys.stderr)
        fails += 1
        continue
    with open(raw_tar(e["pack"]), "rb") as rf:
        rf.seek(e["offset"])
        got = rf.read(e["size"])
    if got != src_bytes:
        print(f"OFFSET MISMATCH {relpath} pack={e['pack']} off={e['offset']}", file=sys.stderr)
        fails += 1

sys.exit(1 if fails else 0)
PY
check $? "objects.jsonl pack/offset references reproduce every source file byte-for-byte"

# --- SHA256SUMS lists every pack + manifest -------------------------------
SUMS="$REPO/checksums/SHA256SUMS"
[ -f "$SUMS" ]; check $? "checksums/SHA256SUMS exists"

MISSING=0
for f in "$REPO/packs/"pack-*.tar.zst "$REPO/packs/"pack-*.json; do
    rel="packs/$(basename "$f")"
    grep -q "  $rel\$" "$SUMS" || { echo "missing from SHA256SUMS: $rel" >&2; MISSING=1; }
done
[ "$MISSING" -eq 0 ]; check $? "SHA256SUMS lists every pack archive and manifest"

# shasum cross-validation (the format must be shasum -c compatible)
if command -v shasum >/dev/null 2>&1; then
    (cd "$REPO" && shasum -a 256 -c checksums/SHA256SUMS >/dev/null 2>&1)
    check $? "shasum -a 256 -c accepts SHA256SUMS"
fi

# --- verify: clean ---------------------------------------------------------
"$TARPACK_BIN" verify --repo "$REPO" >"$WORKDIR/verify1.out" 2>&1
check $? "verify exits 0 on a clean repo"

# --- corrupt one pack: flip a byte with dd conv=notrunc -------------------
VICTIM=$(ls "$REPO/packs/"pack-*.tar.zst | head -1)
VICTIM_REL="packs/$(basename "$VICTIM")"
OFF=100

dd if="$VICTIM" of="$WORKDIR/orig_byte" bs=1 skip=$OFF count=1 2>/dev/null
python3 - "$WORKDIR/orig_byte" "$WORKDIR/flip_byte" <<'PY'
import sys
orig = open(sys.argv[1], "rb").read(1)
open(sys.argv[2], "wb").write(bytes([orig[0] ^ 0xFF]))
PY
dd if="$WORKDIR/flip_byte" of="$VICTIM" bs=1 seek=$OFF count=1 conv=notrunc 2>/dev/null

"$TARPACK_BIN" verify --repo "$REPO" >"$WORKDIR/verify2.out" 2>&1
RC=$?
[ "$RC" -eq 1 ]; check $? "verify exits 1 after corrupting a pack (got $RC)"
grep -q "$(basename "$VICTIM"): FAILED" "$WORKDIR/verify2.out"
check $? "verify names the corrupted file ($VICTIM_REL)"

# --- restore the byte: verify clean again ----------------------------------
dd if="$WORKDIR/orig_byte" of="$VICTIM" bs=1 seek=$OFF count=1 conv=notrunc 2>/dev/null
"$TARPACK_BIN" verify --repo "$REPO" >"$WORKDIR/verify3.out" 2>&1
check $? "verify exits 0 again after restoring the byte"

# --- deep --objects mode ----------------------------------------------------
"$TARPACK_BIN" verify --repo "$REPO" --objects >"$WORKDIR/verify4.out" 2>&1
check $? "verify --objects exits 0 on a clean repo"

dd if="$WORKDIR/flip_byte" of="$VICTIM" bs=1 seek=$OFF count=1 conv=notrunc 2>/dev/null
"$TARPACK_BIN" verify --repo "$REPO" --objects >"$WORKDIR/verify5.out" 2>&1
RC=$?
[ "$RC" -eq 1 ]; check $? "verify --objects exits 1 after re-corrupting (got $RC)"

# --- --pack-algo ffd smoke test (fresh repo, same fixture) -----------------
dd if="$WORKDIR/orig_byte" of="$VICTIM" bs=1 seek=$OFF count=1 conv=notrunc 2>/dev/null
REPO_FFD="$WORKDIR/repo_ffd"
"$TARPACK_BIN" scan "$ROOT" --repo "$REPO_FFD" --label snap1 >/dev/null 2>&1
"$TARPACK_BIN" pack --repo "$REPO_FFD" --snapshot snap1 --target-size 512K --pack-algo ffd \
    >"$WORKDIR/pack_ffd.out" 2>"$WORKDIR/pack_ffd.err"
check $? "pack --pack-algo ffd succeeds"

FFD_COUNT=$(ls "$REPO_FFD/packs/"pack-*.tar.zst 2>/dev/null | wc -l | tr -d ' ')
[ "$FFD_COUNT" -ge 2 ]; check $? "ffd produces multiple packs (got $FFD_COUNT)"
[ "$FFD_COUNT" -le "$PACK_COUNT" ]; check $? "ffd uses no more packs than next-fit ($FFD_COUNT <= $PACK_COUNT)"

"$TARPACK_BIN" verify --repo "$REPO_FFD" --objects >"$WORKDIR/verify_ffd.out" 2>&1
check $? "verify --objects exits 0 on the ffd repo"

# bad algo value rejected
"$TARPACK_BIN" pack --repo "$REPO_FFD" --snapshot snap1 --pack-algo bogus >/dev/null 2>&1
RC=$?
[ "$RC" -eq 64 ]; check $? "pack rejects unknown --pack-algo with usage error (got $RC)"

# bad target size rejected
"$TARPACK_BIN" pack --repo "$REPO_FFD" --snapshot snap1 --target-size 100X >/dev/null 2>&1
RC=$?
[ "$RC" -eq 64 ]; check $? "pack rejects bad --target-size with usage error (got $RC)"

if [ "$FAIL_COUNT" -ne 0 ]; then
    printf '\n%d check(s) failed\n' "$FAIL_COUNT" >&2
    exit 1
fi
printf '\nall multipack integration checks passed\n'
exit 0
