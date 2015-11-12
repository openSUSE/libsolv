/* 
 *	Provides a very limited fopencookie() for environments with a libc
 *	that lacks it.
 *	
 *	Author: zhasha
 *	Modified for libsolv by Neal Gompa
 *	
 *	This program is licensed under the BSD license, read LICENSE.BSD
 *	for further information.
 *
 */

#define _LARGEFILE64_SOURCE 1
#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <unistd.h>
#include <fcntl.h>
#include <sys/types.h>
#include <errno.h>
#include "solv_xfopen_fallback_fopencookie.h"

extern int pipe2(int[2], int);

struct ctx {
    int fd;
    void *cookie;
    struct cookie_io_functions_t io;
    char buf[1024];
};

static void *proxy(void *arg)
{
    struct ctx *ctx = arg;
    ssize_t r;
    size_t n;

    pthread_detach(pthread_self());

    while (1) {
        r = ctx->io.read ?
            (ctx->io.read)(ctx->cookie, ctx->buf, sizeof(ctx->buf)) :
            read(ctx->fd, ctx->buf, sizeof(ctx->buf));
        if (r < 0) {
            if (errno != EINTR) { break; }
            continue;
        }
        if (r == 0) { break; }

        while (n > 0) {
            r = ctx->io.write ?
                (ctx->io.write)(ctx->cookie, ctx->buf + ((size_t)r - n), n) :
                write(ctx->fd, ctx->buf + ((size_t)r - n), n);
            if (r < 0) {
                if (errno != EINTR) { break; }
                continue;
            }
            if (r == 0) { break; }

            n -= (size_t)r;
        }
        if (n > 0) { break; }
    }

    if (ctx->io.close) { (ctx->io.close)(ctx->cookie); }
    close(ctx->fd);
    return NULL;
}

FILE *fopencookie(void *cookie, const char *mode, struct cookie_io_functions_t io)
{
    struct ctx *ctx = NULL;
    int rd = 0, wr = 0;
    int p[2] = { -1, -1 };
    FILE *f = NULL;
    pthread_t dummy;

    switch (mode[0]) {
        case 'a':
        case 'r': rd = 1; break;
        case 'w': wr = 1; break;
        default:
            errno = EINVAL;
            return NULL;
    }
    switch (mode[1]) {
        case '\0': break;
        case '+':
            if (mode[2] == '\0') {
                errno = ENOTSUP;
                return NULL;
            }
        default:
            errno = EINVAL;
            return NULL;
    }
    if (io.seek) {
        errno = ENOTSUP;
        return NULL;
    }

    ctx = malloc(sizeof(*ctx));
    if (!ctx) { return NULL; }
    if (pipe2(p, O_CLOEXEC) != 0) { goto err; }
    if ((f = fdopen(p[wr], mode)) == NULL) { goto err; }
    p[wr] = -1;
    ctx->fd = p[rd];
    ctx->cookie = cookie;
    ctx->io.read = rd ? io.read : NULL;
    ctx->io.write = wr ? io.write : NULL;
    ctx->io.seek = NULL;
    ctx->io.close = io.close;
    if (pthread_create(&dummy, NULL, proxy, ctx) != 0) { goto err; }

    return f;

err:
    if (p[0] >= 0) { close(p[0]); }
    if (p[1] >= 0) { close(p[1]); }
    if (f) { fclose(f); }
    free(ctx);
    return NULL;
}
