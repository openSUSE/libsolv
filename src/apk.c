/*
 * Copyright (c) 2024, SUSE LLC
 *
 * This program is licensed under the BSD license, read LICENSE.BSD
 * for further information
 */

/*
 * apk.c
 *
 * evr comparison for apk
 */

#include <stdio.h>
#include <stdlib.h>
#include <stdarg.h>
#include <unistd.h>
#include <string.h>
#include <sys/types.h>

#include "pool.h"
#include "repo.h"
#include "util.h"
#include "apk.h"

static const char suffixlist[] = "\005alpha\004beta\003pre\002rc\003cvs\003svn\003git\002hg\001p";
static const char classorder[] = ".X_~-$!";

static inline int
suffixclass(const char *evr, size_t l)
{
  const char *sp = suffixlist;
  int i;
  for (i = 1; *sp; sp += *sp + 1, i++)
    if (l == *sp && !strncmp(sp + 1, evr, l))
      return i;
  return 0;
}

static inline int
is_release_suffix(const char *p, const char *pe)
{
  int cl;
  size_t l = 0;
  while (p < pe && *p >= 'a' && *p <= 'z')
    p++, l++;
  cl = suffixclass(p - l, l);
  return cl && cl < 5 ? 1 : 0;
}

static int
classify_part(int initial, const char *evr, const char *evre, const char **part, const char **parte)
{
  int c;
  *part = *parte = evr;
  if (evr >= evre)
    return '$';
  c = *evr++;
  if (c >= 'a' && c <= 'z')
    {
      *parte = evr;
      return 'X';
    }
  if (initial && c >= '0' && c <= '9')
    {
      c = '.';
      evr--;
    }
  if (evr >= evre)
    return '!';
  *part = evr;
  if (c == '.' && *evr >= '0' && *evr <= '9')
    ;
  else if (c == '_' && *evr >= 'a' && *evr <= 'z')
    {
      while (evr < evre && *evr >= 'a' && *evr <= 'z')
	evr++;
    }
  else if (c == '-' && *evr == 'r' && evr + 1 < evre && (evr[1] >= '0' && evr[2] <= '9'))
    evr++;
  else if (c == '~' && ((*evr >= '0' && *evr <= '9') || (*evr >= 'a' && *evr <= 'f')))
    {
      while (evr < evre && ((*evr >= '0' && *evr <= '9') || (*evr >= 'a' && *evr <= 'f')))
	evr++;
      *parte = evr;
      return c;
    }
  else
    {
      *part = *parte;
      return '!';
    }
  while (evr < evre && *evr >= '0' && *evr <= '9')
    evr++;
  *parte = evr;
  return c;
}

int
solv_vercmp_apk(const char *evr1, const char *evr1e, const char *evr2, const char *evr2e)
{
  const char *p1, *p1e, *p2, *p2e;
  int c1, c2, initial, r;
  int fuzzy1 = 0, fuzzy2 = 0;

  if (evr1 < evr1e && *evr1 == '~')
    {
      fuzzy1 = 1;
      evr1++;
    }
  if (evr2 < evr2e && *evr2 == '~')
    {
      fuzzy2 = 1;
      evr2++;
    }
  for (initial = 1;; initial = 0)
    {
      c1 = classify_part(initial, evr1, evr1e, &p1, &p1e);
      c2 = classify_part(initial, evr2, evr2e, &p2, &p2e);
#if 0
      printf("C1: %c >%.*s<\n", c1, (int)(p1e - p1), p1);
      printf("C2: %c >%.*s<\n", c2, (int)(p2e - p2), p2);
#endif
      if (c1 != c2 || c1 == '!' || c1 == '$')
	break;
      evr1 = p1e;
      evr2 = p2e;
      if (p1e - p1 == p2e - p2 && !strncmp(p1, p2, p1e - p1))
	continue;
      if (c1 == '-')
	{
	  if (p1 < p1e && *p1 == 'r')
	    p1++;
	  if (p2 < p2e && *p2 == 'r')
	    p2++;
	}
      else if (c1 == '_')
	{
	  size_t l1 = 0, l2 = 0;
	  while (p1 < p1e && *p1 >= 'a' && *p1 <= 'z')
	    p1++, l1++;
	  while (p2 < p2e && *p2 >= 'a' && *p2 <= 'z')
	    p2++, l2++;
	  c1 = suffixclass(p1 - l1, l1);
	  c2 = suffixclass(p2 - l2, l2);
	  if (c1 != c2)
	    return c1 < c2 ? -1 : 1;
	  c1 = '_';
	}
      if ((c1 == '.' && (initial || (*p1 != '0' && *p2 != '0'))) || c1 == '_' || c1 == '-')
	{
	  while (p1 < p1e && *p1 == '0')
	    p1++;
	  while (p2 < p2e && *p2 == '0')
	    p2++;
          if (p1e - p1 != p2e - p2)
	    return p1e - p1 < p2e - p2 ? -1 : 1;
	}
      r = strncmp(p1, p2, p1e - p1 > p2e - p2 ? p2e - p2 : p1e - p1);
      if (r)
	return r < 0 ? -1 : 1;
      if (p1e - p1 != p2e - p2)
	return p1e - p1 < p2e - p2 ? -1 : 1;
    }
  if (c1 == c2)
    return 0;
  if ((fuzzy1 && c1 == '$') || (fuzzy2 && c2 == '$'))
    return 0;
  if (c1 == '_' && is_release_suffix(p1, p1e))
   return -1;
  if (c2 == '_' && is_release_suffix(p2, p2e))
   return 1;
  /* handle most likely cases first */
  if (c1 == '.' || c2 == '!')
    return 1;
  if (c2 == '.' || c1 == '!' || c1 == '$')
    return -1;
  if (c2 == '$')
    return 1;
  p1 = strchr(classorder, c1);
  p2 = strchr(classorder, c2);
  if (p1 && p2 && p1 != p2)
    return p1 > p2 ? -1 : 1;
  return 0;
}

int
pool_evrcmp_apk(const Pool *pool, const char *evr1, const char *evr2, int mode)
{
  if (evr1 == evr2)
    return 0;
  return solv_vercmp_apk(evr1, evr1 + strlen(evr1), evr2, evr2 + strlen(evr2));
}

#if 0
int
main(int argc, char **argv)
{
  char *p1 = argv[1];
  char *p2 = argv[2];
  int r = solv_vercmp_apk(p1, p1 + strlen(p1), p2, p2 + strlen(p2));
  printf("-> %d\n", r);
  return 0;
}
#endif
