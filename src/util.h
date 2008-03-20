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

extern void *sat_malloc(size_t);
extern void *sat_malloc2(size_t, size_t);
extern void *sat_calloc(size_t, size_t);
extern void *sat_realloc(void *, size_t);
extern void *sat_realloc2(void *, size_t, size_t);
extern void *sat_free(void *);
extern void sat_oom(size_t, size_t);

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
