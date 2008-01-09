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

extern void *sat_malloc(size_t);
extern void *sat_malloc2(size_t, size_t);
extern void *sat_calloc(size_t, size_t);
extern void *sat_realloc(void *, size_t);
extern void *sat_realloc2(void *, size_t, size_t);
extern void *sat_free(void *);

#endif /* SATSOLVER_UTIL_H */
