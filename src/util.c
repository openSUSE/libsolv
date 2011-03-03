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

#ifdef USE_OWN_QSORT
#include "qsort_r.c"
#else

/* bsd's qsort_r has different arguments, so we define our
   own version in case we need to do some clever mapping
 
   see also: http://sources.redhat.com/ml/libc-alpha/2008-12/msg00003.html
 */
#if defined(__GLIBC__)

void
sat_sort(void *base, size_t nmemb, size_t size, int (*compar)(const void *, const void *, void *), void *compard)
{
# if __GLIBC_PREREQ(2, 8)
  qsort_r(base, nmemb, size, compar, compard);
# else
  /* backported for SLE10-SP2 */
  __qsort_r(base, nmemb, size, compar, compard);
# endif
}

#else

struct sat_sort_data {
  int (*compar)(const void *, const void *, void *);
  void *compard;
};

static int
sat_sort_helper(void *compard, const void *a, const void *b)
{
  struct sat_sort_data *d = compard;
  return (*d->compar)(a, b, d->compard);
}

void
sat_sort(void *base, size_t nmemb, size_t size, int (*compar)(const void *, const void *, void *), void *compard)
{
  struct sat_sort_data d;
  d.compar = compar;
  d.compard = compard;
  qsort_r(base, nmemb, size, &d, sat_sort_helper);
}

#endif

#endif	/* USE_OWN_QSORT */

char *
sat_dupjoin(const char *str1, const char *str2, const char *str3)
{
  int l1, l2, l3;
  char *s, *str;
  l1 = str1 ? strlen(str1) : 0;
  l2 = str2 ? strlen(str2) : 0;
  l3 = str3 ? strlen(str3) : 0;
  s = str = sat_malloc(l1 + l2 + l3 + 1);
  if (l1)
    {
      strcpy(s, str1);
      s += l1;
    }
  if (l2)
    {
      strcpy(s, str2);
      s += l2;
    }
  if (l3)
    {
      strcpy(s, str3);
      s += l3;
    }
  *s = 0;
  return str;
}

char *
sat_dupappend(const char *str1, const char *str2, const char *str3)
{
  char *str = sat_dupjoin(str1, str2, str3);
  sat_free((void *)str1);
  return str;
}

int
sat_hex2bin(const char **strp, unsigned char *buf, int bufl)
{
  const char *str = *strp;
  int i;

  for (i = 0; i < bufl; i++)
    {
      int c = *str;
      int d;
      if (c >= '0' && c <= '9')
        d = c - '0';
      else if (c >= 'a' && c <= 'f')
        d = c - ('a' - 10);
      else if (c >= 'A' && c <= 'F')
        d = c - ('A' - 10);
      else
	break;
      c = *++str;
      d <<= 4;
      if (c >= '0' && c <= '9')
        d |= c - '0';
      else if (c >= 'a' && c <= 'f')
        d |= c - ('a' - 10);
      else if (c >= 'A' && c <= 'F')
        d |= c - ('A' - 10);
      else
	break;
      buf[i] = d;
      ++str;
    }
  *strp = str;
  return i;
}

char *
sat_bin2hex(const unsigned char *buf, int l, char *str)
{
  int i;
  for (i = 0; i < l; i++, buf++)
    {
      int c = *buf >> 4;
      *str++ = c < 10 ? c + '0' : c + ('a' - 10);
      c = *buf & 15;
      *str++ = c < 10 ? c + '0' : c + ('a' - 10);
    }
  *str = 0;
  return str;
}


