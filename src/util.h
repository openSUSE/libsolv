/*
 * util.h
 * 
 */

#ifndef UTIL_H
#define UTIL_H

extern void *xmalloc(size_t);
extern void *xmalloc2(size_t, size_t);
extern void *xcalloc(size_t, size_t);
extern void *xrealloc(void *, size_t);
extern void *xrealloc2(void *, size_t, size_t);
extern void *xfree(void *);

#endif /* UTIL_H */
