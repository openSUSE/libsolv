#include <stdio.h>
#include <stdlib.h>
#include <unistd.h>
#include <string.h>

#include "util.h"

void *
xmalloc(size_t len)
{
  void *r = malloc(len ? len : 1);
  if (r)
    return r;
  fprintf(stderr, "Out of memory allocating %zu bytes!\n", len);
  exit(1);
}

void *
xmalloc2(size_t num, size_t len)
{
  if (len && (num * len) / len != num)
    {
      fprintf(stderr, "Out of memory allocating %zu*%zu bytes!\n", num, len);
      exit(1);
    }
  return xmalloc(num * len);
}

void *
xrealloc(void *old, size_t len)
{
  if (old == 0)
    old = malloc(len ? len : 1);
  else
    old = realloc(old, len ? len : 1);
  if (old)
    return old;
  fprintf(stderr, "Out of memory reallocating %zu bytes!\n", len);
  exit(1);
}

void *
xrealloc2(void *old, size_t num, size_t len)
{
  if (len && (num * len) / len != num)
    {
      fprintf(stderr, "Out of memory allocating %zu*%zu bytes!\n", num, len);
      exit(1);
    }
  return xrealloc(old, num * len);
}

void *
xcalloc(size_t num, size_t len)
{
  void *r;
  if (num == 0 || len == 0)
    r = malloc(1);
  else
    r = calloc(num, len);
  if (r)
    return r;
  fprintf(stderr, "Out of memory allocating %zu bytes!\n", num * len);
  exit(1);
}

void *
xfree(void *mem)
{
  if (mem)
    free(mem);
  return 0;
}

