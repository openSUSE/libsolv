/*
 * Copyright (c) 2007, Novell Inc.
 *
 * This program is licensed under the BSD license, read LICENSE.BSD
 * for further information
 */

/*
 * Generic policy interface for SAT solver
 * 
 */

#include <stdio.h>
#include <stdlib.h>
#include <unistd.h>
#include <string.h>

#include "solver.h"
#include "evr.h"
#include "policy.h"
#include "poolvendor.h"
#include "poolarch.h"


static Pool *prune_best_version_arch_sortcmp_data;

/*-----------------------------------------------------------------*/

/*
 * prep for prune_best_version_arch
 *   sort by name
 */

static int
prune_best_version_arch_sortcmp(const void *ap, const void *bp)
{
  Pool *pool = prune_best_version_arch_sortcmp_data;
  int r;
  Id a = *(Id *)ap;
  Id b = *(Id *)bp;
  r = pool->solvables[a].name - pool->solvables[b].name;
  if (r)
    {
      const char *na, *nb;
      /* different names. We use real strcmp here so that the result
       * is not depending on some random solvable order */
      na = id2str(pool, pool->solvables[a].name);
      nb = id2str(pool, pool->solvables[b].name);
      /* bring selections and patterns to the front */
      if (!strncmp(na, "pattern:", 8))
	{
          if (strncmp(nb, "pattern:", 8))
	    return -1;
	}
      else if (!strncmp(nb, "pattern:", 8))
	{
          if (strncmp(na, "pattern:", 8))
	    return 1;
	}
      if (!strncmp(na, "selection:", 10))
	{
          if (strncmp(nb, "selection:", 10))
	    return -1;
	}
      else if (!strncmp(nb, "selection:", 10))
	{
          if (strncmp(na, "selection:", 10))
	    return 1;
	}
      return strcmp(na, nb);
    }
  return a - b;
}

static void
prune_to_highest_prio(Pool *pool, Queue *plist)
{
  int i, j;
  Solvable *s;
  int bestprio = 0;

  /* prune to highest priority */
  for (i = 0; i < plist->count; i++)
    {
      s = pool->solvables + plist->elements[i];
      if (i == 0 || s->repo->priority > bestprio)
	bestprio = s->repo->priority;
    }
  for (i = j = 0; i < plist->count; i++)
    {
      s = pool->solvables + plist->elements[i];
      if (s->repo->priority == bestprio)
	plist->elements[j++] = plist->elements[i];
    }
  plist->count = j;
}

/*
 * prune_to_recommended
 *
 * XXX: should we prune to requires/suggests that are already
 * fulfilled by other packages?
 */
static void
prune_to_recommended(Solver *solv, Queue *plist)
{
  Pool *pool = solv->pool;
  int i, j;
  Solvable *s;
  Id p, *pp, rec, *recp, sug, *sugp;

  if (solv->recommends_index < 0)
    {
      MAPZERO(&solv->recommendsmap);
      MAPZERO(&solv->suggestsmap);
      solv->recommends_index = 0;
    }
  while (solv->recommends_index < solv->decisionq.count)
    {
      p = solv->decisionq.elements[solv->recommends_index++];
      if (p < 0)
	continue;
      s = pool->solvables + p;
      if (s->recommends)
	{
	  recp = s->repo->idarraydata + s->recommends;
          while ((rec = *recp++) != 0)
	    FOR_PROVIDES(p, pp, rec)
	      MAPSET(&solv->recommendsmap, p);
	}
      if (s->suggests)
	{
	  sugp = s->repo->idarraydata + s->suggests;
          while ((sug = *sugp++) != 0)
	    FOR_PROVIDES(p, pp, sug)
	      MAPSET(&solv->suggestsmap, p);
	}
    }
  /* prune to recommended/supplemented */
  for (i = j = 0; i < plist->count; i++)
    {
      p = plist->elements[i];
      if (MAPTST(&solv->recommendsmap, p))
	{
	  plist->elements[j++] = p;
	  continue;
	}
      if (solver_is_supplementing(solv, pool->solvables + p))
        plist->elements[j++] = p;
    }
  if (j)
    plist->count = j;

  /* prune to suggested/enhanced*/
  if (plist->count < 2)
    return;
  for (i = j = 0; i < plist->count; i++)
    {
      p = plist->elements[i];
      if (MAPTST(&solv->suggestsmap, p))
	{
	  plist->elements[j++] = p;
	  continue;
	}
      if (solver_is_enhancing(solv, pool->solvables + p))
        plist->elements[j++] = p;
    }
  if (j)
    plist->count = j;
}

static void
prune_to_best_arch(Pool *pool, Queue *plist)
{
  Id a, bestscore;
  Solvable *s;
  int i, j;

  if (!pool->id2arch || plist->count < 2)
    return;
  bestscore = 0;
  for (i = 0; i < plist->count; i++)
    {
      s = pool->solvables + plist->elements[i];
      a = s->arch;
      a = (a <= pool->lastarch) ? pool->id2arch[a] : 0;
      if (a && a != 1 && (!bestscore || a < bestscore))
	bestscore = a;
    }
  for (i = j = 0; i < plist->count; i++)
    {
      s = pool->solvables + plist->elements[i];
      a = s->arch;
      if (a > pool->lastarch)
	continue;
      a = pool->id2arch[a];
      /* a == 1 -> noarch */
      if (a != 1 && ((a ^ bestscore) & 0xffff0000) != 0)
	continue;
      plist->elements[j++] = plist->elements[i];
    }
  if (j)
    plist->count = j;
}

/*
 * prune_best_version_arch
 * 
 * sort list of packages (given through plist) by name and evr
 * return result through plist
 * 
 */

/* FIXME: should prefer installed if identical version */

static void
prune_to_best_version(Pool *pool, Queue *plist)
{
  Id best = ID_NULL;
  int i, j;
  Solvable *s;

  if (plist->count < 2)		/* no need to prune for a single entry */
    return;
  POOL_DEBUG(SAT_DEBUG_POLICY, "prune_to_best_version %d\n", plist->count);

  /* prune to best architecture */
  if (pool->id2arch)

  prune_best_version_arch_sortcmp_data = pool;
  /* sort by name first */
  qsort(plist->elements, plist->count, sizeof(Id), prune_best_version_arch_sortcmp);

  /* delete obsoleted. hmm, looks expensive! */
  /* FIXME maybe also check provides depending on noupdateprovide? */
  /* FIXME do not prune cycles */
  for (i = 0; i < plist->count; i++)
    {
      Id p, *pp, obs, *obsp;
      s = pool->solvables + plist->elements[i];
      if (!s->obsoletes)
	continue;
      obsp = s->repo->idarraydata + s->obsoletes;
      while ((obs = *obsp++) != 0)
	{
	  FOR_PROVIDES(p, pp, obs)
	    {
	      if (pool->solvables[p].name == s->name)
		continue;
	      for (j = 0; j < plist->count; j++)
		{
		  if (i == j)
		    continue;
		  if (plist->elements[j] == p)
		    plist->elements[j] = 0;
		}
	    }
	}
    }
  for (i = j = 0; i < plist->count; i++)
    if (plist->elements[i])
      plist->elements[j++] = plist->elements[i];
  plist->count = j;

  /* now find best 'per name' */
  for (i = j = 0; i < plist->count; i++)
    {
      s = pool->solvables + plist->elements[i];

      POOL_DEBUG(SAT_DEBUG_POLICY, "- %s\n", solvable2str(pool, s));

      if (!best)		       /* if no best yet, the current is best */
        {
          best = plist->elements[i];
          continue;
        }

      /* name switch: re-init */
      if (pool->solvables[best].name != s->name)   /* new name */
        {
          plist->elements[j++] = best; /* move old best to front */
          best = plist->elements[i];   /* take current as new best */
          continue;
        }

      if (pool->solvables[best].evr != s->evr)   /* compare evr */
        {
          if (evrcmp(pool, pool->solvables[best].evr, s->evr) < 0)
            best = plist->elements[i];
        }
    }

  if (best == ID_NULL)
    best = plist->elements[0];

  plist->elements[j++] = best;
  plist->count = j;
}

void
prune_best_version_arch(Pool *pool, Queue *plist)
{
  if (plist->count > 1)
    prune_to_best_arch(pool, plist);
  if (plist->count > 1)
    prune_to_best_version(pool, plist);
}


void
policy_filter_unwanted(Solver *solv, Queue *plist, Id inst, int mode)
{
  Pool *pool = solv->pool;
  if (plist->count > 1 && mode != POLICY_MODE_SUGGEST)
    prune_to_highest_prio(pool, plist);
  if (plist->count > 1 && mode == POLICY_MODE_CHOOSE)
    prune_to_recommended(solv, plist);
  /* FIXME: do this different! */
  if (inst)
    queue_push(plist, inst);
  if (plist->count > 1)
    prune_to_best_arch(pool, plist);
  if (plist->count > 1)
    prune_to_best_version(pool, plist);
}


int
policy_illegal_archchange(Pool *pool, Solvable *s1, Solvable *s2)
{
  Id a1 = s1->arch, a2 = s2->arch;

  /* we allow changes to/from noarch */
  if (a1 == a2 || a1 == ARCH_NOARCH || a2 == ARCH_NOARCH)
    return 0;
  if (!pool->id2arch)
    return 0;
  a1 = a1 <= pool->lastarch ? pool->id2arch[a1] : 0;
  a2 = a2 <= pool->lastarch ? pool->id2arch[a2] : 0;
  if (((a1 ^ a2) & 0xffff0000) != 0)
    return 1;
  return 0;
}

int
policy_illegal_vendorchange(Pool *pool, Solvable *s1, Solvable *s2)
{
  Id vendormask1, vendormask2;
  if (s1->vendor == s2->vendor)
    return 0;
  vendormask1 = pool_vendor2mask(pool, s1->vendor);
  if (!vendormask1)
    return 0;
  vendormask2 = pool_vendor2mask(pool, s2->vendor);
  if ((vendormask1 & vendormask2) == 0)
    return 0;
  return 1;
}

void
policy_findupdatepackages(Solver *solv, Solvable *s, Queue *qs, int allowall)
{
  /* installed packages get a special upgrade allowed rule */
  Pool *pool = solv->pool;
  Id p, *pp, n, p2, *pp2;
  Id obs, *obsp;
  Solvable *ps;
  Id vendormask;

  queue_empty(qs);
  /*
   * s = solvable ptr
   * n = solvable Id
   */
  n = s - pool->solvables;
  vendormask = pool_vendor2mask(pool, s->vendor);

  /*
   * look for updates for s
   */
  FOR_PROVIDES(p, pp, s->name)	/* every provider of s' name */
    {
      if (p == n)		/* skip itself */
	continue;

      ps = pool->solvables + p;
      if (s->name == ps->name)	/* name match */
	{
	  if (!allowall)
	    {
	      if (!solv->allowdowngrade && evrcmp(pool, s->evr, ps->evr) > 0)
	        continue;
	      if (!solv->allowarchchange && s->arch != ps->arch && policy_illegal_archchange(pool, s, ps))
		continue;
	      if (!solv->allowvendorchange && s->vendor != ps->vendor && policy_illegal_vendorchange(pool, s, ps))
		continue;
	    }
	}
      else if (!solv->noupdateprovide && ps->obsoletes)   /* provides/obsoletes combination ? */
	{
	  obsp = ps->repo->idarraydata + ps->obsoletes;
	  while ((obs = *obsp++) != 0)	/* for all obsoletes */
	    {
	      FOR_PROVIDES(p2, pp2, obs)   /* and all matching providers of the obsoletes */
		{
		  if (p2 == n)		/* match ! */
		    break;
		}
	      if (p2)			/* match! */
		break;
	    }
	  if (!obs)			/* continue if no match */
	    continue;
	  /* here we have 'p' with a matching provides/obsoletes combination
	   * thus flagging p as a valid update candidate for s
	   */
	}
      else
        continue;
      queue_push(qs, p);
    }
  if (solv->noupdateprovide && solv->obsoletes && solv->obsoletes[n - solv->installed->start])
    {
      for (pp = solv->obsoletes_data + solv->obsoletes[n - solv->installed->start]; (p = *pp++) != 0;)
	queue_push(qs, p);
    }
}

