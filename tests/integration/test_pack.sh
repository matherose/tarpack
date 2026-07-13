#!/usr/bin/env bash
#
# Integration test for `tarpack pack` (milestone v0.2).
#
# Builds a fixture tree (regular files, an empty file, a hardlink pair, a
# subdirectory, a non-ASCII UTF-8 filename), runs `tarpack scan` then
# `tarpack pack`, and asserts:
#   - pack-000001.tar.zst exists and lists exactly the unique object paths
#     (hardlink content appears once);
#   - the .part scratch file is gone;
#   - each manifest sha256 equals shasum -a256 of the source file;
#   - the empty file is present with size 0;
#   - OFFSET VALIDATION: for every manifest entry, seeking to `offset` in the
#     decompressed tar and reading `size` bytes reproduces the source file
#     byte-for-byte (including entries after the empty file and after a
#     long-name / large-metadata entry);
#   - determinism: packing the same scan twice yields identical offsets.
#
# Usage: test_pack.sh <path-to-tarpack-binary>

set -u

TARPACK_BIN="${1:?usage: test_pack.sh <path-to-tarpack-binary>}"
FAIL_COUNT=0

pass() { printf 'PASS: %s\n' "$1"; }
fail() { printf 'FAIL: %s\n' "$1" >&2; FAIL_COUNT=$((FAIL_COUNT + 1)); }
check() {
    if [ "$1" -eq 0 ]; then pass "$2"; else fail "$2"; fi
}

WORKDIR="$(mktemp -d /tmp/tarpack_pack_test.XXXXXX)"
cleanup() { rm -rf "$WORKDIR"; }
trap cleanup EXIT

ROOT="$WORKDIR/fixture"
REPO="$WORKDIR/repo"
REPO2="$WORKDIR/repo2"

mkdir -p "$ROOT/subdir"

# Regular files with distinct content.
printf 'hello world\n' > "$ROOT/a.txt"
printf 'another file with more bytes than the first one\n' > "$ROOT/subdir/b.txt"

# Empty (zero-size) file.
: > "$ROOT/empty.txt"

# Hardlink pair: same object, two paths -> packed once.
printf 'shared hardlinked content\n' > "$ROOT/link_src.txt"
ln "$ROOT/link_src.txt" "$ROOT/link_dst.txt"

# Long filename to force a pax extended header (exercises offset accounting
# for large-metadata entries), plus a file *after* it.
LONGNAME="a-very-long-filename-that-exceeds-one-hundred-characters-to-force-a-pax-extended-header-block-xxxxxxxxxx.txt"
printf 'long name payload\n' > "$ROOT/$LONGNAME"
printf 'file after the long-name entry\n' > "$ROOT/zzz_after.txt"

# Non-ASCII (valid UTF-8) filename.
UTF8NAME="café_ünïçödé.txt"
printf 'unicode filename payload\n' > "$ROOT/$UTF8NAME"

# --- scan ---
"$TARPACK_BIN" scan "$ROOT" --repo "$REPO" --label snap1 >"$WORKDIR/scan.out" 2>"$WORKDIR/scan.err"
check $? "scan succeeds"

# --- pack ---
"$TARPACK_BIN" pack --repo "$REPO" --snapshot snap1 >"$WORKDIR/pack.out" 2>"$WORKDIR/pack.err"
PACK_RC=$?
check $((PACK_RC == 0 ? 0 : 1)) "pack exits 0 (clean)"

PACK="$REPO/packs/pack-000001.tar.zst"
MANIFEST="$REPO/packs/pack-000001.json"

[ -f "$PACK" ]; check $? "pack-000001.tar.zst exists"
[ -f "$MANIFEST" ]; check $? "pack-000001.json exists"
[ ! -e "$REPO/packs/pack-000001.tar.zst.part" ]; check $? ".part scratch file is gone"
[ ! -e "$REPO/packs/pack-000001.json.part" ]; check $? ".json.part scratch file is gone"

# --- decompress the archive ---
RAW="$WORKDIR/raw.tar"
if command -v zstd >/dev/null 2>&1; then
    zstd -dc "$PACK" > "$RAW"
else
    tar --zstd -cf /dev/null --version >/dev/null 2>&1 || true
    # fall back to python zstandard if zstd CLI missing (unlikely on CI)
    python3 - "$PACK" "$RAW" <<'PY'
import sys
try:
    import zstandard
except ImportError:
    sys.exit(3)
with open(sys.argv[1],'rb') as f, open(sys.argv[2],'wb') as o:
    zstandard.ZstdDecompressor().copy_stream(f, o)
PY
fi
check $? "decompress pack to raw tar"

# --- archive lists exactly the unique object paths (hardlink content once) ---
python3 - "$RAW" "$LONGNAME" "$UTF8NAME" <<'PY'
import sys, tarfile
raw, longname, utf8name = sys.argv[1], sys.argv[2], sys.argv[3]
with tarfile.open(raw, "r:") as t:
    names = sorted(m.name for m in t.getmembers())
# hardlink pair: exactly one of the two representative paths must appear, once.
expected_others = {"a.txt", "subdir/b.txt", "empty.txt", longname, "zzz_after.txt", utf8name}
hardlink_names = {"link_src.txt", "link_dst.txt"}
present_hardlink = [n for n in names if n in hardlink_names]
rest = [n for n in names if n not in hardlink_names]
ok = (len(present_hardlink) == 1) and (sorted(rest) == sorted(expected_others))
if not ok:
    print("MEMBERS:", names, file=sys.stderr)
    print("present_hardlink:", present_hardlink, file=sys.stderr)
    sys.exit(1)
sys.exit(0)
PY
check $? "archive lists exactly the unique object paths (hardlink packed once)"

# --- per-entry sha256 and offset validation ---
python3 - "$MANIFEST" "$RAW" "$ROOT" <<'PY'
import sys, json, hashlib, base64, os
manifest, raw, root = sys.argv[1], sys.argv[2], sys.argv[3]
with open(manifest) as f:
    doc = json.load(f)

assert doc["format"] == "tarpack-pack-v1", doc.get("format")
assert doc["pack"] == "pack-000001", doc.get("pack")

fails = 0
saw_empty = False
with open(raw, "rb") as rf:
    for e in doc["entries"]:
        if "path" in e:
            relpath = e["path"]
        else:
            relpath = base64.b64decode(e["path_b64"]).decode("utf-8", "surrogateescape")
        src = os.path.join(root, relpath)
        size = e["size"]
        offset = e["offset"]

        # source bytes
        with open(src, "rb") as sf:
            src_bytes = sf.read()

        # sha256 in manifest == sha of source (note: source size may have changed,
        # but in this test it does not, so compare against source directly)
        want_sha = hashlib.sha256(src_bytes).hexdigest()
        if e["sha256"] != want_sha:
            print(f"SHA MISMATCH {relpath}: manifest {e['sha256']} src {want_sha}", file=sys.stderr)
            fails += 1

        # OFFSET: seek to offset in raw tar, read size bytes, compare to source
        rf.seek(offset)
        got = rf.read(size)
        if got != src_bytes[:size]:
            print(f"OFFSET MISMATCH {relpath}: off={offset} size={size}", file=sys.stderr)
            fails += 1

        if size == 0:
            saw_empty = True

if not saw_empty:
    print("no zero-size entry found in manifest", file=sys.stderr)
    fails += 1

sys.exit(1 if fails else 0)
PY
check $? "every manifest entry: sha256 matches source AND offset seeks to correct data"

# --- determinism: pack the same scan into a fresh repo, compare offsets ---
cp -R "$REPO/snapshots" "$REPO2/snapshots" 2>/dev/null || { mkdir -p "$REPO2"; cp -R "$REPO/snapshots" "$REPO2/snapshots"; }
"$TARPACK_BIN" pack --repo "$REPO2" --snapshot snap1 >"$WORKDIR/pack2.out" 2>"$WORKDIR/pack2.err"
check $? "second pack (fresh repo) succeeds"

python3 - "$MANIFEST" "$REPO2/packs/pack-000001.json" <<'PY'
import sys, json
def load(p):
    with open(p) as f:
        d = json.load(f)
    return {e["object_id"]: (e["offset"], e["size"], e["sha256"]) for e in d["entries"]}
a = load(sys.argv[1]); b = load(sys.argv[2])
sys.exit(0 if a == b else 1)
PY
check $? "offsets/sizes/sha256 are identical across two packs (determinism)"

# --- v0.3: re-packing the same snapshot no longer errors "already exists".
# All of this snapshot's objects are already recorded in the object index
# (from the pack above), so pack must skip them entirely: print "nothing to
# pack", create no pack-000002, and exit 0. (Milestone v0.2 asserted this
# repack attempt failed; v0.3's index-aware skip-already-packed behavior
# supersedes that -- see milestone v0.3 spec.) ---
"$TARPACK_BIN" pack --repo "$REPO" --snapshot snap1 >"$WORKDIR/pack3.out" 2>"$WORKDIR/pack3.err"
PACK3_RC=$?
check $((PACK3_RC == 0 ? 0 : 1)) "re-packing the same already-packed snapshot exits 0"
grep -qi 'nothing to pack' "$WORKDIR/pack3.out" "$WORKDIR/pack3.err"
check $? "re-packing the same already-packed snapshot prints 'nothing to pack'"
[ ! -e "$REPO/packs/pack-000002.tar.zst" ]; check $? "re-packing the same snapshot creates no new pack file"

if [ "$FAIL_COUNT" -ne 0 ]; then
    printf '\n%d check(s) failed\n' "$FAIL_COUNT" >&2
    exit 1
fi
printf '\nall integration checks passed\n'
exit 0
