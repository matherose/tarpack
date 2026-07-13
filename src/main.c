#include <stdio.h>
#include <string.h>
#include <time.h>

#include "packer.h"
#include "scanner.h"
#include "upload.h"
#include "verify.h"

static void print_usage(void) {
    fprintf(stderr,
            "usage: tarpack <command> [options]\n"
            "\n"
            "commands:\n"
            "  scan      scan a directory tree\n"
            "  pack      create an archive\n"
            "  verify    verify an archive\n"
            "  restore   restore from an archive\n"
            "  upload    upload a repository to a remote via rclone\n"
            "\n"
            "  --version  print version and exit\n");
}

static int cmd_not_implemented(const char *cmd) {
    fprintf(stderr, "%s: not implemented yet\n", cmd);
    return 2;
}

/* default_label: "YYYY-MM-DDTHH-MM-SS" in UTC, written into buf (must be
 * at least 20 bytes). */
static void default_label(char *buf, size_t buf_size) {
    time_t now = time(NULL);
    struct tm tm_utc;
    gmtime_r(&now, &tm_utc);
    strftime(buf, buf_size, "%Y-%m-%dT%H-%M-%S", &tm_utc);
}

static int cmd_scan(int argc, char **argv) {
    const char *root = NULL;
    const char *repo = NULL;
    const char *label = NULL;
    int hash_mode = 0;

    for (int i = 2; i < argc; i++) {
        if (strcmp(argv[i], "--repo") == 0 && i + 1 < argc) {
            repo = argv[++i];
        } else if (strcmp(argv[i], "--label") == 0 && i + 1 < argc) {
            label = argv[++i];
        } else if (strcmp(argv[i], "--hash") == 0) {
            hash_mode = 1;
        } else if (root == NULL) {
            root = argv[i];
        } else {
            fprintf(stderr, "scan: unrecognized argument: %s\n", argv[i]);
            return 64;
        }
    }

    if (root == NULL || repo == NULL) {
        fprintf(stderr, "usage: tarpack scan <root> --repo <repodir> [--label <name>] [--hash]\n");
        return 64;
    }

    char label_buf[32];
    if (label == NULL) {
        default_label(label_buf, sizeof(label_buf));
        label = label_buf;
    }

    int rc = tp_scan(root, repo, label, hash_mode);
    if (rc < 0) {
        return 1;
    }
    return rc; /* 0 = clean, 1 = completed with warnings */
}

static int cmd_pack(int argc, char **argv) {
    const char *repo = NULL;
    const char *snapshot = NULL;
    int64_t target_size = TP_DEFAULT_TARGET_SIZE;
    enum tp_pack_algo algo = TP_PACK_ALGO_NEXT_FIT;

    for (int i = 2; i < argc; i++) {
        if (strcmp(argv[i], "--repo") == 0 && i + 1 < argc) {
            repo = argv[++i];
        } else if (strcmp(argv[i], "--snapshot") == 0 && i + 1 < argc) {
            snapshot = argv[++i];
        } else if (strcmp(argv[i], "--target-size") == 0 && i + 1 < argc) {
            if (tp_parse_size(argv[++i], &target_size) != 0) {
                fprintf(stderr, "pack: invalid --target-size '%s'\n", argv[i]);
                return 64;
            }
        } else if (strcmp(argv[i], "--pack-algo") == 0 && i + 1 < argc) {
            const char *v = argv[++i];
            if (strcmp(v, "next-fit") == 0) {
                algo = TP_PACK_ALGO_NEXT_FIT;
            } else if (strcmp(v, "ffd") == 0) {
                algo = TP_PACK_ALGO_FFD;
            } else {
                fprintf(stderr, "pack: unrecognized --pack-algo '%s' (expected next-fit or ffd)\n",
                        v);
                return 64;
            }
        } else {
            fprintf(stderr, "pack: unrecognized argument: %s\n", argv[i]);
            return 64;
        }
    }

    if (repo == NULL) {
        fprintf(stderr,
                "usage: tarpack pack --repo <repodir> [--snapshot <label>] "
                "[--target-size <bytes>] [--pack-algo next-fit|ffd]\n");
        return 64;
    }

    int rc = tp_pack_multi(repo, snapshot, target_size, algo);
    if (rc < 0) {
        return 2; /* fatal */
    }
    return rc; /* 0 = clean, 1 = completed with warnings */
}

static int cmd_verify(int argc, char **argv) {
    const char *repo = NULL;
    int deep_objects = 0;

    for (int i = 2; i < argc; i++) {
        if (strcmp(argv[i], "--repo") == 0 && i + 1 < argc) {
            repo = argv[++i];
        } else if (strcmp(argv[i], "--objects") == 0) {
            deep_objects = 1;
        } else {
            fprintf(stderr, "verify: unrecognized argument: %s\n", argv[i]);
            return 64;
        }
    }

    if (repo == NULL) {
        fprintf(stderr, "usage: tarpack verify --repo <repodir> [--objects]\n");
        return 64;
    }

    int rc = tp_verify(repo, deep_objects);
    return rc != 0 ? 1 : 0;
}

static int cmd_upload(int argc, char **argv) {
    const char *repo = NULL;
    const char *remote = NULL;

    for (int i = 2; i < argc; i++) {
        if (strcmp(argv[i], "--repo") == 0 && i + 1 < argc) {
            repo = argv[++i];
        } else if (strcmp(argv[i], "--remote") == 0 && i + 1 < argc) {
            remote = argv[++i];
        } else {
            fprintf(stderr, "upload: unrecognized argument: %s\n", argv[i]);
            return 64;
        }
    }

    if (repo == NULL || remote == NULL) {
        fprintf(stderr, "usage: tarpack upload --repo <repodir> --remote <rclone-remote-path>\n");
        return 64;
    }

    return tp_upload(repo, remote);
}

int main(int argc, char **argv) {
    if (argc < 2) {
        print_usage();
        return 64;
    }

    const char *arg = argv[1];

    if (strcmp(arg, "--version") == 0) {
        printf("tarpack 0.0.1\n");
        return 0;
    }

    if (strcmp(arg, "scan") == 0) {
        return cmd_scan(argc, argv);
    }
    if (strcmp(arg, "pack") == 0) {
        return cmd_pack(argc, argv);
    }
    if (strcmp(arg, "verify") == 0) {
        return cmd_verify(argc, argv);
    }
    if (strcmp(arg, "restore") == 0) {
        return cmd_not_implemented("restore");
    }
    if (strcmp(arg, "upload") == 0) {
        return cmd_upload(argc, argv);
    }

    print_usage();
    return 64;
}
