/*
 * Copyright (c) 2007, Novell Inc.
 *
 * This program is licensed under the BSD license, read LICENSE.BSD
 * for further information
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* we need FNM_CASEFOLD */
#ifndef _GNU_SOURCE
#define _GNU_SOURCE
#endif

#include <fnmatch.h>

#include "pool.h"
#include "poolid.h"
#include "poolvendor.h"
#include "util.h"

const char *vendors[] = {
  "SUSE*",
  "openSUSE*",
  "SGI*",
  "Novell*",
  "Silicon Graphics*",
  "Jpackage Project*",
  "ATI Technologies Inc.*",
  "Nvidia*",
  0,
  0,
};

Id pool_vendor2mask(Pool *pool, Id vendor)
{
  const char *vstr;
  int i;
  Id mask, m;
  const char **v;

  if (vendor == 0)
    return 0;
  for (i = 0; i < pool->vendormap.count; i += 2)
    if (pool->vendormap.elements[i] == vendor)
      return pool->vendormap.elements[i + 1];
  vstr = id2str(pool, vendor);
  m = 1;
  mask = 0;
  for (v = vendors; ; v++)
    {
      if (*v == 0)
	{
	  v++;
	  if (*v == 0)
	    break;
	  if (m == (1 << 31))
	    break;
	  m <<= 1;
	}
      if (fnmatch(*v, vstr, FNM_CASEFOLD) == 0)
	{
	  mask |= m;
	  while (v[1])
	    v++;
	}
    }
  queue_push(&pool->vendormap, vendor);
  queue_push(&pool->vendormap, mask);
  return mask;
}
