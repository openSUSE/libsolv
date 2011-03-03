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

#include <stddef.h>
#include <string.h>

/**
 * malloc
 * exits with error message on error
 */
extern void *sat_malloc(size_t);
extern void *sat_malloc2(size_t, size_t);
extern void *sat_calloc(size_t, size_t);
extern void *sat_realloc(void *, size_t);
extern void *sat_realloc2(void *, size_t, size_t);
extern void *sat_free(void *);
extern void sat_oom(size_t, size_t);
extern unsigned int sat_timems(unsigned int subtract);
extern void sat_sort(void *base, size_t nmemb, size_t size, int (*compar)(const void *, const void *, void *), void *compard);
extern char *sat_dupjoin(const char *str1, const char *str2, const char *str3);
extern char *sat_dupappend(const char *str1, const char *str2, const char *str3);
extern int sat_hex2bin(const char **strp, unsigned char *buf, int bufl);
extern char *sat_bin2hex(const unsigned char *buf, int l, char *str);


static inline void *sat_extend(void *buf, size_t len, size_t nmemb, size_t size, size_t block)
{
  if (nmemb == 1)
    {
      if ((len & block) == 0)
	buf = sat_realloc2(buf, len + (1 + block), size);
    }
  else
    {
      if (((len - 1) | block) != ((len + nmemb - 1) | block))
	buf = sat_realloc2(buf, (len + (nmemb + block)) & ~block, size);
    }
  return buf;
}

/**
 * extend an array by reallocation and zero's the new section
 * buf old pointer
 * len current size
 * nmbemb number of elements to add
 * size size of each element
 * block block size used to allocate the elements
 */
static inline void *sat_zextend(void *buf, size_t len, size_t nmemb, size_t size, size_t block)
{
  buf = sat_extend(buf, len, nmemb, size, block);
  memset((char *)buf + len * size, 0, nmemb * size);
  return buf;
}

static inline void *sat_extend_resize(void *buf, size_t len, size_t size, size_t block)
{
  if (len)
    buf = sat_realloc2(buf, (len + block) & ~block, size);
  return buf;
}

static inline void *sat_calloc_block(size_t len, size_t size, size_t block)
{
  void *buf;
  if (!len)
    return 0;
  buf = sat_malloc2((len + block) & ~block, size);
  memset(buf, 0, ((len + block) & ~block) * size);
  return buf;
}
#endif /* SATSOLVER_UTIL_H */
