#include "util.h"

#include <errno.h>
#include <fcntl.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

void tp_warn(const char *fmt, ...) {
    int saved_errno = errno;
    va_list ap;

    fputs("tarpack: ", stderr);

    va_start(ap, fmt);
    vfprintf(stderr, fmt, ap);
    va_end(ap);

    if (saved_errno != 0) {
        fprintf(stderr, ": %s", strerror(saved_errno));
    }

    fputc('\n', stderr);
    errno = saved_errno;
}

void tp_warnx(const char *fmt, ...) {
    va_list ap;

    fputs("tarpack: ", stderr);

    va_start(ap, fmt);
    vfprintf(stderr, fmt, ap);
    va_end(ap);

    fputc('\n', stderr);
}

int safe_openat(int dirfd, const char *name, int flags) {
    return openat(dirfd, name, flags | O_NOFOLLOW | O_CLOEXEC);
}

int tp_utf8_valid(const uint8_t *s, size_t n) {
    size_t i = 0;

    while (i < n) {
        uint8_t b0 = s[i];
        size_t extra;
        uint32_t cp;
        uint32_t min_cp;

        if (b0 <= 0x7F) {
            i += 1;
            continue;
        } else if ((b0 & 0xE0) == 0xC0) {
            extra = 1;
            cp = (uint32_t)(b0 & 0x1F);
            min_cp = 0x80;
        } else if ((b0 & 0xF0) == 0xE0) {
            extra = 2;
            cp = (uint32_t)(b0 & 0x0F);
            min_cp = 0x800;
        } else if ((b0 & 0xF8) == 0xF0) {
            extra = 3;
            cp = (uint32_t)(b0 & 0x07);
            min_cp = 0x10000;
        } else {
            return 0; /* stray continuation byte or invalid leading byte (e.g. 0xFF) */
        }

        if (i + extra >= n) {
            return 0; /* not enough bytes remaining for this sequence */
        }

        for (size_t k = 1; k <= extra; k++) {
            uint8_t bk = s[i + k];
            if ((bk & 0xC0) != 0x80) {
                return 0;
            }
            cp = (cp << 6) | (uint32_t)(bk & 0x3F);
        }

        if (cp < min_cp) {
            return 0; /* overlong encoding */
        }
        if (cp >= 0xD800 && cp <= 0xDFFF) {
            return 0; /* surrogate code point */
        }
        if (cp > 0x10FFFF) {
            return 0; /* beyond Unicode range */
        }

        i += extra + 1;
    }

    return 1;
}

char *tp_base64_encode(const uint8_t *in, size_t n) {
    static const char table[] =
        "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";

    size_t out_len = ((n + 2) / 3) * 4;
    char *out = malloc(out_len + 1);
    if (out == NULL) {
        return NULL;
    }

    size_t oi = 0;
    size_t i = 0;
    while (i + 3 <= n) {
        uint32_t v = ((uint32_t)in[i] << 16) | ((uint32_t)in[i + 1] << 8) | (uint32_t)in[i + 2];
        out[oi++] = table[(v >> 18) & 0x3F];
        out[oi++] = table[(v >> 12) & 0x3F];
        out[oi++] = table[(v >> 6) & 0x3F];
        out[oi++] = table[v & 0x3F];
        i += 3;
    }

    size_t remaining = n - i;
    if (remaining == 1) {
        uint32_t v = (uint32_t)in[i] << 16;
        out[oi++] = table[(v >> 18) & 0x3F];
        out[oi++] = table[(v >> 12) & 0x3F];
        out[oi++] = '=';
        out[oi++] = '=';
    } else if (remaining == 2) {
        uint32_t v = ((uint32_t)in[i] << 16) | ((uint32_t)in[i + 1] << 8);
        out[oi++] = table[(v >> 18) & 0x3F];
        out[oi++] = table[(v >> 12) & 0x3F];
        out[oi++] = table[(v >> 6) & 0x3F];
        out[oi++] = '=';
    }

    out[oi] = '\0';
    return out;
}
