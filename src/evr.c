/*
 * Copyright (c) 2007, Novell Inc.
 *
 * This program is licensed under the BSD license, read LICENSE.BSD
 * for further information
 */

/*
 * evr.c
 *
 * version compare
 */

#include <stdio.h>
#include <string.h>
#include "evr.h"
#include "pool.h"

int
vercmp(const char *s1, const char *q1, const char *s2, const char *q2)
{
  int r = 0;
  const char *e1, *e2;

  while (s1 < q1 && s2 < q2)
    {
      while (s1 < q1 && !(*s1 >= '0' && *s1 <= '9') &&
          !(*s1 >= 'a' && *s1 <= 'z') && !(*s1 >= 'A' && *s1 <= 'Z'))
	s1++;
      while (s2 < q2 && !(*s2 >= '0' && *s2 <= '9') &&
          !(*s2 >= 'a' && *s2 <= 'z') && !(*s2 >= 'A' && *s2 <= 'Z'))
	s2++;
      if ((*s1 >= '0' && *s1 <= '9') || (*s2 >= '0' && *s2 <= '9'))
	{
	  while (*s1 == '0' && s1[1] >= '0' && s1[1] <= '9')
	    s1++;
	  while (*s2 == '0' && s2[1] >= '0' && s2[1] <= '9')
	    s2++;
	  for (e1 = s1; *e1 >= '0' && *e1 <= '9'; )
	    e1++;
	  for (e2 = s2; *e2 >= '0' && *e2 <= '9'; )
	    e2++;
	  r = e1 - s1 - (e2 - s2);
          if (!r)
	    r = strncmp(s1, s2, e1 - s1);
          if (r)
	    return r > 0 ? 1 : -1;
	}
      else
	{
	  for (e1 = s1; (*e1 >= 'a' && *e1 <= 'z') || (*e1 >= 'A' && *e1 <= 'Z'); )
	    e1++;
	  for (e2 = s2; (*e2 >= 'a' && *e2 <= 'z') || (*e2 >= 'A' && *e2 <= 'Z'); )
	    e2++;
	  r = e1 - s1 - (e2 - s2);
          if (r > 0)
	    {
	      r = strncmp(s1, s2, e2 - s2);
	      return r >= 0 ? 1 : -1;
	    }
          if (r < 0)
	    {
	      r = strncmp(s1, s2, e1 - s1);
	      return r <= 0 ? -1 : 1;
	    }
	  r = strncmp(s1, s2, e1 - s1);
	  if (r)
	    return r > 0 ? 1 : -1;
	}
      s1 = e1;
      s2 = e2;
    }
  return s1 < q1 ? 1 : s2 < q2 ? -1 : 0;
}


// edition (e:v-r) compare
int
evrcmp(Pool *pool, Id evr1id, Id evr2id)
{
  int r;
  const char *evr1, *evr2;
  const char *s1, *s2;
  const char *r1, *r2;

  if (evr1id == evr2id)
    return 0;
  evr1 = id2str(pool, evr1id);
  evr2 = id2str(pool, evr2id);

#if 0
  POOL_DEBUG(DEBUG_EVRCMP, "evrcmp %s %s\n", evr1, evr2);
#endif
  for (s1 = evr1; *s1 >= '0' && *s1 <= '9'; s1++)
    ;
  for (s2 = evr2; *s2 >= '0' && *s2 <= '9'; s2++)
    ;
  if (s1 == evr1 || *s1 != ':')
    s1 = 0;
  if (s2 == evr2 || *s2 != ':')
    s2 = 0;
  if (s1 && s2)
    {
      r = vercmp(evr1, s1, evr2, s2);
      if (r)
	return r;
      evr1 = s1 + 1;
      evr2 = s2 + 1;
    }
  else if (s1)
    {
      if (!pool->promoteepoch)
	{
	  while (*evr1 == '0')
	    evr1++;
	  if (*evr1 != ':')
	    return 1;
	}
      evr1 = s1 + 1;
    }
  else if (s2)
    {
      while (*evr2 == '0')
	evr2++;
      if (*evr2 != ':')
	return -1;
      evr2 = s2 + 1;
    }
  for (s1 = evr1, r1 = 0; *s1; s1++)
    if (*s1 == '-')
      r1 = s1;
  for (s2 = evr2, r2 = 0; *s2; s2++)
    if (*s2 == '-')
      r2 = s2;
  r = vercmp(evr1, r1 ? r1 : s1, evr2, r2 ? r2 : s2);
  if (r)
    return r;
  if (r1 && r2)
    {
      if (s1 != ++r1 && s2 != ++r2)
        r = vercmp(r1, s1, r2, s2);
    }
  return r;
}

// EOF
