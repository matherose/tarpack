#ifndef TARPACK_VERIFY_H
#define TARPACK_VERIFY_H

/*
 * tarpack verify: integrity checking for a repository.
 *
 * Base mode (deep_objects == 0):
 *   - Parse <repo>/checksums/SHA256SUMS.
 *   - Recompute sha256 of every listed file; report OK/FAILED per file
 *     (mirrors `shasum -c` output).
 *   - Flag pack files present on disk under <repo>/packs but missing from
 *     SHA256SUMS.
 *   - Flag SHA256SUMS entries whose file does not exist on disk.
 *   - Flag <repo>/objects/objects.jsonl entries whose referenced pack file
 *     does not exist under <repo>/packs.
 *
 * --objects deep mode (deep_objects != 0), in addition to the above:
 *   - Open each pack-*.tar.zst with libarchive's read API
 *     (archive_read_support_filter_all + archive_read_support_format_all),
 *     stream every entry, recompute its sha256, and compare against that
 *     pack's JSON manifest entries (matched by pathname; sizes are also
 *     compared). Report a per-pack summary.
 *
 * Returns 0 if everything checked out clean, 1 if any mismatch/missing
 * file/inconsistency was found (matching `shasum -c`'s exit convention).
 * A fatal setup error (e.g. repo does not exist) also returns 1 after
 * printing a message; there is no separate "fatal" exit code for verify.
 */
int tp_verify(const char *repo, int deep_objects);

#endif /* TARPACK_VERIFY_H */
