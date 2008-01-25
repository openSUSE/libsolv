/*
 * Copyright (c) 2008, Novell Inc.
 *
 * This program is licensed under the BSD license, read LICENSE.BSD
 * for further information
 */

#include <stdio.h>
#include <string.h>

#include "pool.h"
#include "util.h"

#define DIR_BLOCK 127

typedef struct _Dirpool {
  Id *dirs;
  int ndirs;
  Id *dirtraverse;
} Dirpool;

void
dirpool_create(Dirpool *dp)
{
  memset(dp, 0, sizeof(*dp));
}

void
dirpool_make_dirtraverse(Dirpool *dp)
{
  Id parent, i, *dirtraverse;
  if (!dp->ndirs)
    return;
  dp->dirs = sat_realloc2(dp->dirs, (dp->ndirs + DIR_BLOCK) &~ DIR_BLOCK, sizeof(Id));
  dirtraverse = sat_calloc((dp->ndirs + DIR_BLOCK) &~ DIR_BLOCK, sizeof(Id));
  for (parent = 0, i = 0; i < dp->ndirs; i++)
    {
      if (dp->dirs[i] > 0)
	continue;
      parent = -dp->dirs[i];
      dirtraverse[i] = dirtraverse[parent];
      dirtraverse[parent] = i + 1;
    }
  dp->dirtraverse = dirtraverse;
}

Id
dirpool_add_dir(Dirpool *dp, Id parent, Id comp)
{
  Id did, d, ds, *dirtraverse;

  if (!dp->ndirs)
    {
      dp->dirs = sat_malloc2(DIR_BLOCK, sizeof(Id));
      dp->ndirs = 2;
      dp->dirs[0] = 0;
      dp->dirs[1] = 1;	/* "" */
    }
  if (parent == 0 && comp == 1)
    return 1;
  if (!dp->dirtraverse)
    dirpool_make_dirtraverse(dp);
  dirtraverse = dp->dirtraverse;
  ds = dirtraverse[parent];
  while (ds)
    {
      /* ds: first component in this block
       * ds-1: parent link */
      for (d = ds--; d < dp->ndirs; d++)
	{
	  if (dp->dirs[d] == comp)
	    return d;
	  if (dp->dirs[d] <= 0)
	    break;
	}
      if (ds)
        ds = dp->dirtraverse[ds];
    }
  /* a new one, find last parent */
  for (did = dp->ndirs - 1; did > 0; did--)
    if (dp->dirs[did] <= 0)
      break;
  if (dp->dirs[did] != -parent)
    {
      if ((dp->ndirs & DIR_BLOCK) == 0)
	{
	  dp->dirs = sat_realloc2(dp->dirs, dp->ndirs + DIR_BLOCK, sizeof(Id));
	  dp->dirtraverse = sat_realloc2(dp->dirtraverse, dp->ndirs + DIR_BLOCK, sizeof(Id));
	}
      /* new parent block, link in */
      dp->dirs[dp->ndirs] = -parent;
      dp->dirtraverse[dp->ndirs] = dp->dirtraverse[parent];
      dp->dirtraverse[parent] = ++dp->ndirs;
    }
  if ((dp->ndirs & DIR_BLOCK) == 0)
    {
      dp->dirs = sat_realloc2(dp->dirs, dp->ndirs + DIR_BLOCK, sizeof(Id));
      dp->dirtraverse = sat_realloc2(dp->dirtraverse, dp->ndirs + DIR_BLOCK, sizeof(Id));
    }
  dp->dirs[dp->ndirs] = comp;
  dp->dirtraverse[dp->ndirs] = 0;
  return dp->ndirs++;
}

