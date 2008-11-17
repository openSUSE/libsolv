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


static Solver *prune_to_best_version_sortcmp_data;

/*-----------------------------------------------------------------*/

/*
 * prep for prune_best_version_arch
 *   sort by name
 */

static int
prune_to_best_version_sortcmp(const void *ap, const void *bp)
{
  Solver *solv = prune_to_best_version_sortcmp_data;
  Pool *pool = solv->pool;
  int r;
  Id a = *(Id *)ap;
  Id b = *(Id *)bp;
  Solvable *sa, *sb;

  sa = pool->solvables + a;
  sb = pool->solvables + b;
  r = sa->name - sb->name;
  if (r)
    {
      const char *na, *nb;
      /* different names. We use real strcmp here so that the result
       * is not depending on some random solvable order */
      na = id2str(pool, sa->name);
      nb = id2str(pool, sb->name);
      /* bring patterns to the front */
      /* XXX: no longer needed? */
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
      return strcmp(na, nb);
    }
  /* the same name, bring installed solvables to the front */
  if (solv->installed)
    {
      if (sa->repo == solv->installed)
	{
	  if (sb->repo != solv->installed)
	    return -1;
	}
      else if (sb->repo == solv->installed)
	return 1;	
    }
  /* sort by repository sub-prio (installed repo handled above) */
  r = (sb->repo ? sb->repo->subpriority : 0) - (sa->repo ? sa->repo->subpriority : 0);
  if (r)
    return r;
  /* no idea about the order, sort by id */
  return a - b;
}


/*
 * prune to repository with highest priority.
 * does not prune installed solvables.
 */

static void
prune_to_highest_prio(Pool *pool, Queue *plist)
{
  int i, j;
  Solvable *s;
  int bestprio = 0;

  /* prune to highest priority */
  for (i = 0; i < plist->count; i++)  /* find highest prio in queue */
    {
      s = pool->solvables + plist->elements[i];
      if (pool->installed && s->repo == pool->installed)
	continue;
      if (i == 0 || s->repo->priority > bestprio)
	bestprio = s->repo->priority;
    }
  for (i = j = 0; i < plist->count; i++) /* remove all with lower prio */
    {
      s = pool->solvables + plist->elements[i];
      if (s->repo->priority == bestprio || (pool->installed && s->repo == pool->installed))
	plist->elements[j++] = plist->elements[i];
    }
  plist->count = j;
}


/*
 * prune to recommended/suggested packages.
 * does not prune installed packages (they are also somewhat recommended).
 */

static void
prune_to_recommended(Solver *solv, Queue *plist)
{
  Pool *pool = solv->pool;
  int i, j, k, ninst;
  Solvable *s;
  Id p, pp, rec, *recp, sug, *sugp;

  ninst = 0;
  if (pool->installed)
    {
      for (i = 0; i < plist->count; i++)
	{
	  p = plist->elements[i];
	  s = pool->solvables + p;
	  if (pool->installed && s->repo == pool->installed)
	    ninst++;
	}
    }
  if (plist->count - ninst < 2)
    return;

  /* update our recommendsmap/suggestsmap */
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
  ninst = 0;
  for (i = j = 0; i < plist->count; i++)
    {
      p = plist->elements[i];
      s = pool->solvables + p;
      if (pool->installed && s->repo == pool->installed)
	{
	  ninst++;
	  if (j)
	    plist->elements[j++] = p;
	  continue;
	}
      if (!MAPTST(&solv->recommendsmap, p))
	if (!solver_is_supplementing(solv, s))
	  continue;
      if (!j && ninst)
	{
	  for (k = 0; j < ninst; k++)
	    {
	      s = pool->solvables + plist->elements[k];
	      if (pool->installed && s->repo == pool->installed)
	        plist->elements[j++] = plist->elements[k];
	    }
	}
      plist->elements[j++] = p;
    }
  if (j)
    plist->count = j;

  /* anything left to prune? */
  if (plist->count - ninst < 2)
    return;

  /* prune to suggested/enhanced*/
  ninst = 0;
  for (i = j = 0; i < plist->count; i++)
    {
      p = plist->elements[i];
      s = pool->solvables + p;
      if (pool->installed && s->repo == pool->installed)
	{
	  ninst++;
	  if (j)
	    plist->elements[j++] = p;
	  continue;
	}
      if (!MAPTST(&solv->suggestsmap, p))
        if (!solver_is_enhancing(solv, s))
	  continue;
      if (!j && ninst)
	{
	  for (k = 0; j < ninst; k++)
	    {
	      s = pool->solvables + plist->elements[k];
	      if (pool->installed && s->repo == pool->installed)
	        plist->elements[j++] = plist->elements[k];
	    }
	}
      plist->elements[j++] = p;
    }
  if (j)
    plist->count = j;
}

void
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
 * prune_to_best_version
 *
 * sort list of packages (given through plist) by name and evr
 * return result through plist
 */
void
prune_to_best_version(Solver *solv, Queue *plist)
{
  Pool *pool = solv->pool;
  Id best;
  int i, j;
  Solvable *s;

  if (plist->count < 2)		/* no need to prune for a single entry */
    return;
  POOL_DEBUG(SAT_DEBUG_POLICY, "prune_to_best_version %d\n", plist->count);

  prune_to_best_version_sortcmp_data = solv;
  /* sort by name first, prefer installed */
  qsort(plist->elements, plist->count, sizeof(Id), prune_to_best_version_sortcmp);

  /* delete obsoleted. hmm, looks expensive! */
  /* FIXME maybe also check provides depending on noupdateprovide? */
  /* FIXME do not prune cycles */
  for (i = 0; i < plist->count; i++)
    {
      Id p, pp, obs, *obsp;
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
	      if (!solv->obsoleteusesprovides && !pool_match_nevr(pool, pool->solvables + p, obs))
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
  /* delete zeroed out queue entries */
  for (i = j = 0; i < plist->count; i++)
    if (plist->elements[i])
      plist->elements[j++] = plist->elements[i];
  plist->count = j;

  /* now find best 'per name' */
  best = 0;
  for (i = j = 0; i < plist->count; i++)
    {
      s = pool->solvables + plist->elements[i];

      POOL_DEBUG(SAT_DEBUG_POLICY, "- %s[%s]\n",
		 solvable2str(pool, s),
		 (solv->installed && s->repo == solv->installed) ? "installed" : "not installed");

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
          if (evrcmp(pool, pool->solvables[best].evr, s->evr, EVRCMP_COMPARE) < 0)
            best = plist->elements[i];
        }
    }

  if (!best)
    best = plist->elements[0];

  plist->elements[j++] = best;
  plist->count = j;
}


/* legacy, do not use anymore!
 * (rates arch higher than version, but thats a policy)
 */

void
prune_best_arch_name_version(Solver *solv, Pool *pool, Queue *plist)
{
  if (solv && solv->bestSolvableCb)
    { /* The application is responsible for */
      return solv->bestSolvableCb(solv->pool, plist);
    }

  if (plist->count > 1)
    prune_to_best_arch(pool, plist);
  if (plist->count > 1)
    prune_to_best_version(solv, plist);
}


void
policy_filter_unwanted(Solver *solv, Queue *plist, int mode)
{
  Pool *pool = solv->pool;
  if (plist->count > 1 && mode != POLICY_MODE_SUGGEST)
    prune_to_highest_prio(pool, plist);
  if (plist->count > 1 && mode == POLICY_MODE_CHOOSE)
    prune_to_recommended(solv, plist);
  prune_best_arch_name_version(solv, pool, plist);
}


int
policy_illegal_archchange(Solver *solv, Solvable *s1, Solvable *s2)
{
  Pool *pool = solv->pool;
  Id a1 = s1->arch, a2 = s2->arch;

  if (solv && solv->archCheckCb)
    { /* The application is responsible for */
      return solv->archCheckCb(solv->pool, s1, s2);
    }

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
policy_illegal_vendorchange(Solver *solv, Solvable *s1, Solvable *s2)
{
  Pool *pool = solv->pool;
  Id vendormask1, vendormask2;

  if (solv && solv->vendorCheckCb)
   {   /* The application is responsible for */
     return solv->vendorCheckCb(solv->pool, s1, s2);
   }

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


/*
 * find update candidates
 * 
 * s: solvable to be updated
 * qs: [out] queue to hold Ids of candidates
 * allow_all: 0 = dont allow downgrades, 1 = allow all candidates
 * 
 */
void
policy_findupdatepackages(Solver *solv, Solvable *s, Queue *qs, int allow_all)
{
  /* installed packages get a special upgrade allowed rule */
  Pool *pool = solv->pool;
  Id p, pp, n, p2, pp2;
  Id obs, *obsp;
  Solvable *ps;

  queue_empty(qs);

  if (solv && solv->updateCandidateCb)
    { /* The application is responsible for */
      return solv->updateCandidateCb(solv->pool, s, qs);
    }

  /*
   * s = solvable ptr
   * n = solvable Id
   */
  n = s - pool->solvables;

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
	  if (!allow_all && !solv->allowdowngrade && evrcmp(pool, s->evr, ps->evr, EVRCMP_COMPARE) > 0)
	    continue;
	}
      else if (!solv->noupdateprovide && ps->obsoletes)   /* provides/obsoletes combination ? */
	{
	  obsp = ps->repo->idarraydata + ps->obsoletes;
	  while ((obs = *obsp++) != 0)	/* for all obsoletes */
	    {
	      FOR_PROVIDES(p2, pp2, obs)   /* and all matching providers of the obsoletes */
		{
		  if (!solv->obsoleteusesprovides && !pool_match_nevr(pool, pool->solvables + p2, obs))
		    continue;
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
      if (!allow_all && !solv->allowarchchange && s->arch != ps->arch && policy_illegal_archchange(solv, s, ps))
	continue;
      if (!allow_all && !solv->allowvendorchange && s->vendor != ps->vendor && policy_illegal_vendorchange(solv, s, ps))
	continue;
      queue_push(qs, p);
    }
  /* if we have found some valid candidates and noupdateprovide is not set, we're
     done. otherwise we fallback to all obsoletes */
  if (!solv->noupdateprovide && qs->count)
    return;
  if (solv->obsoletes && solv->obsoletes[n - solv->installed->start])
    {
      Id *opp;
      for (opp = solv->obsoletes_data + solv->obsoletes[n - solv->installed->start]; (p = *opp++) != 0;)
	{
	  ps = pool->solvables + p;
	  if (!allow_all && !solv->allowarchchange && s->arch != ps->arch && policy_illegal_archchange(solv, s, ps))
	    continue;
	  if (!allow_all && !solv->allowvendorchange && s->vendor != ps->vendor && policy_illegal_vendorchange(solv, s, ps))
	    continue;
	  queue_push(qs, p);
	}
    }
}

