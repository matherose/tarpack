#ifndef TARPACK_UTIL_H
#define TARPACK_UTIL_H

#include <stddef.h>
#include <stdint.h>

/*
 * Minimal error/logging and safe-open helpers for tarpack.
 * Kept intentionally small; later tasks are expected to extend this.
 */

/*
 * tp_warn: print "tarpack: " + formatted message to stderr.
 * If errno is non-zero at call time, appends ": " + strerror(errno).
 * Trailing newline is added automatically.
 */
void tp_warn(const char *fmt, ...);

/*
 * tp_warnx: same as tp_warn but never appends strerror() output.
 */
void tp_warnx(const char *fmt, ...);

/*
 * tp_verbose: global verbosity flag, 0 by default. Set to 1 by the CLI when
 * -v/--verbose is given.
 */
extern int tp_verbose;

/*
 * tp_verbosex: same as tp_warnx, but a no-op unless tp_verbose is set.
 */
void tp_verbosex(const char *fmt, ...);

/*
 * safe_openat: openat() wrapper that always adds O_NOFOLLOW | O_CLOEXEC
 * to the caller-supplied flags. Returns the fd on success, or -1 on
 * failure (errno set by openat()).
 */
int safe_openat(int dirfd, const char *name, int flags);

/*
 * tp_utf8_valid: returns 1 if the n bytes at s form a valid, strict UTF-8
 * sequence, 0 otherwise. Rejects overlong encodings, surrogate code points
 * (U+D800..U+DFFF), and code points beyond U+10FFFF. An empty buffer (n==0)
 * is considered valid.
 */
int tp_utf8_valid(const uint8_t *s, size_t n);

/*
 * tp_base64_encode: RFC 4648 standard base64 encoding (with '+', '/' and
 * '=' padding) of the n bytes at in. Returns a malloc'd, NUL-terminated
 * string that the caller must free(), or NULL on allocation failure.
 * Encoding an empty buffer (n==0) returns a malloc'd empty string.
 */
char *tp_base64_encode(const uint8_t *in, size_t n);

#endif /* TARPACK_UTIL_H */
