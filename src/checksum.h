#ifndef TARPACK_CHECKSUM_H
#define TARPACK_CHECKSUM_H

#include <stddef.h>

/*
 * checksums/SHA256SUMS: a flat, append-only, `shasum -a 256` compatible
 * manifest of every pack archive and pack manifest ever written to a
 * repository, in the form:
 *   <sha256 hex>  <path relative to repo, e.g. "packs/pack-000001.tar.zst">\n
 * (two spaces between hash and path, matching GNU/BSD shasum's "text mode"
 * output, so `shasum -c` against this file works unmodified from the repo
 * root).
 */

/*
 * tp_checksums_append: appends one "<sha256_hex>  <relpath>\n" line to
 * <repo>/checksums/SHA256SUMS, creating the checksums/ directory and file
 * as needed. Opens with O_APPEND, writes, then fsyncs the file (and, on the
 * first-ever creation, the containing directory) before returning. Returns
 * 0 on success, -1 on failure.
 */
int tp_checksums_append(const char *repo, const char *sha256_hex, const char *relpath);

/*
 * tp_checksums_line_entry: one decoded line of a SHA256SUMS file.
 */
struct tp_checksums_line_entry {
    char sha256_hex[65];
    char *relpath; /* malloc'd */
};

/*
 * tp_checksums_read: parses <repo>/checksums/SHA256SUMS (two-space
 * "<hex>  <path>" lines, as written by tp_checksums_append / GNU shasum) into
 * a malloc'd array. Returns 0 on success (including "file does not exist",
 * which yields *count_out == 0 and *entries_out == NULL) or -1 on a genuine
 * I/O/parse failure. Caller frees the array with tp_checksums_entries_free.
 */
int tp_checksums_read(const char *repo, struct tp_checksums_line_entry **entries_out,
                       size_t *count_out);

/*
 * tp_checksums_entries_free: frees an array returned by tp_checksums_read.
 */
void tp_checksums_entries_free(struct tp_checksums_line_entry *entries, size_t count);

/*
 * tp_sha256_file: computes the sha256 of the file at path by reading its
 * bytes in chunks. Writes the lowercase hex digest (65 bytes incl. NUL) into
 * out_hex. Returns 0 on success, -1 if the file cannot be opened/read.
 */
int tp_sha256_file(const char *path, char out_hex[65]);

#endif /* TARPACK_CHECKSUM_H */
