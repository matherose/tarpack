#!/usr/bin/env bash
#
# Integration test for content-aware packing (v1.2).
#
# Builds a fixture with a highly compressible text file and an incompressible
# random-bytes file (both >= the 64 KiB entropy sample), packs with the
# default target size, and asserts:
#   - two packs are produced (compressibility classes never share a pack),
#     with the normal-class pack numbered first;
#   - the text file and the random file land in different packs;
#   - the store-level pack is still plain tar.zst readable by stock tar;
#   - the store-level pack barely shrinks while the text pack compresses hard;
#   - verify --objects passes and restore reproduces both files byte-exactly;
#   - pack -v reports the store-level routing decision.
#
# Usage: test_content_aware.sh <path-to-tarpack-binary>

set -u

TARPACK_BIN="${1:?usage: test_content_aware.sh <path-to-tarpack-binary>}"
FAIL_COUNT=0

pass() { printf 'PASS: %s\n' "$1"; }
fail() { printf 'FAIL: %s\n' "$1" >&2; FAIL_COUNT=$((FAIL_COUNT + 1)); }
check() {
    if [ "$1" -eq 0 ]; then pass "$2"; else fail "$2"; fi
}

WORKDIR="$(mktemp -d /tmp/tarpack_content_test.XXXXXX)"
cleanup() { rm -rf "$WORKDIR"; }
trap cleanup EXIT

ROOT="$WORKDIR/fixture"
REPO="$WORKDIR/repo"
REPO2="$WORKDIR/repo2"
DEST="$WORKDIR/out"
mkdir -p "$ROOT"

# ~1 MiB of repeated text (compressible) and 1 MiB of random bytes.
i=0
while [ "$i" -lt 16384 ]; do
    printf 'the quick brown fox jumps over the lazy dog %08d padding\n' "$i"
    i=$((i + 1))
done > "$ROOT/text.txt"
head -c 1048576 /dev/urandom > "$ROOT/random.bin"

"$TARPACK_BIN" scan "$ROOT" --repo "$REPO" >/dev/null 2>&1
check $? "scan succeeds"
"$TARPACK_BIN" pack --repo "$REPO" >/dev/null 2>&1
check $? "pack succeeds"

PACK_COUNT=$(ls "$REPO/packs" | grep -c '\.tar\.zst$')
[ "$PACK_COUNT" -eq 2 ]
check $? "two packs produced (classes never share a pack)"

TEXT_PACK=""
RAND_PACK=""
for p in "$REPO/packs/"*.tar.zst; do
    if tar --zstd -tf "$p" 2>/dev/null | grep -q '^text\.txt$'; then TEXT_PACK="$p"; fi
    if tar --zstd -tf "$p" 2>/dev/null | grep -q '^random\.bin$'; then RAND_PACK="$p"; fi
done
[ -n "$TEXT_PACK" ] && [ -n "$RAND_PACK" ] && [ "$TEXT_PACK" != "$RAND_PACK" ]
check $? "text and random bytes land in different packs"

case "$TEXT_PACK" in
    */pack-000001.tar.zst) true ;;
    *) false ;;
esac
check $? "normal-class pack is numbered first"

TEXT_SIZE=$(wc -c < "$TEXT_PACK")
RAND_SIZE=$(wc -c < "$RAND_PACK")
# Random bytes do not compress: the store-level pack stays >= its 1 MiB input.
[ "$RAND_SIZE" -ge 1048576 ]
check $? "store-level pack barely shrinks (${RAND_SIZE} bytes)"
# Repeated text collapses to a small fraction of its input.
[ "$TEXT_SIZE" -le 262144 ]
check $? "text pack compresses hard (${TEXT_SIZE} bytes)"

"$TARPACK_BIN" verify --repo "$REPO" --objects >/dev/null 2>&1
check $? "verify --objects passes on split packs"

"$TARPACK_BIN" restore --repo "$REPO" --dest "$DEST" >/dev/null 2>&1
check $? "restore succeeds"
cmp -s "$ROOT/text.txt" "$DEST/text.txt"
check $? "text file restored byte-exact"
cmp -s "$ROOT/random.bin" "$DEST/random.bin"
check $? "random file restored byte-exact"

"$TARPACK_BIN" scan "$ROOT" --repo "$REPO2" >/dev/null 2>&1
"$TARPACK_BIN" pack --repo "$REPO2" -v >/dev/null 2>"$WORKDIR/pack.log"
grep -q 'random\.bin: .*store-level' "$WORKDIR/pack.log"
check $? "pack -v reports the store-level routing decision"

if [ "$FAIL_COUNT" -gt 0 ]; then
    printf '%d check(s) failed\n' "$FAIL_COUNT" >&2
    exit 1
fi
printf 'all checks passed\n'
exit 0
