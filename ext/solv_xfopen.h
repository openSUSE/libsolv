/*
 * Copyright (c) 2009-2012, Novell Inc.
 *
 * This program is licensed under the BSD license, read LICENSE.BSD
 * for further information
 */

#ifndef SOLV_XFOPEN_H
#define SOLV_XFOPEN_H

extern FILE *solv_xfopen(const char *fn, const char *mode);
extern FILE *solv_xfopen_fd(const char *fn, int fd, const char *mode);

#endif
