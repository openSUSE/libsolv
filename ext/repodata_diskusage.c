/*
 * repodata_diskusage.c
 *
 * Small helper to convert diskusage data from sustags or rpmmd
 *
 * Copyright (c) 2017, Novell Inc.
 *
 * This program is licensed under the BSD license, read LICENSE.BSD
 * for further information
 */

#include <sys/types.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "pool.h"
#include "repo.h"
#include "util.h"
#include "repodata_diskusage.h"

/* The queue contains (dirid, kbytes, inodes) triplets */

static int
add_diskusage_sortfn(const void *ap, const void *bp, void *dp)
{
  return *(Id *)ap - *(Id *)bp;
}

void
repodata_add_diskusage(Repodata *data, Id handle, Queue *q)
{
  int i, j;
  Dirpool *dp = &data->dirpool;

  /* Sort in dirid order. This ensures that parents come before
   * their children. */
  if (q->count > 3)
    solv_sort(q->elements, q->count / 3, 3 * sizeof(Id), add_diskusage_sortfn, 0);
  for (i = 3; i < q->count; i += 3)
    {
      /* subtract data from parent */
      Id did = q->elements[i];
      if (i + 3 < q->count && q->elements[i + 3] == did)
	{
	  /* identical directory entry! zero this one */
	  q->elements[i + 1] = 0;
	  q->elements[i + 2] = 0;
	  continue;
	}
      while (did)
	{
	  did = dirpool_parent(dp, did);
	  for (j = i - 3; j >= 0; j -= 3)
	    if (q->elements[j] == did)
	      break;
	  if (j >= 0)
	    {
	      if ((unsigned int)q->elements[j + 1] > (unsigned int)q->elements[i + 1])
		q->elements[j + 1] -= q->elements[i + 1];
	      else
		q->elements[j + 1] = 0;
	      if ((unsigned int)q->elements[j + 2] > (unsigned int)q->elements[i + 2])
		q->elements[j + 2] -= q->elements[i + 2];
	      else
		q->elements[j + 2] = 0;
	      break;
	    }
	}
    }
  /* now commit data */
  for (i = 0; i < q->count; i += 3)
    if (q->elements[i + 1] || q->elements[i + 2])
      repodata_add_dirnumnum(data, handle, SOLVABLE_DISKUSAGE, q->elements[i], q->elements[i + 1], q->elements[i + 2]);
  /* empty queue */
  queue_empty(q);
}

