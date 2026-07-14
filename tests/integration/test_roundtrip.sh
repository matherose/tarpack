#!/bin/sh
#
# Integration test for `tarpack restore` (milestone v1.1).
#
# Builds a fixture tree exercising: regular files, an empty file, a 3-name
# hardlink group, relative + absolute + dangling symlinks, a multibyte UTF-8
# name, a name with spaces and quotes, and nested dirs with distinct modes and
# known mtimes. Then: scan -> pack (small --target-size to force MULTIPLE
# packs) -> restore to a fresh dir -> assert the tree round-trips:
#   - diff -r clean
#   - hardlink inode-sharing groups identical (by grouping, not raw inode nums)
#   - symlink targets byte-identical
#   - mtimes match (second precision; nsec too when stat supports it)
#   - modes match
#   - `tarpack verify --objects` still exits 0
#
# Also checks that an OLDER snapshot restores correctly in an incremental repo:
# scan+pack a first tree, save a copy, scan+pack an expanded tree, then restore
# snapshot 1 and compare against the saved copy.
#
# Usage: test_roundtrip.sh <path-to-tarpack-binary>

set -u

TARPACK_BIN="${1:?usage: test_roundtrip.sh <path-to-tarpack-binary>}"
FAIL_COUNT=0

pass() { printf 'PASS: %s\n' "$1"; }
fail() { printf 'FAIL: %s\n' "$1" >&2; FAIL_COUNT=$((FAIL_COUNT + 1)); }
check() {
    if [ "$1" -eq 0 ]; then pass "$2"; else fail "$2"; fi
}

WORKDIR="$(mktemp -d /tmp/tarpack_roundtrip_test.XXXXXX)"
cleanup() { rm -rf "$WORKDIR"; }
trap cleanup EXIT

ROOT="$WORKDIR/fixture"
REPO="$WORKDIR/repo"
DEST="$WORKDIR/dest"

# stat helpers: BSD (macOS) vs GNU. Emit mode (octal perms), mtime seconds,
# and mtime nanoseconds; and per-file inode number.
if stat -f '%p' / >/dev/null 2>&1; then
    STATFLAVOR=bsd
else
    STATFLAVOR=gnu
fi

mode_of()  { # permission bits as 4-digit octal
    if [ "$STATFLAVOR" = bsd ]; then
        printf '%04o\n' "$(( 0$(stat -f '%p' "$1") & 07777 ))"
    else
        stat -c '%a' "$1"
    fi
}
mtime_s()  {
    if [ "$STATFLAVOR" = bsd ]; then stat -f '%m' "$1"; else stat -c '%Y' "$1"; fi
}
mtime_ns() {
    if [ "$STATFLAVOR" = bsd ]; then
        # BSD stat: %Fm gives fractional seconds like 1700000000.123456789
        v="$(stat -f '%Fm' "$1" 2>/dev/null)"
        case "$v" in *.*) printf '%s\n' "${v#*.}";; *) printf '0\n';; esac
    else
        stat -c '%y' "$1" | sed -n 's/.*\.\([0-9]*\).*/\1/p'
    fi
}
inode_of() {
    if [ "$STATFLAVOR" = bsd ]; then stat -f '%i' "$1"; else stat -c '%i' "$1"; fi
}
lreadlink() { readlink "$1"; }

# compare_trees A B: recursive tree equality -- entry sets, types, regular
# file contents (byte-for-byte), and symlink target bytes. Used instead of
# `diff -r` because both BSD and GNU diff fail on dangling symlinks (they try
# to open the target). Strictly stronger than diff -r for our purposes.
compare_trees() {
    python3 - "$1" "$2" <<'PY'
import os, sys
a, b = sys.argv[1], sys.argv[2]
fails = 0
def entries(root):
    out = set()
    for dirpath, dirnames, filenames in os.walk(root, followlinks=False):
        rel = os.path.relpath(dirpath, root)
        for n in dirnames + filenames:
            out.add(os.path.normpath(os.path.join(rel, n)))
    return out
ea, eb = entries(a), entries(b)
for missing in sorted(ea - eb):
    print(f"only in {a}: {missing}", file=sys.stderr); fails += 1
for extra in sorted(eb - ea):
    print(f"only in {b}: {extra}", file=sys.stderr); fails += 1
for rel in sorted(ea & eb):
    pa, pb = os.path.join(a, rel), os.path.join(b, rel)
    la, lb = os.path.islink(pa), os.path.islink(pb)
    if la != lb:
        print(f"type mismatch (symlink?) {rel}", file=sys.stderr); fails += 1; continue
    if la:
        ta = os.readlink(pa)
        tb = os.readlink(pb)
        if ta != tb:
            print(f"symlink target mismatch {rel}: {ta!r} != {tb!r}", file=sys.stderr); fails += 1
        continue
    da, db = os.path.isdir(pa), os.path.isdir(pb)
    if da != db:
        print(f"type mismatch (dir?) {rel}", file=sys.stderr); fails += 1; continue
    if da:
        continue
    with open(pa, "rb") as f: ba = f.read()
    with open(pb, "rb") as f: bb = f.read()
    if ba != bb:
        print(f"content mismatch {rel}", file=sys.stderr); fails += 1
sys.exit(1 if fails else 0)
PY
}

# ---------------------------------------------------------------------------
# Build the fixture tree.
# ---------------------------------------------------------------------------
mkdir -p "$ROOT"
mkdir -p "$ROOT/nested/deep"

printf 'hello world\n'    > "$ROOT/alpha.txt"
printf 'second file body' > "$ROOT/beta.bin"
printf 'deep content here' > "$ROOT/nested/deep/gamma.dat"
: > "$ROOT/empty.txt"                 # empty (zero-size) file

# 3-name hardlink group (same inode/content, 3 links)
printf 'linked payload' > "$ROOT/link_a"
ln "$ROOT/link_a" "$ROOT/link_b"
ln "$ROOT/link_a" "$ROOT/nested/link_c"

# symlinks: relative, absolute, dangling
ln -s alpha.txt          "$ROOT/rel_link"
ln -s /etc/hosts         "$ROOT/abs_link"
ln -s does_not_exist     "$ROOT/dangling"

# multibyte UTF-8 name (café_😀.txt via printf octal escapes: POSIX sh does
# not expand \x inside double quotes) and a name with spaces/quotes
UTF8NAME="$(printf 'caf\303\251_\360\237\230\200.txt')"
printf 'unicode body' > "$ROOT/$UTF8NAME"
printf 'quoted body'  > "$ROOT/has space \"quote\".txt"

# distinct dir modes + known mtimes (set files' mtimes too)
chmod 0700 "$ROOT/nested"
chmod 0755 "$ROOT/nested/deep"
# Set deterministic mtimes on everything (touch -t: [[CC]YY]MMDDhhmm[.ss])
find "$ROOT" -depth ! -type l -exec touch -t 202301021530.45 {} + 2>/dev/null
# symlink mtimes (touch -h where supported)
for l in rel_link abs_link dangling; do
    touch -h -t 202301021530.45 "$ROOT/$l" 2>/dev/null || true
done

# ---------------------------------------------------------------------------
# scan -> pack (force multiple packs with a tiny target size) -> restore
# ---------------------------------------------------------------------------
echo "--- scan ---"
"$TARPACK_BIN" scan "$ROOT" --repo "$REPO" --label snap1 --hash
check $? "scan snap1"

echo "--- pack (tiny target-size forces multiple packs) ---"
"$TARPACK_BIN" pack --repo "$REPO" --snapshot snap1 --target-size 1
check $? "pack snap1"

npacks=$(ls "$REPO"/packs/pack-*.tar.zst 2>/dev/null | wc -l | tr -d ' ')
if [ "$npacks" -ge 2 ]; then pass "multiple packs produced ($npacks)"; else fail "expected >=2 packs, got $npacks"; fi

echo "--- restore ---"
"$TARPACK_BIN" restore --repo "$REPO" --dest "$DEST" --snapshot snap1
check $? "restore snap1 exit 0"

# --- recursive tree comparison clean (diff -r equivalent, symlink-safe) ---
compare_trees "$ROOT" "$DEST"
check $? "recursive tree comparison fixture vs restore is clean"

# --- symlink targets byte-identical ---
for l in rel_link abs_link dangling; do
    a="$(lreadlink "$ROOT/$l")"
    b="$(lreadlink "$DEST/$l")"
    if [ "$a" = "$b" ]; then pass "symlink $l target byte-identical ($a)"; else fail "symlink $l: '$a' != '$b'"; fi
done

# --- hardlink inode-sharing groups identical (grouping, not raw inode) ---
ia=$(inode_of "$DEST/link_a"); ib=$(inode_of "$DEST/link_b"); ic=$(inode_of "$DEST/nested/link_c")
if [ "$ia" = "$ib" ] && [ "$ib" = "$ic" ]; then
    pass "restored link_a/link_b/nested/link_c share one inode ($ia)"
else
    fail "hardlink group not shared: $ia $ib $ic"
fi
# And a non-linked file must NOT share their inode.
io=$(inode_of "$DEST/alpha.txt")
if [ "$io" != "$ia" ]; then pass "unrelated file has a distinct inode"; else fail "unrelated file shares hardlink inode"; fi

# --- modes match (files + dirs) ---
mode_check() {
    ma="$(mode_of "$ROOT/$1")"; mb="$(mode_of "$DEST/$1")"
    if [ "$ma" = "$mb" ]; then pass "mode $1 matches ($ma)"; else fail "mode $1: $ma != $mb"; fi
}
mode_check alpha.txt
mode_check empty.txt
mode_check nested
mode_check nested/deep

# --- mtimes match (second precision; nsec when available) ---
mtime_check() {
    sa="$(mtime_s "$ROOT/$1")"; sb="$(mtime_s "$DEST/$1")"
    if [ "$sa" = "$sb" ]; then pass "mtime(sec) $1 matches ($sa)"; else fail "mtime(sec) $1: $sa != $sb"; fi
    na="$(mtime_ns "$ROOT/$1")"; nb="$(mtime_ns "$DEST/$1")"
    if [ "$na" = "$nb" ]; then pass "mtime(nsec) $1 matches ($na)"; else fail "mtime(nsec) $1: $na != $nb"; fi
}
mtime_check alpha.txt
mtime_check nested            # directory mtime restored last, deepest-first
mtime_check nested/deep

# symlink mtime (sec) where the platform tracks it
sla="$(mtime_s "$ROOT/rel_link")"; slb="$(mtime_s "$DEST/rel_link")"
if [ "$sla" = "$slb" ]; then pass "symlink mtime(sec) rel_link matches"; else fail "symlink mtime rel_link: $sla != $slb"; fi

# --- verify --objects still clean ---
echo "--- verify --objects ---"
"$TARPACK_BIN" verify --repo "$REPO" --objects
check $? "verify --objects exits 0 after restore"

# ---------------------------------------------------------------------------
# Incremental repo: restore an OLDER snapshot.
# Save a copy of the current (snap1) tree state, then expand the tree,
# scan+pack a snap2, and restore snap1 -> must match the saved copy.
# ---------------------------------------------------------------------------
echo "--- incremental: save snap1 state, expand tree, scan+pack snap2 ---"
SAVED="$WORKDIR/saved_snap1"
cp -a "$DEST" "$SAVED"   # DEST is a faithful copy of the snap1 tree

# expand the tree with new content
printf 'brand new file\n' > "$ROOT/delta_new.txt"
printf 'more new bytes'   > "$ROOT/nested/deep/epsilon_new.dat"
find "$ROOT/delta_new.txt" "$ROOT/nested/deep/epsilon_new.dat" -exec touch -t 202301031600.00 {} +

"$TARPACK_BIN" scan "$ROOT" --repo "$REPO" --label snap2 --hash
check $? "scan snap2"
"$TARPACK_BIN" pack --repo "$REPO" --snapshot snap2 --target-size 1
check $? "pack snap2"

DEST2="$WORKDIR/dest_snap1_again"
"$TARPACK_BIN" restore --repo "$REPO" --dest "$DEST2" --snapshot snap1
check $? "restore older snapshot snap1 from incremental repo"

compare_trees "$SAVED" "$DEST2"
check $? "older snapshot restore matches saved snap1 state"

# the new files must NOT appear in the snap1 restore
if [ ! -e "$DEST2/delta_new.txt" ]; then pass "snap1 restore omits snap2-only files"; else fail "snap1 restore leaked snap2 file"; fi

# ---------------------------------------------------------------------------
# Missing-object fatal path: corrupt the index so an object cannot resolve,
# and confirm restore fails with exit 2 (fatal).
# ---------------------------------------------------------------------------
echo "--- missing object -> fatal exit 2 ---"
REPO_BAD="$WORKDIR/repo_bad"
cp -a "$REPO" "$REPO_BAD"
: > "$REPO_BAD/objects/objects.jsonl"   # wipe the index -> nothing resolves
DEST_BAD="$WORKDIR/dest_bad"
"$TARPACK_BIN" restore --repo "$REPO_BAD" --dest "$DEST_BAD" --snapshot snap1
rc=$?
if [ "$rc" -eq 2 ]; then pass "missing objects -> fatal exit 2"; else fail "missing objects: expected exit 2, got $rc"; fi

# ---------------------------------------------------------------------------
echo
if [ "$FAIL_COUNT" -ne 0 ]; then
    printf '\n%d check(s) failed\n' "$FAIL_COUNT" >&2
    exit 1
fi
printf '\nall checks passed\n'
exit 0
