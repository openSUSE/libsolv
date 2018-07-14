/*
 * Copyright (c) 2017, SUSE LLC.
 *
 * This program is licensed under the BSD license, read LICENSE.BSD
 * for further information
 */

/* Functions that help getting/setting userinstalled packages. */

#include <stdio.h>
#include <stdlib.h>
#include <unistd.h>
#include <string.h>

#include "solver.h"
#include "solver_private.h"
#include "bitmap.h"
#include "pool.h"
#include "util.h"
#include "poolarch.h"
#include "linkedpkg.h"

static int
get_userinstalled_cmp(const void *ap, const void *bp, void *dp)
{
  return *(Id *)ap - *(Id *)bp;
}

static int
get_userinstalled_cmp_names(const void *ap, const void *bp, void *dp)
{
  Pool *pool = dp;
  return strcmp(pool_id2str(pool, *(Id *)ap), pool_id2str(pool, *(Id *)bp));
}

static int
get_userinstalled_cmp_namearch(const void *ap, const void *bp, void *dp)
{
  Pool *pool = dp;
  int r;
  r = strcmp(pool_id2str(pool, ((Id *)ap)[0]), pool_id2str(pool, ((Id *)bp)[0]));
  if (r)
    return r;
  return strcmp(pool_id2str(pool, ((Id *)ap)[1]), pool_id2str(pool, ((Id *)bp)[1]));
}

static void
get_userinstalled_sort_uniq(Pool *pool, Queue *q, int flags)
{
  Id lastp = -1, lasta = -1;
  int i, j;
  if (q->count < ((flags & GET_USERINSTALLED_NAMEARCH) ? 4 : 2))
    return;
  if ((flags & GET_USERINSTALLED_NAMEARCH) != 0)
    solv_sort(q->elements, q->count / 2, 2 * sizeof(Id), get_userinstalled_cmp_namearch, pool);
  else if ((flags & GET_USERINSTALLED_NAMES) != 0)
    solv_sort(q->elements, q->count, sizeof(Id), get_userinstalled_cmp_names, pool);
  else
    solv_sort(q->elements, q->count, sizeof(Id), get_userinstalled_cmp, 0);
  if ((flags & GET_USERINSTALLED_NAMEARCH) != 0)
    {
      for (i = j = 0; i < q->count; i += 2)
	if (q->elements[i] != lastp || q->elements[i + 1] != lasta)
	  {
	    q->elements[j++] = lastp = q->elements[i];
	    q->elements[j++] = lasta = q->elements[i + 1];
	  }
    }
  else
    {
      for (i = j = 0; i < q->count; i++)
	if (q->elements[i] != lastp)
	  q->elements[j++] = lastp = q->elements[i];
    }
  queue_truncate(q, j);
}

static void
namearch2solvables(Pool *pool, Queue *q, Queue *qout, int job)
{
  int i;
  if (!pool->installed)
    return;
  for (i = 0; i < q->count; i += 2)
    {
      Id p, pp, name = q->elements[i], arch = q->elements[i + 1];
      FOR_PROVIDES(p, pp, name)
	{
	  Solvable *s = pool->solvables + p;
	  if (s->repo != pool->installed || s->name != name || (arch && s->arch != arch))
	    continue;
	  if (job)
	    queue_push(qout, job);
	  queue_push(qout, p);
	}
    }
}

void
solver_get_userinstalled(Solver *solv, Queue *q, int flags)
{
  Pool *pool = solv->pool;
  Id p, p2, pp;
  Solvable *s;
  Repo *installed = solv->installed;
  int i, j;
  Map userinstalled;
  
  map_init(&userinstalled, 0);
  queue_empty(q);
  /* first process jobs */
  for (i = 0; i < solv->job.count; i += 2)
    {
      Id how = solv->job.elements[i];
      Id what, select;
      if (installed && (how & SOLVER_JOBMASK) == SOLVER_USERINSTALLED)
	{
	  if (!userinstalled.size)
	    map_grow(&userinstalled, installed->end - installed->start);
	  what = solv->job.elements[i + 1];
	  select = how & SOLVER_SELECTMASK;
	  if (select == SOLVER_SOLVABLE_ALL || (select == SOLVER_SOLVABLE_REPO && what == installed->repoid))
	    {
	      FOR_REPO_SOLVABLES(installed, p, s)
	        MAPSET(&userinstalled, p - installed->start);
	    }
	  FOR_JOB_SELECT(p, pp, select, what)
	    if (pool->solvables[p].repo == installed)
	      MAPSET(&userinstalled, p - installed->start);
	  continue;
	}
      if ((how & SOLVER_JOBMASK) != SOLVER_INSTALL)
	continue;
      if ((how & SOLVER_NOTBYUSER) != 0)
	continue;
      what = solv->job.elements[i + 1];
      select = how & SOLVER_SELECTMASK;
      FOR_JOB_SELECT(p, pp, select, what)
        if (solv->decisionmap[p] > 0)
	  {
	    queue_push(q, p);
#ifdef ENABLE_LINKED_PKGS
	    if (has_package_link(pool, pool->solvables + p))
	      {
		int j;
		Queue lq;
		queue_init(&lq);
		find_package_link(pool, pool->solvables + p, 0, &lq, 0, 0);
		for (j = 0; j < lq.count; j++)
		  if (solv->decisionmap[lq.elements[j]] > 0)
		    queue_push(q, lq.elements[j]);
	      }
#endif
	  }
    }
  /* now process updates of userinstalled packages */
  if (installed && userinstalled.size)
    {
      for (i = 1; i < solv->decisionq.count; i++)
	{
	  p = solv->decisionq.elements[i];
	  if (p <= 0)
	    continue;
	  s = pool->solvables + p;
	  if (!s->repo)
	    continue;
	  if (s->repo == installed)
	    {
	      if (MAPTST(&userinstalled, p - installed->start))
		queue_push(q, p);
	      continue;
	    }
	  /* new package, check if we replace a userinstalled one */
	  FOR_PROVIDES(p2, pp, s->name)
	    {
	      Solvable *ps = pool->solvables + p2;
	      if (p2 == p || ps->repo != installed || !MAPTST(&userinstalled, p2 - installed->start))
		continue;
	      if (!pool->implicitobsoleteusesprovides && s->name != ps->name)
		continue;
	      if (pool->implicitobsoleteusescolors && !pool_colormatch(pool, s, ps))
		continue;
	      queue_push(q, p);
	      break;
	    }
	  if (!p2 && s->repo != installed && s->obsoletes)
	    {
	      Id obs, *obsp = s->repo->idarraydata + s->obsoletes;
	      while ((obs = *obsp++) != 0)
		{
		  FOR_PROVIDES(p2, pp, obs)
		    {
		      Solvable *ps = pool->solvables + p2;
		      if (p2 == p || ps->repo != installed || !MAPTST(&userinstalled, p2 - installed->start))
			continue;
		      if (!pool->obsoleteusesprovides && !pool_match_nevr(pool, ps, obs))
			continue;
		      if (pool->obsoleteusescolors && !pool_colormatch(pool, s, ps)) 
			continue;
		      queue_push(q, p); 
		      break;
		    }
		  if (p2)
		    break;
		}
	    }
	}
    }
  map_free(&userinstalled);

  /* convert to desired output format */
  if ((flags & GET_USERINSTALLED_NAMEARCH) != 0)
    {
      int qcount = q->count;
      queue_insertn(q, 0, qcount, 0);
      for (i = j = 0; i < qcount; i++)
	{
	  s = pool->solvables + q->elements[i + qcount];
	  q->elements[j++] = s->name;
	  q->elements[j++] = s->arch;
	}
    }
  else if ((flags & GET_USERINSTALLED_NAMES) != 0)
    {
      for (i = 0; i < q->count; i++)
	{
	  s = pool->solvables + q->elements[i];
	  q->elements[i] = s->name;
	}
    }
  /* sort and unify */
  get_userinstalled_sort_uniq(pool, q, flags);

  /* invert if asked for */
  if ((flags & GET_USERINSTALLED_INVERTED) != 0)
    {
      /* first generate queue with all installed packages */
      Queue invq;
      queue_init(&invq);
      for (i = 1; i < solv->decisionq.count; i++)
	{
	  p = solv->decisionq.elements[i];
	  if (p <= 0)
	    continue;
	  s = pool->solvables + p;
	  if (!s->repo)
	    continue;
	  if ((flags & GET_USERINSTALLED_NAMEARCH) != 0)
	    queue_push2(&invq, s->name, s->arch);
	  else if ((flags & GET_USERINSTALLED_NAMES) != 0)
	    queue_push(&invq, s->name);
	  else
	    queue_push(&invq, p);
	}
      /* push q on invq, just in case... */
      queue_insertn(&invq, invq.count, q->count, q->elements);
      get_userinstalled_sort_uniq(pool, &invq, flags);
      /* subtract queues (easy as they are sorted and invq is a superset of q) */
      if ((flags & GET_USERINSTALLED_NAMEARCH) != 0)
	{
	  if (q->count)
	    {
	      for (i = j = 0; i < invq.count; i += 2)
		if (invq.elements[i] == q->elements[j] && invq.elements[i + 1] == q->elements[j + 1])
		  {
		    invq.elements[i] = invq.elements[i + 1] = 0;
		    j += 2;
		    if (j >= q->count)
		      break;
		  }
	      queue_empty(q);
	    }
	  for (i = 0; i < invq.count; i += 2)
	    if (invq.elements[i])
	      queue_push2(q, invq.elements[i], invq.elements[i + 1]);
	}
      else
	{
	  if (q->count)
	    {
	      for (i = j = 0; i < invq.count; i++)
		if (invq.elements[i] == q->elements[j])
		  {
		    invq.elements[i] = 0;
		    if (++j >= q->count)
		      break;
		  }
	      queue_empty(q);
	    }
	  for (i = 0; i < invq.count; i++)
	    if (invq.elements[i])
	      queue_push(q, invq.elements[i]);
	}
      queue_free(&invq);
    }
}

void
pool_add_userinstalled_jobs(Pool *pool, Queue *q, Queue *job, int flags)
{
  int i;

  if ((flags & GET_USERINSTALLED_INVERTED) != 0)
    {
      Queue invq;
      Id p, lastid;
      Solvable *s;
      int bad;
      if (!pool->installed)
	return;
      queue_init(&invq);
      if ((flags & GET_USERINSTALLED_NAMEARCH) != 0)
	flags &= ~GET_USERINSTALLED_NAMES;	/* just in case */
      FOR_REPO_SOLVABLES(pool->installed, p, s)
	queue_push(&invq, flags & GET_USERINSTALLED_NAMES ? s->name : p);
      if ((flags & GET_USERINSTALLED_NAMEARCH) != 0)
	{
	  /* for namearch we convert to packages */
	  namearch2solvables(pool, q, &invq, 0);
	  get_userinstalled_sort_uniq(pool, &invq, flags);
	  namearch2solvables(pool, q, &invq, 0);
	  flags = 0;
	}
      else
	{
	  queue_insertn(&invq, invq.count, q->count, q->elements);
	  get_userinstalled_sort_uniq(pool, &invq, flags);
	  /* now the fun part, add q again, sort, and remove all dups */
	  queue_insertn(&invq, invq.count, q->count, q->elements);
	}
      if (invq.count > 1)
	{
	  if ((flags & GET_USERINSTALLED_NAMES) != 0)
	    solv_sort(invq.elements, invq.count, sizeof(Id), get_userinstalled_cmp_names, pool);
	  else
	    solv_sort(invq.elements, invq.count, sizeof(Id), get_userinstalled_cmp, 0);
	}
      lastid = -1;
      bad = 1;
      for (i = 0; i < invq.count; i++)
	{
	  if (invq.elements[i] == lastid)
	    {
	      bad = 1;
	      continue;
	    }
	  if (!bad)
	    queue_push2(job, SOLVER_USERINSTALLED | (flags & GET_USERINSTALLED_NAMES ? SOLVER_SOLVABLE_NAME : SOLVER_SOLVABLE), lastid);
	  bad = 0;
	  lastid = invq.elements[i];
	}
      if (!bad)
	queue_push2(job, SOLVER_USERINSTALLED | (flags & GET_USERINSTALLED_NAMES ? SOLVER_SOLVABLE_NAME : SOLVER_SOLVABLE), lastid);
      queue_free(&invq);
    }
  else
    {
      if (flags & GET_USERINSTALLED_NAMEARCH)
	namearch2solvables(pool, q, job, SOLVER_USERINSTALLED | SOLVER_SOLVABLE);
      else
	{
	  for (i = 0; i < q->count; i++)
	    queue_push2(job, SOLVER_USERINSTALLED | (flags & GET_USERINSTALLED_NAMES ? SOLVER_SOLVABLE_NAME : SOLVER_SOLVABLE), q->elements[i]);
	}
    }
}

