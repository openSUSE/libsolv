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
#include "dirpool.h"

#define DIR_BLOCK 127

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
  dp->dirs = sat_extend_resize(dp->dirs, dp->ndirs, sizeof(Id), DIR_BLOCK);
  dirtraverse = sat_calloc_block(dp->ndirs, sizeof(Id), DIR_BLOCK);
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
dirpool_add_dir(Dirpool *dp, Id parent, Id comp, int create)
{
  Id did, d, ds, *dirtraverse;

  if (!dp->ndirs)
    {
      dp->ndirs = 2;
      dp->dirs = sat_extend_resize(dp->dirs, dp->ndirs, sizeof(Id), DIR_BLOCK);
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
  if (!create)
    return 0;
  /* a new one, find last parent */
  for (did = dp->ndirs - 1; did > 0; did--)
    if (dp->dirs[did] <= 0)
      break;
  if (dp->dirs[did] != -parent)
    {
      /* make room for parent entry */
      dp->dirs = sat_extend(dp->dirs, dp->ndirs, 1, sizeof(Id), DIR_BLOCK);
      dp->dirtraverse = sat_extend(dp->dirtraverse, dp->ndirs, 1, sizeof(Id), DIR_BLOCK);
      /* new parent block, link in */
      dp->dirs[dp->ndirs] = -parent;
      dp->dirtraverse[dp->ndirs] = dp->dirtraverse[parent];
      dp->dirtraverse[parent] = ++dp->ndirs;
    }
  /* make room for new entry */
  dp->dirs = sat_extend(dp->dirs, dp->ndirs, 1, sizeof(Id), DIR_BLOCK);
  dp->dirtraverse = sat_extend(dp->dirtraverse, dp->ndirs, 1, sizeof(Id), DIR_BLOCK);
  dp->dirs[dp->ndirs] = comp;
  dp->dirtraverse[dp->ndirs] = 0;
  return dp->ndirs++;
}
