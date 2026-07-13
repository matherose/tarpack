#ifndef TARPACK_SCANNER_H
#define TARPACK_SCANNER_H

/*
 * tp_scan: walks the directory tree rooted at `root`, writing a snapshot
 * manifest to <repo>/snapshots/<label>.jsonl (see manifest.h). The repo
 * directory itself is skipped if it lives inside root (compared by
 * dev+ino, so bind mounts / symlink tricks don't matter).
 *
 * Traversal never follows symlinks. Directory entries are processed in
 * sorted (byte-lexicographic) order within each directory for deterministic
 * output. Regular files sharing (dev,ino) -- hardlinks -- are recorded
 * once in the object table and reference the same object id in every
 * manifest entry. Unsupported entry types (sockets, FIFOs, devices) are
 * skipped with a tp_warnx() notice.
 *
 * Index-aware dedup (v0.3): the cross-run object index at
 * <repo>/objects/objects.jsonl (see index.h) is loaded before scanning.
 * Fresh object ids are allocated starting one past the highest existing
 * "O<n>" id seen in the index, so ids never collide across runs.
 *
 * For each newly-encountered (dev,ino) (i.e. the first name seen for that
 * inode in this scan; subsequent hardlinked names always reuse whatever id
 * the first name resolved to):
 *   - Fast mode (always on): if (relative path, size, mtime_sec,
 *     mtime_nsec) exactly matches an index entry, the file reuses that
 *     entry's object_id (the object is already packed) instead of getting a
 *     fresh id.
 *   - Hash mode (hash_mode != 0): for files NOT matched by the fast path,
 *     the file's content is streamed and SHA-256 hashed; if that digest
 *     matches an index entry's sha256, the file reuses that object_id
 *     (catches renamed/copied files that fast mode cannot detect) and the
 *     digest is recorded in the snapshot entry's "sha256" field regardless
 *     of whether it matched. Fast-mode hits do not require hashing.
 *
 * A one-line summary of new vs. already-packed object counts is printed to
 * stderr at the end of the scan.
 *
 * Returns 0 if the scan completed with no errors, 1 if it completed but
 * one or more files/directories could not be read (warnings were
 * printed), or -1 on a fatal error that prevented scanning at all (e.g.
 * root does not exist / is not a directory, or the manifest could not be
 * opened for writing).
 */
int tp_scan(const char *root, const char *repo, const char *label, int hash_mode);

#endif /* TARPACK_SCANNER_H */
