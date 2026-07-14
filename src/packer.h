#ifndef TARPACK_PACKER_H
#define TARPACK_PACKER_H

#include <stddef.h>
#include <stdint.h>
#include <sys/types.h>

/*
 * Pack writer (multi-pack since milestone v1.0).
 *
 * tp_pack_multi reads a snapshot JSONL manifest, collects the unique
 * regular-file objects it references that are not yet in the cross-run
 * object index, and writes them into one or more zstd-compressed pax tar
 * archives <repo>/packs/pack-NNNNNN.tar.zst -- split so each pack's
 * estimated UNCOMPRESSED tar size stays under a target (--target-size) --
 * each alongside a pack manifest <repo>/packs/pack-NNNNNN.json describing
 * the byte offset (in the uncompressed tar stream) and sha256 of each
 * object's data. After each pack commits, its archive and manifest hashes
 * are appended to <repo>/checksums/SHA256SUMS and its objects to the index.
 */

/*
 * tp_packed_object: one unique object selected from a snapshot for packing.
 * The representative path is the FIRST path listed for this object_id in the
 * snapshot (hardlinks share one object_id but are packed only once). path is
 * a malloc'd buffer of raw bytes (NOT necessarily valid UTF-8, hence the
 * explicit length), NUL-terminated for convenience.
 */
struct tp_packed_object {
    char *object_id; /* malloc'd, e.g. "O1" */
    char *path;      /* malloc'd representative relative path, raw bytes */
    size_t path_len;

    /* metadata copied from the representative snapshot file entry */
    uid_t uid;
    gid_t gid;
    mode_t mode_perm;
    off_t size;
    int64_t mtime_sec;
    long mtime_nsec;

    /* filled in during packing (empty/0 until then) */
    char sha256_hex[65];
    int64_t offset; /* byte offset of file data in the uncompressed tar stream */
};

/*
 * tp_pack_object_set: dynamic array of tp_packed_object in first-seen order.
 */
struct tp_pack_object_set {
    struct tp_packed_object *objects;
    size_t count;
    size_t capacity;
};

/*
 * tp_pack_object_set_free: frees the objects array and every malloc'd string
 * inside it. Safe to call on a zero-initialized set.
 */
void tp_pack_object_set_free(struct tp_pack_object_set *s);

/*
 * tp_pack_collect: reads the snapshot JSONL at snapshot_path and appends,
 * in first-seen order, one tp_packed_object per unique object_id referenced
 * by a "file" entry. Only "file" entries contribute; dir/symlink/header
 * lines are ignored. The representative path/metadata come from the FIRST
 * file entry seen for each object_id.
 *
 * Returns 0 on success, -1 on read/parse/allocation failure. On success the
 * "root" recorded in the snapshot header is copied into *root_out (malloc'd,
 * caller frees) when root_out is non-NULL.
 */
int tp_pack_collect(const char *snapshot_path, struct tp_pack_object_set *out, char **root_out);

/*
 * tp_pack_manifest_write: writes the pack manifest JSON document to path
 * (a single JSON object, not JSONL):
 *   {"format":"tarpack-pack-v1","pack":"<pack_name>","created":<unix>,
 *    "entries":[{"object_id","path"|"path_b64","offset","size","sha256"}...]}
 * The path field uses "path" when the raw representative path is valid UTF-8,
 * otherwise "path_b64" (base64), matching the snapshot encoding rule.
 * Each entry's offset/size/sha256 are taken from the tp_packed_object.
 * Returns 0 on success, -1 on failure.
 */
int tp_pack_manifest_write(const char *path, const char *pack_name, int64_t created,
                            const struct tp_pack_object_set *set);

/*
 * tp_pack_manifest_entry: one decoded pack-manifest entry. path holds raw
 * bytes (base64-decoded if the wire form used path_b64), NUL-terminated.
 */
struct tp_pack_manifest_entry {
    char *object_id;
    char *path;
    size_t path_len;
    int64_t offset;
    off_t size;
    char sha256_hex[65];
};

/*
 * tp_pack_manifest_read: parses the pack manifest JSON at path. On success
 * sets *entries_out to a malloc'd array of *count_out entries (caller frees
 * via tp_pack_manifest_entries_free) and copies "format"/"pack"/"created"
 * into the provided out-params when non-NULL. Returns 0 on success, -1 on
 * failure.
 */
int tp_pack_manifest_read(const char *path, char **format_out, char **pack_out,
                           int64_t *created_out, struct tp_pack_manifest_entry **entries_out,
                           size_t *count_out);

/*
 * tp_pack_manifest_entries_free: frees an array returned by
 * tp_pack_manifest_read.
 */
void tp_pack_manifest_entries_free(struct tp_pack_manifest_entry *entries, size_t count);

/* ---------------------------------------------------------------------- */
/* Multi-pack splitting (milestone v1.0)                                   */
/* ---------------------------------------------------------------------- */
/* (The v0.2/v0.3 single-pack tp_pack() entry point was superseded by
 * tp_pack_multi below; a single-pack result is simply the degenerate case
 * where every object fits under the target size.)                         */

/*
 * TP_DEFAULT_TARGET_SIZE: default --target-size when the user does not pass
 * one, in bytes (50 GiB, base 1024).
 */
#define TP_DEFAULT_TARGET_SIZE ((int64_t)50 * 1024 * 1024 * 1024)

/*
 * tp_parse_size: parses a size string of the form "<number>[K|M|G]"
 * (case-insensitive suffix, base 1024; no suffix means plain bytes) into
 * *out_bytes. Returns 0 on success, -1 if the string is empty, has trailing
 * garbage, is not a valid non-negative integer, or overflows. Leading/
 * trailing whitespace is not accepted (caller is expected to pass a bare
 * CLI argument).
 */
int tp_parse_size(const char *s, int64_t *out_bytes);

/*
 * tp_estimate_entry_cost: estimated contribution of one object of size
 * `size` bytes to the UNCOMPRESSED tar stream, for the purpose of bin
 * packing decisions: a 512-byte ustar header, the data rounded up to the
 * next 512-byte block, plus a conservative flat 1536-byte allowance for a
 * possible pax extended header set. Deterministic; a slight overestimate/
 * underestimate versus the real archive is acceptable (see spec).
 */
int64_t tp_estimate_entry_cost(off_t size);

/*
 * tp_bin_item: one input to the pure bin-assignment functions below: an
 * opaque numeric id (typically an index into the caller's object array) and
 * its precomputed estimated cost (see tp_estimate_entry_cost).
 */
struct tp_bin_item {
    size_t id;
    int64_t cost;
};

/*
 * tp_bin_assign_next_fit: assigns each item in `items` (in the given input
 * order) to a bin, writing the chosen bin index (0-based, in the order bins
 * are first opened) into the parallel `bin_out` array (must have room for
 * `count` entries). Algorithm: path-ordered next-fit -- walk items in input
 * order; if the current bin is non-empty and adding the next item would
 * exceed `target`, close the current bin and start a new one. An item whose
 * own cost exceeds `target` is placed alone in its own bin (it never merges
 * with a neighbor, even if that neighbor bin is empty). `target` must be
 * >= 1. Returns the number of bins used (>= 1 for count >= 1, 0 for
 * count == 0), or -1 on invalid input (items/bin_out NULL while count > 0,
 * or target <= 0).
 */
long tp_bin_assign_next_fit(const struct tp_bin_item *items, size_t count, int64_t target,
                             size_t *bin_out);

/*
 * tp_bin_assign_ffd: assigns each item to a bin using First-Fit Decreasing:
 * items are conceptually sorted by cost descending (ties broken by
 * ascending original input index, for determinism), then each item is
 * placed into the first already-open bin with enough remaining room, or a
 * new bin if none fits. An item whose own cost exceeds `target` always
 * starts (alone in) a brand new bin -- it is never placed alongside another
 * item, and no other item is later placed into its bin (it is immediately
 * considered "full"). bin_out is indexed by the ORIGINAL input position
 * (i.e. bin_out[i] is the bin assigned to items[i]), so callers do not need
 * to track the internal sort permutation. Bin indices are assigned in the
 * order bins are first opened during the descending-cost walk. Returns the
 * number of bins used, or -1 on invalid input (same rules as next-fit).
 */
long tp_bin_assign_ffd(const struct tp_bin_item *items, size_t count, int64_t target,
                        size_t *bin_out);

/*
 * tp_pack_algo: which bin-assignment algorithm tp_pack_multi uses to split
 * newly-collected objects across multiple packs.
 */
enum tp_pack_algo {
    TP_PACK_ALGO_NEXT_FIT = 0, /* default: path-ordered next-fit */
    TP_PACK_ALGO_FFD = 1,      /* --pack-algo ffd: First-Fit Decreasing */
};

/*
 * Content-aware packing (v1.2). Objects at least TP_PACK_ENTROPY_SAMPLE
 * bytes long whose leading TP_PACK_ENTROPY_SAMPLE-byte sample measures at
 * least TP_PACK_ENTROPY_MIN_BITS bits/byte of Shannon entropy are treated
 * as already-compressed data (media, archives, encrypted blobs) and are
 * packed into separate store-level packs written at zstd level
 * TP_PACK_STORE_ZSTD_LEVEL -- still ordinary tar.zst, just without wasting
 * CPU re-compressing incompressible bytes at default effort.
 */
#define TP_PACK_ENTROPY_SAMPLE ((size_t)(64 * 1024))
#define TP_PACK_ENTROPY_MIN_BITS 7.5
#define TP_PACK_STORE_ZSTD_LEVEL 1

/*
 * tp_sample_is_incompressible: samples the first TP_PACK_ENTROPY_SAMPLE
 * bytes of <root_fd>/<path> and returns 1 if the file should be routed to a
 * store-level pack, 0 otherwise. Files shorter than the sample size, and any
 * open/read problem, classify as 0 (compressible): misclassifying in that
 * direction costs only CPU, never correctness. When bits_out is non-NULL it
 * receives the measured entropy in bits/byte (0.0 when no full sample was
 * read).
 */
int tp_sample_is_incompressible(int root_fd, const char *path, int64_t size, double *bits_out);

/*
 * tp_pack_multi: the v1.0 top-level pack operation. Behaves like tp_pack
 * (same snapshot resolution / index-skip semantics) but splits the objects
 * that still need storing across as many pack-NNNNNN files as required to
 * respect target_size_bytes (estimated on the uncompressed tar stream; see
 * tp_estimate_entry_cost), using `algo` to decide the split. Each pack is
 * written, fsynced, renamed, manifested, checksummed (checksums/SHA256SUMS)
 * and appended to the object index -- in that order -- before the next pack
 * starts, so a crash between packs leaves a fully consistent repository.
 *
 * target_size_bytes must be >= 1 (callers typically pass
 * TP_DEFAULT_TARGET_SIZE or a --target-size value parsed by tp_parse_size).
 *
 * Returns 0 on full success, 1 if it completed but emitted warnings, or -1
 * on a fatal error. Same semantics as tp_pack otherwise.
 */
int tp_pack_multi(const char *repo, const char *snapshot_label, int64_t target_size_bytes,
                   enum tp_pack_algo algo);

#endif /* TARPACK_PACKER_H */
