/*
 * Copyright (c) 2014, Novell Inc.
 *
 * This program is licensed under the BSD license, read LICENSE.BSD
 * for further information
 */

/*
 * cplxdeps.c
 *
 * normalize complex dependencies into CNF/DNF form
 */

#include <stdio.h>
#include <stdlib.h>
#include <unistd.h>
#include <string.h>
#include <assert.h>

#include "pool.h"
#include "cplxdeps.h"

#ifdef ENABLE_COMPLEX_DEPS

#undef CPLXDEBUG

int
pool_is_complex_dep_rd(Pool *pool, Reldep *rd)
{
  for (;;)
    {
      if (rd->flags == REL_AND || rd->flags == REL_COND || rd->flags == REL_UNLESS)	/* those two are the complex ones */
	return 1;
      if (rd->flags != REL_OR)
	return 0;
      if (ISRELDEP(rd->name) && pool_is_complex_dep_rd(pool, GETRELDEP(pool, rd->name)))
	return 1;
      if (!ISRELDEP(rd->evr))
	return 0;
      rd = GETRELDEP(pool, rd->evr);
    }
}

/* expand simple dependencies into package lists */
static int
expand_simpledeps(Pool *pool, Queue *bq, int start, int split)
{
  int end = bq->count;
  int i, x;
  int newsplit = 0;
  for (i = start; i < end; i++)
    {
      if (i == split)
	newsplit = bq->count - (end - start);
      x = bq->elements[i];
      if (x == pool->nsolvables)
	{
	  Id *dp = pool->whatprovidesdata + bq->elements[++i];
	  for (; *dp; dp++)
	    queue_push(bq, *dp);
	}
      else
	queue_push(bq, x);
    }
  if (i == split)
    newsplit = bq->count - (end - start);
  queue_deleten(bq, start, end - start);
  return newsplit;
}

#ifdef CPLXDEBUG
static void
print_depblocks(Pool *pool, Queue *bq, int start)
{
  int i;

  for (i = start; i < bq->count; i++)
    {
      if (bq->elements[i] == pool->nsolvables)
	{
	  Id *dp = pool->whatprovidesdata + bq->elements[++i];
	  printf(" (");
	  while (*dp)
	    printf(" %s", pool_solvid2str(pool, *dp++));
	  printf(" )");
	}
      else if (bq->elements[i] > 0)
	printf(" %s", pool_solvid2str(pool, bq->elements[i]));
      else if (bq->elements[i] < 0)
	printf(" -%s", pool_solvid2str(pool, -bq->elements[i]));
      else
	printf(" ||");
    }
  printf("\n");
}
#endif

/* invert all literals in the blocks. note that this also turns DNF into CNF and vice versa */
static int
invert_depblocks(Pool *pool, Queue *bq, int start, int r)
{
  int i, j, end;
  if (r == 0 || r == 1)
    return r ? 0 : 1;
  expand_simpledeps(pool, bq, start, 0);
  end = bq->count;
  for (i = j = start; i < end; i++)
    {
      if (bq->elements[i])
	{
          bq->elements[i] = -bq->elements[i];
	  continue;
	}
      /* end of block reached, reverse */
      if (i - 1 > j)
	{
	  int k;
	  for (k = i - 1; j < k; j++, k--)
	    {
	      Id t = bq->elements[j];
	      bq->elements[j] = bq->elements[k];
	      bq->elements[k] = t;
	    }
	}
      j = i + 1;
    }
  return -1;
}

/* distributive property: (a1*a2 + b1*b2) * (c1*c2 + d1*d2) = 
   a1*a2*c1*c2 + a1*a2*d1*d2 + b1*b2*c1*c2 + b1*b2*d1*d2 */
static int
distribute_depblocks(Pool *pool, Queue *bq, int bqcnt, int bqcnt2, int flags)
{
  int i, j, bqcnt3;
#ifdef CPLXDEBUG
  printf("COMPLEX DISTRIBUTE %d %d %d\n", bqcnt, bqcnt2, bq->count);
#endif
  bqcnt2 = expand_simpledeps(pool, bq, bqcnt, bqcnt2);
  bqcnt3 = bq->count;
  for (i = bqcnt; i < bqcnt2; i++)
    {
      for (j = bqcnt2; j < bqcnt3; j++)
	{
	  int a, b;
	  int bqcnt4 = bq->count;
	  int k = i;

	  /* mix i block with j block, both blocks are sorted */
	  while (bq->elements[k] && bq->elements[j])
	    {
	      if (bq->elements[k] < bq->elements[j])
		queue_push(bq, bq->elements[k++]);
	      else
		{
		  if (bq->elements[k] == bq->elements[j])
		    k++;
		  queue_push(bq, bq->elements[j++]);
		}
	    }
	  while (bq->elements[j])
	    queue_push(bq, bq->elements[j++]);
	  while (bq->elements[k])
	    queue_push(bq, bq->elements[k++]);

	  /* block is finished, check for A + -A */
	  for (a = bqcnt4, b = bq->count - 1; a < b; )
	    {
	      if (-bq->elements[a] == bq->elements[b])
		break;
	      if (-bq->elements[a] > bq->elements[b])
		a++;
	      else
		b--;
	    }
	  if (a < b)
	    queue_truncate(bq, bqcnt4);	/* ignore this block */
	  else
	    queue_push(bq, 0);	/* finish block */
	}
      /* advance to next block */
      while (bq->elements[i])
	i++;
    }
  queue_deleten(bq, bqcnt, bqcnt3 - bqcnt);
  if (bqcnt == bq->count)
    return flags & CPLXDEPS_TODNF ? 0 : 1;
  return -1;
}

static int normalize_dep(Pool *pool, Id dep, Queue *bq, int flags);

static int
normalize_dep_or(Pool *pool, Id dep1, Id dep2, Queue *bq, int flags, int invflags)
{
  int r1, r2, bqcnt2, bqcnt = bq->count;
  r1 = normalize_dep(pool, dep1, bq, flags);
  if (r1 == 1)
    return 1;		/* early exit */
  bqcnt2 = bq->count;
  r2 = normalize_dep(pool, dep2, bq, flags ^ invflags);
  if (invflags)
    r2 = invert_depblocks(pool, bq, bqcnt2, r2);
  if (r1 == 1 || r2 == 1)
    {
      queue_truncate(bq, bqcnt);
      return 1;
    }
  if (r1 == 0)
    return r2;
  if (r2 == 0)
    return r1;
  if ((flags & CPLXDEPS_TODNF) == 0)
    return distribute_depblocks(pool, bq, bqcnt, bqcnt2, flags);
  return -1;
}

static int
normalize_dep_and(Pool *pool, Id dep1, Id dep2, Queue *bq, int flags, int invflags)
{
  int r1, r2, bqcnt2, bqcnt = bq->count;
  r1 = normalize_dep(pool, dep1, bq, flags);
  if (r1 == 0)
    return 0;		/* early exit */
  bqcnt2 = bq->count;
  r2 = normalize_dep(pool, dep2, bq, flags ^ invflags);
  if (invflags)
    r2 = invert_depblocks(pool, bq, bqcnt2, r2); 
  if (r1 == 0 || r2 == 0)
    {    
      queue_truncate(bq, bqcnt);
      return 0;
    }    
  if (r1 == 1)
    return r2;
  if (r2 == 1)
    return r1;
  if ((flags & CPLXDEPS_TODNF) != 0)
    return distribute_depblocks(pool, bq, bqcnt, bqcnt2, flags);
  return -1;
}

static int
normalize_dep_if_else(Pool *pool, Id dep1, Id dep2, Id dep3, Queue *bq, int flags)
{
  /* A IF (B ELSE C) -> (A OR ~B) AND (C OR B) */
  int r1, r2, bqcnt2, bqcnt = bq->count;
  r1 = normalize_dep_or(pool, dep1, dep2, bq, flags, CPLXDEPS_TODNF);
  if (r1 == 0)
    return 0;		/* early exit */
  bqcnt2 = bq->count;
  r2 = normalize_dep_or(pool, dep2, dep3, bq, flags, 0);
  if (r1 == 0 || r2 == 0)
    {
      queue_truncate(bq, bqcnt);
      return 0;
    }
  if (r1 == 1)
    return r2;
  if (r2 == 1)
    return r1;
  if ((flags & CPLXDEPS_TODNF) != 0)
    return distribute_depblocks(pool, bq, bqcnt, bqcnt2, flags);
  return -1;
}

static int
normalize_dep_unless_else(Pool *pool, Id dep1, Id dep2, Id dep3, Queue *bq, int flags)
{
  /* A UNLESS (B ELSE C) -> (A AND ~B) OR (C AND B) */
  int r1, r2, bqcnt2, bqcnt = bq->count;
  r1 = normalize_dep_and(pool, dep1, dep2, bq, flags, CPLXDEPS_TODNF);
  if (r1 == 1)
    return 1;		/* early exit */
  bqcnt2 = bq->count;
  r2 = normalize_dep_and(pool, dep2, dep3, bq, flags, 0);
  if (r1 == 1 || r2 == 1)
    {
      queue_truncate(bq, bqcnt);
      return 1;
    }
  if (r1 == 0)
    return r2;
  if (r2 == 0)
    return r1;
  if ((flags & CPLXDEPS_TODNF) == 0)
    return distribute_depblocks(pool, bq, bqcnt, bqcnt2, flags);
  return -1;
}

/*
 * returns:
 *   0: no blocks
 *   1: matches all
 *  -1: at least one block
 */
static int
normalize_dep(Pool *pool, Id dep, Queue *bq, int flags)
{
  Id p, dp;
  int bqcnt;

  if (pool_is_complex_dep(pool, dep))
    {
      Reldep *rd = GETRELDEP(pool, dep);
      if (rd->flags == REL_COND)
	{
	  Id evr = rd->evr;
	  if (ISRELDEP(evr))
	    {
	      Reldep *rd2 = GETRELDEP(pool, evr);
	      if (rd2->flags == REL_ELSE)
		return normalize_dep_if_else(pool, rd->name, rd2->name, rd2->evr, bq, flags);
	    }
	  return normalize_dep_or(pool, rd->name, rd->evr, bq, flags, CPLXDEPS_TODNF);
	}
      if (rd->flags == REL_UNLESS)
	{
	  Id evr = rd->evr;
	  if (ISRELDEP(evr))
	    {
	      Reldep *rd2 = GETRELDEP(pool, evr);
	      if (rd2->flags == REL_ELSE)
		return normalize_dep_unless_else(pool, rd->name, rd2->name, rd2->evr, bq, flags);
	    }
	  return normalize_dep_and(pool, rd->name, rd->evr, bq, flags, CPLXDEPS_TODNF);
	}
      if (rd->flags == REL_OR)
	return normalize_dep_or(pool, rd->name, rd->evr, bq, flags, 0);
      if (rd->flags == REL_AND)
	return normalize_dep_and(pool, rd->name, rd->evr, bq, flags, 0);
    }

  /* fallback case: just use package list */
  dp = pool_whatprovides(pool, dep);
  if (dp <= 2 || !pool->whatprovidesdata[dp])
    return dp == 2 ? 1 : 0;
  if (pool->whatprovidesdata[dp] == SYSTEMSOLVABLE)
    return 1;
  bqcnt = bq->count;
  if ((flags & CPLXDEPS_NAME) != 0)
    {
      while ((p = pool->whatprovidesdata[dp++]) != 0)
	{
	  if (!pool_match_nevr(pool, pool->solvables + p, dep))
	    continue;
	  queue_push(bq, p);
	  if ((flags & CPLXDEPS_TODNF) != 0)
	    queue_push(bq, 0);
	}
    }
  else if ((flags & CPLXDEPS_TODNF) != 0)
    {
      while ((p = pool->whatprovidesdata[dp++]) != 0)
        queue_push2(bq, p, 0);
    }
  else
    queue_push2(bq, pool->nsolvables, dp);	/* not yet expanded marker + offset */
  if (bq->count == bqcnt)
    return 0;	/* no provider */
  if (!(flags & CPLXDEPS_TODNF))
    queue_push(bq, 0);	/* finish block */
  return -1;
}

int
pool_normalize_complex_dep(Pool *pool, Id dep, Queue *bq, int flags)
{
  int i, bqcnt = bq->count;

  i = normalize_dep(pool, dep, bq, flags);
  if ((flags & CPLXDEPS_EXPAND) != 0)
    {
      if (i != 0 && i != 1)
        expand_simpledeps(pool, bq, bqcnt, 0);
    }
  if ((flags & CPLXDEPS_INVERT) != 0)
    i = invert_depblocks(pool, bq, bqcnt, i);
#ifdef CPLXDEBUG
  if (i == 0)
    printf("NONE\n");
  else if (i == 1)
    printf("ALL\n");
  else
    print_depblocks(pool, bq, bqcnt);
#endif
  return i;
}

void
pool_add_pos_literals_complex_dep(Pool *pool, Id dep, Queue *q, Map *m, int neg)
{
  while (ISRELDEP(dep))
    {
      Reldep *rd = GETRELDEP(pool, dep);
      if (rd->flags != REL_AND && rd->flags != REL_OR && rd->flags != REL_COND && rd->flags != REL_UNLESS)
	break;
      pool_add_pos_literals_complex_dep(pool, rd->name, q, m, neg);
      dep = rd->evr;
      if (rd->flags == REL_COND || rd->flags == REL_UNLESS)
	{
	  neg = !neg;
	  if (ISRELDEP(dep))
	    {
	      Reldep *rd2 = GETRELDEP(pool, rd->evr);
	      if (rd2->flags == REL_ELSE)
		{
	          pool_add_pos_literals_complex_dep(pool, rd2->evr, q, m, !neg);
		  dep = rd2->name;
		}
	    }
	}
    }
  if (!neg)
    {
      Id p, pp;
      FOR_PROVIDES(p, pp, dep)
	if (!MAPTST(m, p))
	  queue_push(q, p);
    }
}

#endif	/* ENABLE_COMPLEX_DEPS */

