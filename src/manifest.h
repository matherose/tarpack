#ifndef TARPACK_MANIFEST_H
#define TARPACK_MANIFEST_H

#include <stdint.h>
#include <stdio.h>
#include <sys/types.h>

/*
 * tp_snapshot_writer: writes a JSONL snapshot manifest to
 * <repo>/snapshots/<label>.jsonl. Creates the snapshots/ directory (and
 * any missing parents of it) if needed. The first line written is always
 * the header; subsequent lines are entries (one JSON object per line).
 */
struct tp_snapshot_writer {
    FILE *f;
    char *path; /* malloc'd path to the .jsonl file, owned by the writer */
    int error;  /* set to errno (or a negative sentinel) on the first failure */
};

/*
 * tp_snapshot_writer_open: creates <repo>/snapshots/ if missing, opens
 * <repo>/snapshots/<label>.jsonl for writing (truncating if it exists),
 * and writes the header line:
 *   {"format":"tarpack-snapshot-v1","created":<unix>,"root":"<abs root>"}
 * Returns 0 on success, -1 on failure (errno set).
 */
int tp_snapshot_writer_open(struct tp_snapshot_writer *w, const char *repo, const char *label,
                             const char *root);

/*
 * tp_extra_times: the secondary timestamps of an entry (v1.3). mtime stays a
 * required first-class field; these are best-effort extras. A *_sec value of
 * TP_TIME_ABSENT means "not available" (e.g. btime on filesystems or kernels
 * that do not expose it) and the corresponding manifest field is omitted.
 * Note ctime (inode change time) and, on Linux, btime (creation time) are
 * recorded for the archive but can never be written back at restore -- no
 * OS API exists to set them.
 */
#define TP_TIME_ABSENT INT64_MIN
struct tp_extra_times {
    int64_t atime_sec;
    long atime_nsec;
    int64_t btime_sec; /* creation ("birth") time */
    long btime_nsec;
    int64_t ctime_sec; /* inode change time; informational only */
    long ctime_nsec;
};

/*
 * tp_snapshot_write_file: appends a "file" entry line. path must be relative
 * to the scan root, '/'-separated, no leading "./". mode_perm is the
 * permission bits (e.g. 0644); it will be formatted as a 4-digit octal
 * string. sha256_hex is optional: pass NULL or "" to omit the "sha256"
 * field entirely (v0.3 hash mode populates it when a digest was computed
 * during scanning; fast-mode-only scans leave it unset). extra is optional:
 * NULL omits all secondary-timestamp fields (same as all-TP_TIME_ABSENT).
 * Returns 0 on success, -1 on failure.
 */
int tp_snapshot_write_file(struct tp_snapshot_writer *w, const char *path, const char *object_id,
                            nlink_t nlink, uid_t uid, gid_t gid, mode_t mode_perm, off_t size,
                            int64_t mtime_sec, long mtime_nsec, dev_t dev, ino_t ino,
                            const char *sha256_hex, const struct tp_extra_times *extra);

/*
 * tp_snapshot_write_dir: appends a "dir" entry line.
 */
int tp_snapshot_write_dir(struct tp_snapshot_writer *w, const char *path, uid_t uid, gid_t gid,
                           mode_t mode_perm, int64_t mtime_sec, long mtime_nsec, dev_t dev,
                           ino_t ino, const struct tp_extra_times *extra);

/*
 * tp_snapshot_write_symlink: appends a "symlink" entry line. target_len is
 * the raw byte length of target (which is not necessarily NUL-terminated
 * text but is treated as a byte buffer, as with path).
 */
int tp_snapshot_write_symlink(struct tp_snapshot_writer *w, const char *path, const char *target,
                               size_t target_len, uid_t uid, gid_t gid, int64_t mtime_sec,
                               long mtime_nsec, dev_t dev, ino_t ino,
                               const struct tp_extra_times *extra);

/*
 * tp_snapshot_writer_close: flushes and closes the underlying file, frees
 * internal storage. Returns 0 on success, -1 if a prior write already
 * failed or the final fflush/fclose fails.
 */
int tp_snapshot_writer_close(struct tp_snapshot_writer *w);

/* --- Reader -------------------------------------------------------------- */

enum tp_manifest_entry_type {
    TP_ENTRY_HEADER,
    TP_ENTRY_FILE,
    TP_ENTRY_DIR,
    TP_ENTRY_SYMLINK,
};

/*
 * tp_manifest_entry: a single decoded line. Only fields relevant to `type`
 * are populated; others are zeroed. path/target are always decoded to raw
 * bytes (base64-decoded if the line used the *_b64 form), and are
 * NUL-terminated for convenience in addition to having an explicit length.
 */
struct tp_manifest_entry {
    enum tp_manifest_entry_type type;

    /* header fields */
    char *format;
    int64_t created;
    char *root;

    /* file/dir/symlink fields */
    char *path;
    size_t path_len;
    char *object_id;   /* file only */
    nlink_t nlink;      /* file only */
    uid_t uid;
    gid_t gid;
    mode_t mode_perm;   /* file/dir only; parsed from octal string */
    off_t size;          /* file only */
    int64_t mtime_sec;
    long mtime_nsec;
    dev_t dev;
    ino_t ino;
    char *target;        /* symlink only */
    size_t target_len;
    char sha256_hex[65]; /* file only; empty string if the "sha256" field was absent */

    /* secondary timestamps; *_sec == TP_TIME_ABSENT when the field was
     * missing (snapshots written before v1.3, or unavailable at scan time) */
    struct tp_extra_times extra;
};

/*
 * tp_manifest_entry_free: frees any malloc'd strings inside *e. Does not
 * free *e itself.
 */
void tp_manifest_entry_free(struct tp_manifest_entry *e);

/*
 * tp_manifest_parse_line: parses one JSONL line (NUL-terminated, no
 * trailing newline required) into *out. Returns 0 on success, -1 on
 * parse failure. Caller must tp_manifest_entry_free(out) when done.
 */
int tp_manifest_parse_line(const char *line, struct tp_manifest_entry *out);

/*
 * tp_manifest_read_file: opens path and calls cb(&entry, user_data) for
 * each line in order (including the header line as TP_ENTRY_HEADER).
 * Stops and returns -1 on the first parse error or if cb returns nonzero.
 * Returns 0 if all lines were read and processed successfully.
 */
typedef int (*tp_manifest_line_cb)(const struct tp_manifest_entry *entry, void *user_data);
int tp_manifest_read_file(const char *path, tp_manifest_line_cb cb, void *user_data);

#endif /* TARPACK_MANIFEST_H */
