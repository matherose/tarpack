#ifndef TARPACK_RESTORE_H
#define TARPACK_RESTORE_H

#include <stddef.h>
#include <stdint.h>
#include <sys/types.h>

#include "manifest.h" /* struct tp_extra_times */

/*
 * tarpack restore: reconstruct a scanned tree from a repository into a
 * destination directory.
 *
 *   tp_restore(repo, dest, snapshot_label)
 *
 * Loads the chosen snapshot (snapshot_label == NULL selects the latest by
 * lexicographic filename sort, the same rule as pack) and the cross-run
 * object index. Every "file" entry's object_id must resolve to a pack via the
 * index; a missing object is a fatal error. Objects are grouped by pack and
 * each pack is read once (sequentially) with libarchive; each object's data is
 * streamed to the FIRST snapshot path that references it under dest. Remaining
 * hardlink names are created with link(), symlinks with symlink() (target
 * bytes verbatim). Directory modes/mtimes and file modes/mtimes are restored;
 * ownership only when running as root.
 *
 * Returns 0 on full success, 1 if it completed but emitted warnings (e.g. a
 * per-file hash mismatch or a non-fatal metadata restore failure), or -1 on a
 * fatal error (missing objects, unreadable snapshot, path traversal, etc.).
 * The main() wrapper maps -1 -> exit 2, 1 -> exit 1, 0 -> exit 0, and a usage
 * error -> exit 64.
 */
int tp_restore(const char *repo, const char *dest, const char *snapshot_label);

/*
 * tp_restore_path_safe: returns 1 if `path` (a relative snapshot path of
 * `len` raw bytes, NUL-terminated) is safe to extract under a destination
 * directory, 0 otherwise. A path is rejected if it is empty, absolute (leading
 * '/'), or contains a ".." component (a segment that is exactly "..", between
 * path separators or at either end). "weird but legal" names -- embedded
 * spaces, quotes, dots that are not a lone "..", multibyte UTF-8, a leading
 * "./"-free relative name -- are accepted. This is a pure function with no
 * filesystem access, exposed for unit testing.
 */
int tp_restore_path_safe(const char *path, size_t len);

/*
 * tp_restore_file_ref: one "file" reference from a snapshot -- a path and the
 * object_id it resolves to. Paths are raw bytes, NUL-terminated, with an
 * explicit length.
 */
struct tp_restore_file_ref {
    char *object_id; /* malloc'd */
    char *path;      /* malloc'd, raw bytes */
    size_t path_len;
};

/*
 * tp_restore_link_group: all snapshot paths that share one object_id, in
 * first-seen (snapshot) order. refs[0].path is the extraction target (the
 * path the object's data is streamed to); refs[1..] are additional hardlink
 * names to be created with link() to refs[0].path.
 */
struct tp_restore_link_group {
    char *object_id; /* malloc'd */
    struct tp_restore_file_ref *refs;
    size_t count;
    size_t capacity;

    /* metadata copied from the FIRST file entry seen for this object_id
     * (hardlinked names share one inode, so one set of metadata suffices) */
    uid_t uid;
    gid_t gid;
    mode_t mode_perm;
    off_t size;
    int64_t mtime_sec;
    long mtime_nsec;
    struct tp_extra_times extra; /* atime/btime best-effort; ctime informational */
};

/*
 * tp_restore_link_set: all link groups collected from a snapshot, in the order
 * each object_id is first seen.
 */
struct tp_restore_link_set {
    struct tp_restore_link_group *groups;
    size_t count;
    size_t capacity;
};

/*
 * tp_restore_collect_links: reads the snapshot JSONL at snapshot_path and
 * groups its "file" entries by object_id (first-seen order for both groups and
 * the paths within a group). refs[0] of each group is the first path listed
 * for that object_id -- the hardlink target. Returns 0 on success, -1 on
 * read/parse/allocation failure. Caller frees via tp_restore_link_set_free.
 */
int tp_restore_collect_links(const char *snapshot_path, struct tp_restore_link_set *out);

/*
 * tp_restore_link_set_free: frees a set filled by tp_restore_collect_links.
 * Safe to call on a zero-initialized set.
 */
void tp_restore_link_set_free(struct tp_restore_link_set *s);

#endif /* TARPACK_RESTORE_H */
