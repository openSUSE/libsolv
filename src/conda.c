/*
 * Copyright (c) 2019, SUSE LLC
 *
 * This program is licensed under the BSD license, read LICENSE.BSD
 * for further information
 */

/*
 * conda.c
 *
 * evr comparison and package matching for conda
 */

#include <stdio.h>
#include <stdlib.h>
#include <stdarg.h>
#include <unistd.h>
#include <string.h>

#include "pool.h"
#include "conda.h"

static const char *
endseg(const char *seg, const char *end)
{
  for (; seg < end; seg++)
    if (*seg == '.' || *seg == '-' || *seg == '_')
      break;
  return seg;
}

static const char *
endpart(const char *seg, const char *end)
{
  if (seg == end)
    return seg;
  if (*seg >= '0' && *seg <= '9')
    {
      for (seg++; seg < end; seg++)
        if (!(*seg >= '0' && *seg <= '9'))
	  break;
    }
  else if (*seg == '*')
    {
      for (seg++; seg < end; seg++)
	if (*seg != '*')
	  break;
    }
  else
    {
      for (seg++; seg < end; seg++)
        if ((*seg >= '0' && *seg <= '9') || *seg == '*')
	  break;
    }
  return seg;
}

/* C implementation of the version comparison code in conda/models/version.py */
static int
solv_vercmp_conda(const char *s1, const char *q1, const char *s2, const char *q2)
{
  const char *s1p, *s2p;
  const char *s1e, *s2e;
  int r, isfirst;

  for (;;)
    {
      while (s1 < q1 && (*s1 == '.' || *s1 == '-' || *s1 == '_'))
	s1++;
      while (s2 < q2 && (*s2 == '.' || *s2 == '-' || *s2 == '_'))
	s2++;
      if (s1 == q1 && s2 == q2)
	return 0;
      /* find end of component */
      s1e = endseg(s1, q1);
      s2e = endseg(s2, q2);

      for (isfirst = 1; ; isfirst = 0)
	{
	  if (s1 == s1e && s2 == s2e)
	    break;
          s1p = endpart(s1, s1e);
          s2p = endpart(s2, s2e);
	  /* prepend 0 if not numeric */
	  if (isfirst)
	    {
	      if (s1p != s1 && !(*s1 >= '0' && *s1 <= '9'))
		s1p = s1;
	      if (s2p != s2 && !(*s2 >= '0' && *s2 <= '9'))
		s2p = s2;
	    }
	  /* special case "post" */
	  if (s1p - s1 == 4 && !strncasecmp(s1, "post", 4))
	    {
	      if (s2p - s2 == 4 && !strncasecmp(s2, "post", 4))
		{
		  s1 = s1p;
		  s2 = s2p;
		  continue;
		}
	      return 1;
	    }
	  if (s2p - s2 == 4 && !strncasecmp(s2, "post", 4))
	    return -1;

	  if (isfirst || ((s1 == s1p || (*s1 >= '0' && *s1 <= '9')) && (s2 == s2p || (*s2 >= '0' && *s2 <= '9'))))
	    {
	      /* compare numbers */
	      while (s1 < s1p && *s1 == '0')
		s1++;
	      while (s2 < s2p && *s2 == '0')
		s2++;
	      if (s1p - s1 < s2p - s2)
		return -1;
	      if (s1p - s1 > s2p - s2)
		return 1;
	      r = s1p - s1 ? strncmp(s1, s2, s1p - s1) : 0;
	      if (r)
		return r;
	    }
	  else if (s1 == s1p || (*s1 >= '0' && *s1 <= '9'))
	    return 1;
	  else if (s2 == s2p || (*s2 >= '0' && *s2 <= '9'))
	    return -1;
	  else
	    {
	      /* special case "dev" */
	      if (*s2 != '*' && s1p - s1 == 3 && !strncasecmp(s1, "dev", 3))
		{
		  if (s2p - s2 == 3 && !strncasecmp(s2, "dev", 3))
		    {
		      s1 = s1p;
		      s2 = s2p;
		      continue;
		    }
		  return -1;
		}
	      if (*s1 != '*' && s2p - s2 == 3 && !strncasecmp(s2, "dev", 3))
		return 1;
	      /* compare strings */
	      r = s2p - s2 > s1p - s1 ? s1p - s1 : s2p - s2;
	      if (r)
	        r = strncasecmp(s1, s2, r);
	      if (r)
		return r;
              if (s1p - s1 < s2p - s2) 
                return -1; 
              if (s1p - s1 > s2p - s2) 
                return 1;
	    }
	  s1 = s1p;
	  s2 = s2p;
	}
    }
}

int
pool_evrcmp_conda(const Pool *pool, const char *evr1, const char *evr2, int mode)
{
  static char zero[2] = {'0', 0};
  int r;
  const char *s1, *s2;
  const char *r1, *r2;

  if (evr1 == evr2)
    return 0;

  /* split and compare epoch */
  for (s1 = evr1; *s1 >= '0' && *s1 <= '9'; s1++)
    ;
  for (s2 = evr2; *s2 >= '0' && *s2 <= '9'; s2++)
    ;
  if (s1 == evr1 || *s1 != '!')
    s1 = 0;
  if (s2 == evr1 || *s2 != '!')
    s2 = 0;
  if (s1 || s2)
    {
      r = solv_vercmp_conda(s1 ? evr1 : zero, s1 ? s1 : zero + 1, 
                            s2 ? evr2 : zero, s2 ? s2 : zero + 1);
      if (r)
	return r;
      if (s1)
        evr1 = s1 + 1;
      if (s2)
        evr2 = s2 + 1;
    }
  /* split into version/localversion */
  for (s1 = evr1, r1 = 0; *s1; s1++)
    if (*s1 == '+')
      r1 = s1;
  for (s2 = evr2, r2 = 0; *s2; s2++)
    if (*s2 == '+')
      r2 = s2;
  r = solv_vercmp_conda(evr1, r1 ? r1 : s1, evr2, r2 ? r2 : s2);
  if (r)
    return r;
  if (!r1 && !r2)
    return 0;
  if (!r1 && r2)
    return -1;
  if (r1 && !r2)
    return 1;
  return solv_vercmp_conda(r1 + 1, s1, r2 + 1, s2);
}

#if 0
/* return true if solvable s matches the spec */
/* see conda/models/match_spec.py */
int
solvable_conda_matchspec(Solvable *s, const char *spec)
{
}
#endif
