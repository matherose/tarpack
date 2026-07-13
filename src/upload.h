#ifndef TARPACK_UPLOAD_H
#define TARPACK_UPLOAD_H

/*
 * tarpack upload: a thin wrapper around `rclone copy` that mirrors a
 * repository's packs/, checksums/, snapshots/, and objects/ subdirectories
 * to a remote rclone destination, packs (bulk data) first and metadata
 * last, so a reader of the remote never sees index/checksum entries that
 * outrun the pack bytes they describe.
 *
 * No system() is used anywhere; each rclone invocation is a plain
 * fork+execvp of the literal argv built by tp_upload_build_argv_set below,
 * so there is no shell quoting/injection surface.
 */

/*
 * TP_UPLOAD_STEP_COUNT: number of rclone invocations tp_upload performs,
 * one per repository subdirectory, in this fixed order: packs, checksums,
 * snapshots, objects.
 */
#define TP_UPLOAD_STEP_COUNT 4

/*
 * tp_upload_argv_set: the argv arrays for one `tarpack upload` invocation,
 * one per repository subdirectory step, in upload order (packs first,
 * metadata last). Each step's argv is a malloc'd NULL-terminated array of
 * malloc'd strings, execvp-ready: {"rclone", "copy", "<repo>/<subdir>",
 * "<remote>/<subdir>", NULL}.
 */
struct tp_upload_argv_set {
    char **argv[TP_UPLOAD_STEP_COUNT];
};

/*
 * tp_upload_build_argv_set: builds the argv arrays for uploading `repo` to
 * `remote` via rclone, in the fixed step order documented above. Returns 0
 * on success (out populated, caller must tp_upload_argv_set_free it), -1 on
 * allocation failure or if repo/remote/out is NULL or empty.
 */
int tp_upload_build_argv_set(const char *repo, const char *remote,
                              struct tp_upload_argv_set *out);

/*
 * tp_upload_argv_set_free: frees every argv array and string built by
 * tp_upload_build_argv_set. Safe to call on a zero-initialized set.
 */
void tp_upload_argv_set_free(struct tp_upload_argv_set *set);

/*
 * tp_upload: the top-level `tarpack upload --repo <repo> --remote <remote>`
 * operation. Runs the TP_UPLOAD_STEP_COUNT rclone invocations in order via
 * fork+execvp (never system()). If execvp itself fails (e.g. rclone is not
 * on PATH), prints a clear message and returns 2. If any rclone invocation
 * exits non-zero, its exit status is propagated (returned) immediately
 * without running subsequent steps. Returns 0 if every step's rclone
 * process exits 0.
 */
int tp_upload(const char *repo, const char *remote);

#endif /* TARPACK_UPLOAD_H */
