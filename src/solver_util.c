/*
 * Copyright (c) 2017, SUSE Inc.
 *
 * This program is licensed under the BSD license, read LICENSE.BSD
 * for further information
 */

/*
 * solver_util.c
 *
 * Dependency solver helper functions
 */

#include <stdio.h>
#include <stdlib.h>
#include <unistd.h>
#include <string.h>

#include "solver.h"
#include "solver_private.h"
#include "bitmap.h"
#include "pool.h"
#include "poolarch.h"
#include "util.h"


/*-------------------------------------------------------------------
 * check if a installed package p is being updated
 */
static int
solver_is_updating(Solver *solv, Id p)
{
  /* check if the update rule is true */
  Pool *pool = solv->pool;
  Rule *r;
  Id l, pp;
  if (solv->decisionmap[p] >= 0)
    return 0;	/* old package stayed */
  r = solv->rules + solv->updaterules + (p - solv->installed->start);
  FOR_RULELITERALS(l, pp, r)
    if (l > 0 && l != p && solv->decisionmap[l] > 0)
      return 1;
  return 0;
}

/*-------------------------------------------------------------------
 * handle split provides
 *
 * a splitprovides dep looks like
 *     namespace:splitprovides(pkg REL_WITH path)
 * and is only true if pkg is installed and contains the specified path.
 * we also make sure that pkg is selected for an update, otherwise the
 * update would always be forced onto the user.
 * Map m is the map used when called from dep_possible.
 */
int
solver_splitprovides(Solver *solv, Id dep, Map *m)
{
  Pool *pool = solv->pool;
  Id p, pp;
  Reldep *rd;
  Solvable *s;

  if (!solv->dosplitprovides || !solv->installed)
    return 0;
  if (!ISRELDEP(dep))
    return 0;
  rd = GETRELDEP(pool, dep);
  if (rd->flags != REL_WITH)
    return 0;
  /*
   * things are a bit tricky here if pool->addedprovides == 1, because most split-provides are in
   * a non-standard location. If we simply call pool_whatprovides, we'll drag in the complete
   * file list. Instead we rely on pool_addfileprovides ignoring the addfileprovidesfiltered flag
   * for installed packages and check the lazywhatprovidesq (ignoring the REL_WITH part, but
   * we filter the package name further down anyway).
   */
  if (pool->addedfileprovides == 1 && !ISRELDEP(rd->evr) && !pool->whatprovides[rd->evr])
    pp = pool_searchlazywhatprovidesq(pool, rd->evr);
  else
    pp = pool_whatprovides(pool, dep);
  while ((p = pool->whatprovidesdata[pp++]) != 0)
    {
      /* here we have packages that provide the correct name and contain the path,
       * now do extra filtering */
      s = pool->solvables + p;
      if (s->repo != solv->installed || s->name != rd->name)
	continue;
      /* check if the package is updated. if m is set, we're called from dep_possible */
      if (m || solver_is_updating(solv, p))
	return 1;
    }
  return 0;
}

int
solver_dep_possible_slow(Solver *solv, Id dep, Map *m)
{
  Pool *pool = solv->pool;
  Id p, pp;

  if (ISRELDEP(dep))
    {
      Reldep *rd = GETRELDEP(pool, dep);
      if (rd->flags >= 8)
         {
          if (rd->flags == REL_COND || rd->flags == REL_UNLESS)
            return 1;
          if (rd->flags == REL_AND)
            {
              if (!solver_dep_possible_slow(solv, rd->name, m))
                return 0;
              return solver_dep_possible_slow(solv, rd->evr, m);
            }
          if (rd->flags == REL_OR)
            {
              if (solver_dep_possible_slow(solv, rd->name, m))
                return 1;
              return solver_dep_possible_slow(solv, rd->evr, m);
            }
          if (rd->flags == REL_NAMESPACE && rd->name == NAMESPACE_SPLITPROVIDES)
            return solver_splitprovides(solv, rd->evr, m);
        }
    }
  FOR_PROVIDES(p, pp, dep)
    {
      if (MAPTST(m, p))
        return 1;
    }
  return 0;
}

int
solver_dep_fulfilled_cplx(Solver *solv, Reldep *rd)
{
  Pool *pool = solv->pool;
  if (rd->flags == REL_COND)
    {
      if (ISRELDEP(rd->evr))
	{
	  Reldep *rd2 = GETRELDEP(pool, rd->evr);
	  if (rd2->flags == REL_ELSE)
	    {
	      if (solver_dep_fulfilled(solv, rd2->name))
		return solver_dep_fulfilled(solv, rd->name);
	      return solver_dep_fulfilled(solv, rd2->evr);
	    }
	}
      if (solver_dep_fulfilled(solv, rd->name))
	return 1;
      return !solver_dep_fulfilled(solv, rd->evr);
    }
  if (rd->flags == REL_UNLESS)
    {
      if (ISRELDEP(rd->evr))
	{
	  Reldep *rd2 = GETRELDEP(pool, rd->evr);
	  if (rd2->flags == REL_ELSE)
	    {
	      if (!solver_dep_fulfilled(solv, rd2->name))
		return solver_dep_fulfilled(solv, rd->name);
	      return solver_dep_fulfilled(solv, rd2->evr);
	    }
	}
      if (!solver_dep_fulfilled(solv, rd->name))
	return 0;
      return !solver_dep_fulfilled(solv, rd->evr);
    }
  if (rd->flags == REL_AND)
    {
      if (!solver_dep_fulfilled(solv, rd->name))
	return 0;
      return solver_dep_fulfilled(solv, rd->evr);
    }
  if (rd->flags == REL_OR)
    {
      if (solver_dep_fulfilled(solv, rd->name))
	return 1;
      return solver_dep_fulfilled(solv, rd->evr);
    }
  return 0;
}


/* mirrors solver_dep_fulfilled, but returns 2 if a new package
 * was involved */
static int
solver_dep_fulfilled_alreadyinstalled(Solver *solv, Id dep)
{
  Pool *pool = solv->pool;
  Id p, pp;
  int r;

  if (ISRELDEP(dep))
    {
      Reldep *rd = GETRELDEP(pool, dep);
      if (rd->flags == REL_COND)
	{
	  int r1, r2;
	  if (ISRELDEP(rd->evr))
	    {
	      Reldep *rd2 = GETRELDEP(pool, rd->evr);
	      if (rd2->flags == REL_ELSE)
		{
		  r1 = solver_dep_fulfilled_alreadyinstalled(solv, rd2->name);
		  if (r1)
		    {
		      r2 = solver_dep_fulfilled_alreadyinstalled(solv, rd->name);
		      return r2 && r1 == 2 ? 2 : r2;
		    }
		  return solver_dep_fulfilled_alreadyinstalled(solv, rd2->evr);
		}
	    }
	  r1 = solver_dep_fulfilled_alreadyinstalled(solv, rd->name);
	  r2 = !solver_dep_fulfilled_alreadyinstalled(solv, rd->evr);
	  if (!r1 && !r2)
	    return 0;
          return r1 == 2 ? 2 : 1;
	}
      if (rd->flags == REL_UNLESS)
	{
	  int r1, r2;
	  if (ISRELDEP(rd->evr))
	    {
	      Reldep *rd2 = GETRELDEP(pool, rd->evr);
	      if (rd2->flags == REL_ELSE)
		{
		  r1 = solver_dep_fulfilled_alreadyinstalled(solv, rd2->name);
		  if (r1)
		    {
		      r2 = solver_dep_fulfilled_alreadyinstalled(solv, rd2->evr);
		      return r2 && r1 == 2 ? 2 : r2;
		    }
		  return solver_dep_fulfilled_alreadyinstalled(solv, rd->name);
		}
	    }
	  /* A AND NOT(B) */
	  r1 = solver_dep_fulfilled_alreadyinstalled(solv, rd->name);
	  r2 = !solver_dep_fulfilled_alreadyinstalled(solv, rd->evr);
	  if (!r1 || !r2)
	    return 0;
          return r1 == 2 ? 2 : 1;
	}
      if (rd->flags == REL_AND)
        {
	  int r2, r1 = solver_dep_fulfilled_alreadyinstalled(solv, rd->name);
          if (!r1)
            return 0;
	  r2 = solver_dep_fulfilled_alreadyinstalled(solv, rd->evr);
	  if (!r2)
	    return 0;
          return r1 == 2 || r2 == 2 ? 2 : 1;
        }
      if (rd->flags == REL_OR)
	{
	  int r2, r1 = solver_dep_fulfilled_alreadyinstalled(solv, rd->name);
	  r2 = solver_dep_fulfilled_alreadyinstalled(solv, rd->evr);
	  if (!r1 && !r2)
	    return 0;
          return r1 == 2 || r2 == 2 ? 2 : 1;
	}
      if (rd->flags == REL_NAMESPACE && rd->name == NAMESPACE_SPLITPROVIDES)
        return solver_splitprovides(solv, rd->evr, 0) ? 2 : 0;
      if (rd->flags == REL_NAMESPACE && solv->installsuppdepq)
	{
	  Queue *q = solv->installsuppdepq;
	  int i;
	  for (i = 0; i < q->count; i++)
	    if (q->elements[i] == dep || q->elements[i] == rd->name)
	      return 2;
	}
    }
  r = 0;
  FOR_PROVIDES(p, pp, dep)
    if (solv->decisionmap[p] > 0)
      {
	Solvable *s = pool->solvables + p;
	if (s->repo && s->repo != solv->installed)
	  return 2;
        r = 1;
      }
  return r;
}

int
solver_is_supplementing_alreadyinstalled(Solver *solv, Solvable *s)
{
  Id sup, *supp;
  supp = s->repo->idarraydata + s->supplements;
  while ((sup = *supp++) != 0)
    if (solver_dep_fulfilled_alreadyinstalled(solv, sup) == 2)
      return 1;
  return 0;
}
/*
 * add all installed packages that package p obsoletes to Queue q.
 * Package p is not installed. Also, we know that if
 * solv->keepexplicitobsoletes is not set, p is not in the multiversion map.
 * Entries may get added multiple times.
 */
static void
solver_add_obsoleted(Solver *solv, Id p, Queue *q)
{
  Pool *pool = solv->pool;
  Repo *installed = solv->installed;
  Id p2, pp2;
  Solvable *s = pool->solvables + p;
  Id obs, *obsp;
  Id lastp2 = 0;

  if (!solv->keepexplicitobsoletes || !(solv->multiversion.size && MAPTST(&solv->multiversion, p)))
    {
      FOR_PROVIDES(p2, pp2, s->name)
        {
          Solvable *ps = pool->solvables + p2;
          if (ps->repo != installed)
            continue;
          if (!pool->implicitobsoleteusesprovides && ps->name != s->name)
            continue;
          if (pool->implicitobsoleteusescolors && !pool_colormatch(pool, s, ps))
            continue;
          queue_push(q, p2);
          lastp2 = p2;
        }
    }
  if (!s->obsoletes)
    return;
  obsp = s->repo->idarraydata + s->obsoletes;
  while ((obs = *obsp++) != 0)
    FOR_PROVIDES(p2, pp2, obs)
      {
        Solvable *ps = pool->solvables + p2;
        if (ps->repo != installed)
          continue;
        if (!pool->obsoleteusesprovides && !pool_match_nevr(pool, ps, obs))
          continue;
        if (pool->obsoleteusescolors && !pool_colormatch(pool, s, ps))
          continue;
        if (p2 == lastp2)
          continue;
        queue_push(q, p2);
        lastp2 = p2;
      }
}

/*
 * Call solver_add_obsoleted and intersect the result with the
 * elements in Queue q starting at qstart.
 * Assumes that it's the first call if qstart == q->count.
 * May use auxillary map m for the intersection process, all
 * elements of q starting at qstart must have their bit cleared.
 * (This is also true after the function returns.)
 * (See solver_add_obsoleted for limitations of the package p)
 */
void
solver_intersect_obsoleted(Solver *solv, Id p, Queue *q, int qstart, Map *m)
{
  int i, j;
  int qcount = q->count;

  solver_add_obsoleted(solv, p, q);
  if (qcount == qstart)
    return;     /* first call */
  if (qcount == q->count)
    j = qstart;
  else if (qcount == qstart + 1)
    {
      /* easy if there's just one element */
      j = qstart;
      for (i = qcount; i < q->count; i++)
        if (q->elements[i] == q->elements[qstart])
          {
            j++;        /* keep the element */
            break;
          }
    }
  else if (!m || (!m->size && q->count - qstart <= 8))
    {
      /* faster than a map most of the time */
      int k;
      for (i = j = qstart; i < qcount; i++)
        {
          Id ip = q->elements[i];
          for (k = qcount; k < q->count; k++)
            if (q->elements[k] == ip)
              {
                q->elements[j++] = ip;
                break;
              }
        }
    }
  else
    {
      /* for the really pathologic cases we use the map */
      Repo *installed = solv->installed;
      if (!m->size)
        map_init(m, installed->end - installed->start);
      for (i = qcount; i < q->count; i++)
        MAPSET(m, q->elements[i] - installed->start);
      for (i = j = qstart; i < qcount; i++)
        if (MAPTST(m, q->elements[i] - installed->start))
          {
            MAPCLR(m, q->elements[i] - installed->start);
            q->elements[j++] = q->elements[i];
          }
    }
  queue_truncate(q, j);
}

