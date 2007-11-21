/*
 * Copyright (c) 2007, Novell Inc.
 *
 * This program is licensed under the BSD license, read LICENSE.BSD
 * for further information
 */

/*
 * util.h
 * 
 */

#ifndef SATSOLVER_UTIL_H
#define SATSOLVER_UTIL_H

extern void *xmalloc(size_t);
extern void *xmalloc2(size_t, size_t);
extern void *xcalloc(size_t, size_t);
extern void *xrealloc(void *, size_t);
extern void *xrealloc2(void *, size_t, size_t);
extern void *xfree(void *);

#endif /* SATSOLVER_UTIL_H */
