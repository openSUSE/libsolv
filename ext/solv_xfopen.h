/*
 * Copyright (c) 2009-2012, Novell Inc.
 *
 * This program is licensed under the BSD license, read LICENSE.BSD
 * for further information
 */

#ifndef SOLV_XFOPEN_H
#define SOLV_XFOPEN_H

#include <stddef.h>

#ifdef _MSC_VER
  #include <BaseTsd.h>
  typedef SSIZE_T ssize_t;
#else
  #include <unistd.h>
#endif

extern FILE *solv_xfopen(const char *fn, const char *mode);
extern FILE *solv_xfopen_fd(const char *fn, int fd, const char *mode);
extern FILE *solv_xfopen_buf(const char *fn, char **bufp, size_t *buflp, const char *mode);
extern int   solv_xfopen_iscompressed(const char *fn);
extern FILE *solv_fmemopen(const char *buf, size_t bufl, const char *mode);

FILE *solv_cookieopen(void *cookie, const char *mode, ssize_t (*cread)(void *, char *, size_t), ssize_t (*cwrite)(void *, const char *, size_t), int (*cclose)(void *));	/* internal */

#endif
