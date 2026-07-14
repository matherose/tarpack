# tarpack

[![CI](https://github.com/matherose/tarpack/actions/workflows/ci.yml/badge.svg)](https://github.com/matherose/tarpack/actions/workflows/ci.yml)

Pack an rsync-style backup tree into large, cloud-friendly `tar.zst` archives — with deduplication, integrity verification, rclone upload, and full restore.

tarpack is built for one job: you have a NAS holding rsync snapshots (millions of small files, lots of hardlinks) and you want to cold-store them remotely as a small number of big, checksummed archives instead of syncing the file soup directly. Each pack is a plain single-frame `tar --zstd` archive, so even without tarpack you can always get your data back with stock `tar`.

## How it works

```
source tree ──scan──▶ snapshot manifest ──pack──▶ pack-NNNNNN.tar.zst (~50 GB each)
                      (JSONL, per run)            + per-pack JSON manifest
                                                  + append-only object index
                                                  + SHA256SUMS
                                     ──verify──▶  checksums / deep per-object re-hash
                                     ──upload──▶  rclone remote (packs first, metadata last)
                                     ──restore──▶ byte-identical tree, hardlinks and all
```

- **Deduplication.** Hardlinks are detected within a scan via `(dev, ino)` and stored once. Across runs, objects already in the index are skipped — keyed on `(path, size, mtime)` by default, or on SHA-256 content with `--hash`.
- **Content-aware compression.** Files whose leading 64 KiB is already high-entropy (media, archives, encrypted data) are routed into separate store-level packs written at zstd level 1 — same tar.zst format, no CPU wasted re-compressing incompressible bytes.
- **Crash safety.** Packs are written to a `.part` file, fsynced, and renamed; the object index is appended last, as the single source of truth. A run interrupted at any point resumes cleanly: torn index lines are truncated, unreferenced packs deleted, and their objects re-packed.
- **Interop.** Packs are pax-format tar in one zstd frame: `tar --zstd -tf pack-000001.tar.zst` just works. Each entry carries the original mode, uid/gid *and* user/group names, and the full set of nanosecond timestamps (mtime, atime, ctime, plus the creation date via the pax `LIBARCHIVE.creationtime` extension), so even a plain `tar -x` restores sensible metadata. Hardlinks, symlinks, and directory metadata live in the snapshot manifest and are reproduced exactly by `tarpack restore`.

## Building

Requires a C11 compiler, CMake ≥ 3.15, and **libarchive with zstd support** (the only system dependency — SHA-256, cJSON, and uthash are vendored).

```sh
# macOS (Apple ships libarchive without headers; use Homebrew's)
brew install libarchive
export PKG_CONFIG_PATH="$(brew --prefix libarchive)/lib/pkgconfig"

# Debian/Ubuntu
sudo apt install libarchive-dev

cmake -B build -DCMAKE_BUILD_TYPE=Release
cmake --build build
build/tarpack --version
```

Debug builds (`-DCMAKE_BUILD_TYPE=Debug`) enable AddressSanitizer and UBSan.

## Quick start

```sh
REPO=/backups/tarpack-repo

# 1. Scan the source tree into a snapshot manifest
tarpack scan /mnt/nas/rsync-tree --repo "$REPO"

# 2. Pack new objects into ~50 GB archives
tarpack pack --repo "$REPO"

# 3. Verify pack checksums (add --objects to re-hash every object)
tarpack verify --repo "$REPO" --objects

# 4. Mirror the repository to an rclone remote
tarpack upload --repo "$REPO" --remote b2-crypt:backups/nas

# 5. Restore (latest snapshot by default)
tarpack restore --repo "$REPO" --dest /mnt/restore
```

Incremental use is the same commands again: a new `scan` records a new snapshot, and `pack` only packs objects not already in the index.

## Commands

Every command except `verify` accepts `-v`/`--verbose` for a per-item progress line on stderr (`verify` already reports per item by design).

### `tarpack scan <root> --repo <repodir> [--label <name>] [--hash] [-v]`

Walks `<root>` (an `openat`-based walker; symlinks are recorded, never followed) and writes a snapshot manifest to `<repodir>/snapshots/<label>.jsonl`. The label defaults to the current UTC time (`YYYY-MM-DDTHH-MM-SS`). With `--hash`, files are SHA-256'd during the scan and dedup becomes content-based instead of `(path, size, mtime)`.

### `tarpack pack --repo <repodir> [--snapshot <label>] [--target-size <size>] [--pack-algo next-fit|ffd] [-v]`

Packs every object from the snapshot (latest by default) that is not yet in the object index. `--target-size` accepts `K`/`M`/`G` suffixes (base 1024) and defaults to `50G`; a single object larger than the target gets its own pack. `next-fit` (default) keeps directory locality by packing in path order; `ffd` (first-fit decreasing) trades locality for tighter bins. Interrupted runs are recovered automatically on the next invocation.

Packing is content-aware: each file ≥ 64 KiB has its first 64 KiB sampled, and files measuring ≥ 7.5 bits/byte of Shannon entropy (already-compressed media, archives, encrypted data) are grouped into separate packs written at zstd level 1 instead of the default effort. Classification never affects correctness — packs stay ordinary tar.zst — and any doubt (short file, read error) falls back to normal compression.

### `tarpack verify --repo <repodir> [--objects]`

Recomputes the SHA-256 of every file listed in `checksums/SHA256SUMS` (the file is `shasum -c` compatible). With `--objects`, additionally streams every pack and re-hashes each entry against the pack manifest, detecting membership drift in both directions.

### `tarpack restore --repo <repodir> --dest <dir> [--snapshot <label>] [-v]`

Rebuilds the tree from the latest (or named) snapshot: directories first, then one sequential pass per pack, extracting each object once and recreating additional hardlink names with `link()`. Symlink targets are restored byte-for-byte and never resolved. Modes, nanosecond mtimes, and atimes are restored (directories last, deepest-first); creation dates are restored on macOS/BSD (Linux offers no API to set them — the values still live in the manifest and pax headers); ownership only when running as root. ctime is recorded at scan time but is kernel-managed and can never be written back. Every snapshot path is validated first — absolute paths or `..` components abort the restore before anything touches disk.

### `tarpack upload --repo <repodir> --remote <rclone-remote-path> [-v]`

Runs `rclone copy` for `packs/`, `checksums/`, `snapshots/`, and `objects/` — in that order, bulk data first, so the remote never lists metadata that outruns its pack bytes. Each rclone call is a plain `fork`+`execvp` (no shell involved). A non-zero rclone exit stops the sequence and is propagated.

### Exit codes

| code | meaning |
|------|---------|
| 0    | success |
| 1    | completed with warnings (e.g. a file changed mid-pack, a hash mismatch on restore) |
| 2    | fatal error |
| 64   | usage error |

## Repository layout

```
repo/
  snapshots/<label>.jsonl   # one manifest per scan: files, dirs, symlinks + metadata
  packs/pack-NNNNNN.tar.zst # the archives (plus a .json manifest per pack)
  objects/objects.jsonl     # append-only index: object_id → size, sha256, pack, offset
  checksums/SHA256SUMS      # shasum -c compatible digests of every committed pack
```

All manifests are JSONL (one JSON object per line) and safe to inspect with `jq`. Non-UTF-8 paths are stored base64-encoded under `path_b64`.

## Development

```sh
cmake -B build-debug -DCMAKE_BUILD_TYPE=Debug
cmake --build build-debug
ctest --test-dir build-debug --output-on-failure
```

The suite is 20 tests (unit + shell-driven integration), including a full scan→pack→verify→restore roundtrip with hardlinks, dangling symlinks, non-ASCII names, and nanosecond mtimes, a kill-and-resume crash test, a content-aware packing roundtrip, and an atime/creation-date restore test. CI runs the matrix ubuntu/macos × Debug/Release with leak detection on Linux.

The original design document lives in [`deep-research-report.md`](deep-research-report.md).

## License

[WTFPL](LICENSE). Vendored components keep their own licenses: cJSON (MIT), uthash (BSD), B-Con SHA-256 (public domain).
