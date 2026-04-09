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

/* directories are stored as components,
 * components are simple ids from the string pool
 *   /usr/bin   ->  "", "usr", "bin"
 *   /usr/lib   ->  "", "usr", "lib"
 *   foo/bar    ->  "foo", "bar"
 *   /usr/games ->  "", "usr", "games"
 *
 * all directories are stores in the "dirs" array
 *   dirs[id] > 0 : component string pool id
 *   dirs[id] <= 0 : -(parent directory id)
 *
 * Directories with the same parent are stored as
 * multiple blocks. We need multiple blocks because
 * we cannot insert entries into old blocks, as that
 * would shift the ids of already used directories.
 * Each block starts with (-parent_dirid) and contains
 * component ids of the directory entries.
 * (The (-parent_dirid) entry is not a valid directory
 * id, it's just used internally)
 *
 * There is also the aux "dirtraverse" array, which
 * is created on demand to speed things up a bit.
 * if dirs[id] > 0, dirtravers[id] points to the first
 * entry in the last block with parent id.
 * if dirs[id] <= 0, dirtravers[id] points to the entry
 * in the previous block with the same parent.
 * (Thus it acts as a linked list that starts at the
 * parent dirid and chains all the blocks with that
 * parent.)
 *
 *  id    dirs[id]  dirtraverse[id]
 *   0     0           8       [no parent, block#0]
 *   1    ""           3
 *   2    -1                   [parent 1, /, block #0]
 *   3    "usr"       12
 *   4    -3                   [parent 3, /usr, block #0]
 *   5    "bin"
 *   6    "lib"
 *   7     0           1       [no parent, block#1]
 *   8    "foo"       10
 *   9    -8                   [parent 8, foo, block #0]
 *  10    "bar"
 *  11    -3           5       [parent 3, /usr, block #1]
 *  12    "games"
 *
 * to find all children of dirid 3 ("/usr"), follow the
 * dirtraverse link to 12 -> "games". Then follow the
 * dirtraverse link of this block to 5 -> "bin", "lib"
 */

void
dirpool_init(Dirpool *dp)
{
  memset(dp, 0, sizeof(*dp));
}

void
dirpool_free(Dirpool *dp)
{
  solv_free(dp->dirs);
  solv_free(dp->dirtraverse);
  solv_free(dp->dirhashtbl);
}

void
dirpool_make_dirtraverse(Dirpool *dp)
{
  Id parent, i, *dirtraverse;
  if (!dp->ndirs)
    return;
  dp->dirs = solv_extend_resize(dp->dirs, dp->ndirs, sizeof(Id), DIR_BLOCK);
  dirtraverse = solv_calloc_block(dp->ndirs, sizeof(Id), DIR_BLOCK);
  for (i = 0; i < dp->ndirs; i++)
    {
      if (dp->dirs[i] > 0)
	continue;
      parent = -dp->dirs[i];
      dirtraverse[i] = dirtraverse[parent];
      dirtraverse[parent] = i + 1;
    }
  dp->dirtraverse = dirtraverse;
}

/* Build or rebuild the (parent, comp) -> dirid hash table.
 * This replaces the old O(B*C) dirtraverse linked-list scan
 * with O(1) amortized lookup in dirpool_add_dir. */
static void
dirpool_resize_hash(Dirpool *dp, int numnew)
{
  Hashval hashmask, h, hh;
  Hashtable ht;
  Id i, d, parent;

  hashmask = mkmask(dp->ndirs + numnew);
  if (dp->dirhashtbl && hashmask <= dp->dirhashmask)
    return;
  dp->dirhashmask = hashmask;
  solv_free(dp->dirhashtbl);
  ht = dp->dirhashtbl = (Hashtable)solv_calloc(hashmask + 1, sizeof(Id));
  for (i = 2; i < dp->ndirs; i++)
    {
      if (dp->dirs[i] <= 0)
	continue;
      /* walk back to block header to find parent */
      for (d = i; dp->dirs[--d] > 0; )
	;
      parent = -dp->dirs[d];
      h = relhash(parent, dp->dirs[i], 0) & hashmask;
      hh = HASHCHAIN_START;
      while (ht[h])
	h = HASHCHAIN_NEXT(h, hh, hashmask);
      ht[h] = i;
    }
}

Id
dirpool_add_dir(Dirpool *dp, Id parent, Id comp, int create)
{
  Id did, d;
  Hashval h, hh, hashmask;
  Hashtable ht;

  if (!dp->ndirs)
    {
      if (!create)
	return 0;
      dp->ndirs = 2;
      dp->dirs = solv_extend_resize(dp->dirs, dp->ndirs, sizeof(Id), DIR_BLOCK);
      dp->dirs[0] = 0;
      dp->dirs[1] = 1;	/* "" */
    }
  if (comp <= 0)
    return 0;
  if (parent == 0 && comp == 1)
    return 1;

  /* grow hash table if load factor exceeds 50% */
  if ((Hashval)dp->ndirs * 2 > dp->dirhashmask)
    dirpool_resize_hash(dp, DIR_BLOCK);

  ht = dp->dirhashtbl;
  hashmask = dp->dirhashmask;

  /* probe for existing (parent, comp) entry */
  h = relhash(parent, comp, 0) & hashmask;
  hh = HASHCHAIN_START;
  while ((did = ht[h]) != 0)
    {
      if (dp->dirs[did] == comp)
	{
	  /* comp matches, verify parent by walking back to
	   * the block header (short sequential scan) */
	  d = did;
	  while (dp->dirs[--d] > 0)
	    ;
	  if (-dp->dirs[d] == parent)
	    return did;
	}
      h = HASHCHAIN_NEXT(h, hh, hashmask);
    }

  if (!create)
    return 0;

  if (!dp->dirtraverse)
    dirpool_make_dirtraverse(dp);

  /* find last parent block */
  for (did = dp->ndirs - 1; did > 0; did--)
    if (dp->dirs[did] <= 0)
      break;
  if (dp->dirs[did] != -parent)
    {
      /* make room for parent entry */
      dp->dirs = solv_extend(dp->dirs, dp->ndirs, 1, sizeof(Id), DIR_BLOCK);
      dp->dirtraverse = solv_extend(dp->dirtraverse, dp->ndirs, 1, sizeof(Id), DIR_BLOCK);
      /* new parent block, link in */
      dp->dirs[dp->ndirs] = -parent;
      dp->dirtraverse[dp->ndirs] = dp->dirtraverse[parent];
      dp->dirtraverse[parent] = ++dp->ndirs;
    }
  /* make room for new entry */
  dp->dirs = solv_extend(dp->dirs, dp->ndirs, 1, sizeof(Id), DIR_BLOCK);
  dp->dirtraverse = solv_extend(dp->dirtraverse, dp->ndirs, 1, sizeof(Id), DIR_BLOCK);
  dp->dirs[dp->ndirs] = comp;
  dp->dirtraverse[dp->ndirs] = 0;

  /* insert new entry into hash table (h still points at
   * the empty slot from the failed probe above) */
  ht[h] = dp->ndirs;

  return dp->ndirs++;
}
