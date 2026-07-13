#include "upload.h"

#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/wait.h>
#include <unistd.h>

#include "util.h"

static const char *const kSubdirs[TP_UPLOAD_STEP_COUNT] = {
    "packs",
    "checksums",
    "snapshots",
    "objects",
};

/* join_path: builds "<base>/<subdir>", collapsing a trailing '/' on base so
 * the result never contains a double slash. Returns a malloc'd string, or
 * NULL on allocation failure. */
static char *join_path(const char *base, const char *subdir) {
    size_t base_len = strlen(base);
    while (base_len > 0 && base[base_len - 1] == '/') {
        base_len--;
    }
    size_t subdir_len = strlen(subdir);
    char *out = malloc(base_len + 1 + subdir_len + 1);
    if (out == NULL) {
        return NULL;
    }
    memcpy(out, base, base_len);
    out[base_len] = '/';
    memcpy(out + base_len + 1, subdir, subdir_len + 1);
    return out;
}

static char **build_step_argv(const char *repo, const char *remote, const char *subdir) {
    char *src = join_path(repo, subdir);
    char *dst = join_path(remote, subdir);
    char **argv = calloc(5, sizeof(*argv));
    if (src == NULL || dst == NULL || argv == NULL) {
        free(src);
        free(dst);
        free(argv);
        return NULL;
    }
    argv[0] = strdup("rclone");
    argv[1] = strdup("copy");
    argv[2] = src; /* transferred */
    argv[3] = dst; /* transferred */
    argv[4] = NULL;
    if (argv[0] == NULL || argv[1] == NULL) {
        free(argv[0]);
        free(argv[1]);
        free(argv[2]);
        free(argv[3]);
        free(argv);
        return NULL;
    }
    return argv;
}

static void free_step_argv(char **argv) {
    if (argv == NULL) {
        return;
    }
    for (int i = 0; argv[i] != NULL; i++) {
        free(argv[i]);
    }
    free(argv);
}

int tp_upload_build_argv_set(const char *repo, const char *remote, struct tp_upload_argv_set *out) {
    if (repo == NULL || remote == NULL || out == NULL || repo[0] == '\0' || remote[0] == '\0') {
        return -1;
    }

    memset(out, 0, sizeof(*out));

    for (int i = 0; i < TP_UPLOAD_STEP_COUNT; i++) {
        out->argv[i] = build_step_argv(repo, remote, kSubdirs[i]);
        if (out->argv[i] == NULL) {
            tp_upload_argv_set_free(out);
            return -1;
        }
    }
    return 0;
}

void tp_upload_argv_set_free(struct tp_upload_argv_set *set) {
    if (set == NULL) {
        return;
    }
    for (int i = 0; i < TP_UPLOAD_STEP_COUNT; i++) {
        free_step_argv(set->argv[i]);
        set->argv[i] = NULL;
    }
}

/* run_step: forks and execvp's argv[0](argv), waits for completion. Returns
 * the child's exit status (0-255) on normal exit, 2 if execvp itself failed
 * (rclone not found / not executable), or -1 on a fork/wait failure. */
static int run_step(char **argv) {
    pid_t pid = fork();
    if (pid < 0) {
        tp_warn("upload: fork failed");
        return -1;
    }
    if (pid == 0) {
        execvp(argv[0], argv);
        /* only reached if execvp failed */
        fprintf(stderr, "tarpack: upload: cannot execute '%s': %s\n", argv[0], strerror(errno));
        _exit(127);
    }

    int status = 0;
    pid_t wr;
    do {
        wr = waitpid(pid, &status, 0);
    } while (wr < 0 && errno == EINTR);
    if (wr < 0) {
        tp_warn("upload: waitpid failed");
        return -1;
    }

    if (WIFEXITED(status)) {
        int code = WEXITSTATUS(status);
        if (code == 127) {
            /* distinguish "exec failed" (rclone missing) from a genuine
             * rclone exit code of 127, which is unlikely but not
             * impossible; per spec, exec failure should be reported as a
             * clear message and status 2. We already printed a message
             * above in the child (visible to the user via inherited
             * stderr), so just normalize to 2 here. */
            return 2;
        }
        return code;
    }
    if (WIFSIGNALED(status)) {
        tp_warnx("upload: '%s' killed by signal %d", argv[0], WTERMSIG(status));
        return 128 + WTERMSIG(status);
    }
    return -1;
}

int tp_upload(const char *repo, const char *remote) {
    struct tp_upload_argv_set set;
    if (tp_upload_build_argv_set(repo, remote, &set) != 0) {
        tp_warnx("upload: invalid repo/remote arguments");
        return 2;
    }

    int rc = 0;
    for (int i = 0; i < TP_UPLOAD_STEP_COUNT; i++) {
        int step_rc = run_step(set.argv[i]);
        if (step_rc != 0) {
            rc = step_rc < 0 ? 2 : step_rc;
            break;
        }
    }

    tp_upload_argv_set_free(&set);
    return rc;
}
