/*
 * Copyright (c) 2007, Novell Inc.
 *
 * This program is licensed under the BSD license, read LICENSE.BSD
 * for further information
 */

#define _GNU_SOURCE

#include <stdio.h>
#include <stdlib.h>
#include <unistd.h>
#include <string.h>
#include <sys/time.h>

#include "util.h"

void
sat_oom(size_t num, size_t len)
{
  if (num)
    fprintf(stderr, "Out of memory allocating %zu*%zu bytes!\n", num, len);
  else
    fprintf(stderr, "Out of memory allocating %zu bytes!\n", len);
  abort();
  exit(1);
}

void *
sat_malloc(size_t len)
{
  void *r = malloc(len ? len : 1);
  if (!r)
    sat_oom(0, len);
  return r;
}

void *
sat_malloc2(size_t num, size_t len)
{
  if (len && (num * len) / len != num)
    sat_oom(num, len);
  return sat_malloc(num * len);
}

void *
sat_realloc(void *old, size_t len)
{
  if (old == 0)
    old = malloc(len ? len : 1);
  else
    old = realloc(old, len ? len : 1);
  if (!old)
    sat_oom(0, len);
  return old;
}

void *
sat_realloc2(void *old, size_t num, size_t len)
{
  if (len && (num * len) / len != num)
    sat_oom(num, len);
  return sat_realloc(old, num * len);
}

void *
sat_calloc(size_t num, size_t len)
{
  void *r;
  if (num == 0 || len == 0)
    r = malloc(1);
  else
    r = calloc(num, len);
  if (!r)
    sat_oom(num, len);
  return r;
}

void *
sat_free(void *mem)
{
  if (mem)
    free(mem);
  return 0;
}

unsigned int
sat_timems(unsigned int subtract)
{
  struct timeval tv;
  unsigned int r;

  if (gettimeofday(&tv, 0))
    return 0;
  r = (((unsigned int)tv.tv_sec >> 16) * 1000) << 16;
  r += ((unsigned int)tv.tv_sec & 0xffff) * 1000;
  r += (unsigned int)tv.tv_usec / 1000;
  return r - subtract;
}

/* bsd's qsort_r has different arguments, so we define our
   own version in case we need to do some clever mapping */
void
sat_sort(void *base, size_t nmemb, size_t size, int (*compar)(const void *, const void *, void *), void *compard)
{
  qsort_r(base, nmemb, size, compar, compard);
}
