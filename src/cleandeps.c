/*
 * Copyright (c) 2017, SUSE Inc.
 *
 * This program is licensed under the BSD license, read LICENSE.BSD
 * for further information
 */

/*
 * cleandeps.c
 *
 * code to find and erase unneeded packages
 */

#include <stdio.h>
#include <stdlib.h>
#include <unistd.h>
#include <string.h>

#include "solver.h"
#include "solverdebug.h"
#include "solver_private.h"
#include "bitmap.h"
#include "pool.h"
#include "util.h"
#include "policy.h"
#include "cplxdeps.h"

#undef CLEANDEPSDEBUG

/*
 * This functions collects all packages that are looked at
 * when a dependency is checked. We need it to "pin" installed
 * packages when removing a supplemented package in createcleandepsmap.
 * Here's an not uncommon example:
 *   A contains "Supplements: packageand(B, C)"
 *   B contains "Requires: A"
 * Now if we remove C, the supplements is no longer true,
 * thus we also remove A. Without the dep_pkgcheck function, we
 * would now also remove B, but this is wrong, as adding back
 * C doesn't make the supplements true again. Thus we "pin" B
 * when we remove A.
 * There's probably a better way to do this, but I haven't come
 * up with it yet ;)
 */
static void
dep_pkgcheck_slow(Solver *solv, Id dep, Map *m, Queue *q)
{
  Pool *pool = solv->pool;
  Id p, pp;

  if (ISRELDEP(dep))
    {
      Reldep *rd = GETRELDEP(pool, dep);
      if (rd->flags >= 8)
	{
	  if (rd->flags == REL_AND)
	    {
	      dep_pkgcheck_slow(solv, rd->name, m, q);
	      dep_pkgcheck_slow(solv, rd->evr, m, q);
	      return;
	    }
	  if (rd->flags == REL_COND || rd->flags == REL_UNLESS)
	    {
	      dep_pkgcheck_slow(solv, rd->name, m, q);
	      if (ISRELDEP(rd->evr))
		{
		  Reldep *rd2 = GETRELDEP(pool, rd->evr);
		  if (rd2->flags == REL_ELSE)
		    dep_pkgcheck_slow(solv, rd2->evr, m, q);
		}
	      return;
	    }
	  if (rd->flags == REL_NAMESPACE && rd->name == NAMESPACE_SPLITPROVIDES)
	    return;
	}
    }
  FOR_PROVIDES(p, pp, dep)
    if (!m || MAPTST(m, p))
      queue_push(q, p);
}

static inline void
dep_pkgcheck(Solver *solv, Id dep, Map *m, Queue *q)
{
  Pool *pool = solv->pool;
  Id p, pp;

  if (!ISSIMPLEDEP(pool, dep))
    {
      dep_pkgcheck_slow(solv, dep, m, q);
      return;
    }
  FOR_PROVIDES(p, pp, dep)
    if (!m || MAPTST(m, p))
      queue_push(q, p);
}

static int
check_xsupp(Solver *solv, Queue *depq, Id dep)
{
  Pool *pool = solv->pool;
  Id p, pp;

  if (ISRELDEP(dep))
    {
      Reldep *rd = GETRELDEP(pool, dep);
      if (rd->flags >= 8)
	{
	  if (rd->flags == REL_AND)
	    {
	      if (!check_xsupp(solv, depq, rd->name))
		return 0;
	      return check_xsupp(solv, depq, rd->evr);
	    }
	  if (rd->flags == REL_OR)
	    {
	      if (check_xsupp(solv, depq, rd->name))
		return 1;
	      return check_xsupp(solv, depq, rd->evr);
	    }
	  if (rd->flags == REL_NAMESPACE && rd->name == NAMESPACE_SPLITPROVIDES)
	    return 0;
	  if (depq && rd->flags == REL_NAMESPACE)
	    {
	      int i;
	      for (i = 0; i < depq->count; i++)
		if (depq->elements[i] == dep || depq->elements[i] == rd->name)
		 return 1;
	    }
	}
    }
  FOR_PROVIDES(p, pp, dep)
    if (p == SYSTEMSOLVABLE || pool->solvables[p].repo == solv->installed)
      return 1;
  return 0;
}

struct trj_data {
  Queue *edges;
  Id *low;
  Id idx;
  Id nstack;
  Id firstidx;
};

/* Tarjan's SCC algorithm, slightly modifed */
static void
trj_visit(struct trj_data *trj, Id node)
{
  Id *low = trj->low;
  Queue *edges = trj->edges;
  Id nnode, myidx, stackstart;
  int i;

  low[node] = myidx = trj->idx++;
  low[(stackstart = trj->nstack++)] = node;
  for (i = edges->elements[node]; (nnode = edges->elements[i]) != 0; i++)
    {
      Id l = low[nnode];
      if (!l)
	{
	  if (!edges->elements[edges->elements[nnode]])
	    {
	      trj->idx++;
	      low[nnode] = -1;
	      continue;
	    }
	  trj_visit(trj, nnode);
	  l = low[nnode];
	}
      if (l < 0)
	continue;
      if (l < trj->firstidx)
	{
	  int k;
	  for (k = l; low[low[k]] == l; k++)
	    low[low[k]] = -1;
	}
      else if (l < low[node])
	low[node] = l;
    }
  if (low[node] == myidx)
    {
      if (myidx != trj->firstidx)
	myidx = -1;
      for (i = stackstart; i < trj->nstack; i++)
	low[low[i]] = myidx;
      trj->nstack = stackstart;
    }
}

#ifdef ENABLE_COMPLEX_DEPS
static void
complex_filter_unneeded(Pool *pool, Id ip, Id req, Queue *edges, Map *cleandepsmap, Queue *unneededq)
{
  int i, j;
  Queue dq;
  Id p;

  queue_init(&dq);
  i = pool_normalize_complex_dep(pool, req, &dq, CPLXDEPS_EXPAND);
  if (i == 0 || i == 1)
    {
      queue_free(&dq);
      return;
    }
  for (i = 0; i < dq.count; i++)
    {
      for (; (p = dq.elements[i]) != 0; i++)
	{
	  if (p < 0)
	    {
	      if (pool->solvables[-p].repo != pool->installed)
	        break;
	      continue;
	    }
	  if (p == ip || pool->solvables[p].repo != pool->installed || !MAPTST(cleandepsmap, p - pool->installed->start))
	    continue;
	  for (j = 0; j < unneededq->count; j++)
	    if (p == unneededq->elements[j])
	      {
		if (edges->elements[edges->count - 1] != j + 1)
		  queue_push(edges, j + 1);
	        break;
	      }
	}
      while (dq.elements[i])
	i++;
    }
  queue_free(&dq);
}
#endif


static void
filter_unneeded(Solver *solv, Queue *unneededq, Map *unneededmap, int justone)
{
  Pool *pool = solv->pool;
  Repo *installed = solv->installed;
  Queue edges;
  Id *nrequires;
  Map m, installedm;
  int i, j, pass, count = unneededq->count;
  Id *low;

  if (unneededq->count < 2)
    return;
  map_init(&m, 0);
  if (!unneededmap)
    {
      unneededmap = &m;
      map_grow(unneededmap, installed->end - installed->start);
      for (i = 0; i < count; i++)
	MAPSET(unneededmap, unneededq->elements[i] - installed->start);
    }
  map_init(&installedm, pool->nsolvables);
  for (i = installed->start; i < installed->end; i++)
    if (pool->solvables[i].repo == installed)
      MAPSET(&installedm, i);

  nrequires = solv_calloc(count, sizeof(Id));
  queue_init(&edges);
  queue_prealloc(&edges, count * 4 + 10);	/* pre-size */

  /*
   * Go through the solvables in the nodes queue and create edges for
   * all requires/recommends/supplements between the nodes.
   * The edges are stored in the edges queue, we add 1 to the node
   * index so that nodes in the edges queue are != 0 and we can
   * terminate the edge list with 0.
   * Thus for node element 5, the edges are stored starting at
   * edges.elements[6] and are 0-terminated.
   */
  /* leave first element zero to make things easier */
  /* also add trailing zero */
  queue_insertn(&edges, 0, 1 + count + 1, 0);

  /* first requires and recommends */
  for (i = 0; i < count; i++)
    {
      Solvable *s = pool->solvables + unneededq->elements[i];
      int oldcount = edges.count;
      edges.elements[i + 1] = oldcount;
      for (pass = 0; pass < 2; pass++)
	{
	  unsigned int off = pass == 0 ? s->requires : s->recommends;
	  Id p, pp, dep, *dp;
	  if (off)
	    for (dp = s->repo->idarraydata + off; (dep = *dp) != 0; dp++)
	      {
#ifdef ENABLE_COMPLEX_DEPS
		if (pool_is_complex_dep(pool, dep))
		  {
		    complex_filter_unneeded(pool, s - pool->solvables, dep, &edges, unneededmap, unneededq);
		    continue;
		  }
#endif
		if (justone)
		  {
		    int count = 0;
		    FOR_PROVIDES(p, pp, dep)
		      {
			Solvable *sp = pool->solvables + p;
			if (s == sp || sp->repo != installed)
			  continue;
			count++;
		      }
		    if (count != 1)
		      continue;
		  }
		FOR_PROVIDES(p, pp, dep)
		  {
		    Solvable *sp = pool->solvables + p;
		    if (s == sp || sp->repo != installed || !MAPTST(unneededmap, p - installed->start))
		      continue;
		    for (j = 0; j < count; j++)
		      if (p == unneededq->elements[j])
			{
			  if (edges.elements[edges.count - 1] != j + 1)
			    queue_push(&edges, j + 1);
			}
		  }
	      }
	  if (pass == 0)
	    nrequires[i] = edges.count - oldcount;
	}
      queue_push(&edges, 0);
    }
#if 0
  printf("requires + recommends\n");
  for (i = 0; i < count; i++)
    {
      int j;
      printf("  %s (%d requires):\n", pool_solvid2str(pool, unneededq->elements[i]), nrequires[i]);
      for (j = edges.elements[i + 1]; edges.elements[j]; j++)
	printf("    - %s\n", pool_solvid2str(pool, unneededq->elements[edges.elements[j] - 1]));
    }
#endif

  /* then add supplements */
  for (i = 0; i < count; i++)
    {
      Solvable *s = pool->solvables + unneededq->elements[i];
      if (s->supplements)
	{
	  Id *dp;
	  int k;
	  for (dp = s->repo->idarraydata + s->supplements; *dp; dp++)
	    if (solver_dep_possible(solv, *dp, &installedm))
	      {
		Queue iq;
		Id iqbuf[16];
		queue_init_buffer(&iq, iqbuf, sizeof(iqbuf)/sizeof(*iqbuf));
		dep_pkgcheck(solv, *dp, 0, &iq);
		if (justone && iq.count != 1)
		  {
		    queue_free(&iq);
		    continue;
		  }
		for (k = 0; k < iq.count; k++)
		  {
		    Id p = iq.elements[k];
		    Solvable *sp = pool->solvables + p;
		    if (p == unneededq->elements[i] || sp->repo != installed || !MAPTST(unneededmap, p - installed->start))
		      continue;
		    for (j = 0; j < count; j++)
		      if (p == unneededq->elements[j])
			break;
		    /* now add edge from j + 1 to i + 1 */
		    queue_insert(&edges, edges.elements[j + 1] + nrequires[j], i + 1);
		    /* addapt following edge pointers */
		    for (j = j + 2; j < count + 1; j++)
		      edges.elements[j]++;
		  }
		queue_free(&iq);
	      }
	}
    }
#if 0
  /* print result */
  printf("+ supplements\n");
  for (i = 0; i < count; i++)
    {
      int j;
      printf("  %s (%d requires):\n", pool_solvid2str(pool, unneededq->elements[i]), nrequires[i]);
      for (j = edges.elements[i + 1]; edges.elements[j]; j++)
	printf("    - %s\n", pool_solvid2str(pool, unneededq->elements[edges.elements[j] - 1]));
    }
#endif
  map_free(&installedm);

  /* now run SCC algo two times, first with requires+recommends+supplements,
   * then again without the requires. We run it the second time to get rid
   * of packages that got dragged in via recommends/supplements */
  /*
   * low will contain the result of the SCC search.
   * it must be of at least size 2 * (count + 1) and
   * must be zero initialized.
   * The layout is:
   *    0  low low ... low stack stack ...stack 0
   *            count              count
   */
  low = solv_calloc(count + 1, 2 * sizeof(Id));
  for (pass = 0; pass < 2; pass++)
    {
      struct trj_data trj;
      if (pass)
	{
	  memset(low, 0, (count + 1) * (2 * sizeof(Id)));
	  for (i = 0; i < count; i++)
	    {
	      edges.elements[i + 1] += nrequires[i];
	      if (!unneededq->elements[i])
		low[i + 1] = -1;	/* ignore this node */
	    }
	}
      trj.edges = &edges;
      trj.low = low;
      trj.idx = count + 1;	/* stack starts here */
      for (i = 1; i <= count; i++)
	{
	  if (low[i])
	    continue;
	  if (edges.elements[edges.elements[i]])
	    {
	      trj.firstidx = trj.nstack = trj.idx;
	      trj_visit(&trj, i);
	    }
	  else
	    {
	      Id myidx = trj.idx++;
	      low[i] = myidx;
	      low[myidx] = i;
	    }
	}
      /* prune packages */
      for (i = 0; i < count; i++)
	if (low[i + 1] <= 0)
	  unneededq->elements[i] = 0;
    }
  solv_free(low);
  solv_free(nrequires);
  queue_free(&edges);

  /* finally remove all pruned entries from unneededq */
  for (i = j = 0; i < count; i++)
    if (unneededq->elements[i])
      unneededq->elements[j++] = unneededq->elements[i];
  queue_truncate(unneededq, j);
  map_free(&m);
}


#ifdef ENABLE_COMPLEX_DEPS
static void
complex_cleandeps_remove(Pool *pool, Id ip, Id req, Map *im, Map *installedm, Queue *iq)
{
  int i;
  Queue dq;
  Id p;

  queue_init(&dq);
  i = pool_normalize_complex_dep(pool, req, &dq, CPLXDEPS_EXPAND);
  if (i == 0 || i == 1)
    {
      queue_free(&dq);
      return;
    }
  for (i = 0; i < dq.count; i++)
    {
      for (; (p = dq.elements[i]) != 0; i++)
	{
	  if (p < 0)
	    {
	      if (!MAPTST(installedm, -p))
	        break;
	      continue;
	    }
	  if (p != SYSTEMSOLVABLE && MAPTST(im, p))
	    {
#ifdef CLEANDEPSDEBUG
	      printf("%s requires/recommends %s\n", pool_solvid2str(pool, ip), pool_solvid2str(pool, p));
#endif
	      queue_push(iq, p);
	    }
	}
      while (dq.elements[i])
	i++;
    }
  queue_free(&dq);
}

static void
complex_cleandeps_addback(Pool *pool, Id ip, Id req, Map *im, Map *installedm, Queue *iq, Map *userinstalled)
{
  int i, blk;
  Queue dq;
  Id p;

  queue_init(&dq);
  i = pool_normalize_complex_dep(pool, req, &dq, CPLXDEPS_EXPAND);
  if (i == 0 || i == 1)
    {
      queue_free(&dq);
      return;
    }
  for (i = 0; i < dq.count; i++)
    {
      blk = i;
      for (; (p = dq.elements[i]) != 0; i++)
	{
	  if (p < 0)
	    {
	      if (!MAPTST(installedm, -p))
	        break;
	    }
	  else if (p == ip)
	    break;
	}
      if (!p)
	{
	  for (i = blk; (p = dq.elements[i]) != 0; i++)
	    {
	      if (p < 0)
		continue;
	      if (MAPTST(im, p))
		continue;
	      if (!MAPTST(installedm, p))
		continue;
	      if (p == ip || MAPTST(userinstalled, p - pool->installed->start))
		continue;
#ifdef CLEANDEPSDEBUG
	      printf("%s requires/recommends %s\n", pool_solvid2str(pool, ip), pool_solvid2str(pool, p));
#endif
	      MAPSET(im, p);
	      queue_push(iq, p);
	    }
	}
      while (dq.elements[i])
	i++;
    }
  queue_free(&dq);
}

#endif

static inline int
queue_contains(Queue *q, Id id)
{
  int i;
  for (i = 0; i < q->count; i++)
    if (q->elements[i] == id)
      return 1;
  return 0;
}

static void
find_update_seeds(Solver *solv, Queue *updatepkgs_filtered, Map *userinstalled)
{
  Pool *pool = solv->pool;
  Repo *installed = solv->installed;
  Queue *cleandeps_updatepkgs = solv->cleandeps_updatepkgs;
  int i, j;
  Id p;

  queue_prealloc(updatepkgs_filtered, cleandeps_updatepkgs->count);
  for (i = 0; i < cleandeps_updatepkgs->count; i++)
    {
      p = cleandeps_updatepkgs->elements[i];
      if (pool->solvables[p].repo == installed)
	{
#ifdef ENABLE_LINKED_PKGS
	  const char *name = pool_id2str(pool, pool->solvables[p].name);
	  if (strncmp(name, "pattern:", 8) == 0 || strncmp(name, "application:", 12) == 0)
	    continue;
#endif
	  queue_push(updatepkgs_filtered, p);
	}
    }
#ifdef CLEANDEPSDEBUG
  printf("SEEDS IN (%d)\n", updatepkgs_filtered->count);
  for (i = 0; i < updatepkgs_filtered->count; i++)
    printf("  - %s\n", pool_solvid2str(pool, updatepkgs_filtered->elements[i]));
#endif
  filter_unneeded(solv, updatepkgs_filtered, 0, 1);
#ifdef CLEANDEPSDEBUG
  printf("SEEDS OUT (%d)\n", updatepkgs_filtered->count);
  for (i = 0; i < updatepkgs_filtered->count; i++)
    printf("  - %s\n", pool_solvid2str(pool, updatepkgs_filtered->elements[i]));
#endif
  /* make sure userinstalled packages are in the seeds */
  for (i = j = 0; i < updatepkgs_filtered->count; i++)
    {
      p = updatepkgs_filtered->elements[i];
      if (!MAPTST(userinstalled, p - installed->start))
	updatepkgs_filtered->elements[j++] = p;
    }
  queue_truncate(updatepkgs_filtered, j);
  for (i = 0; i < cleandeps_updatepkgs->count; i++)
    {
      p = cleandeps_updatepkgs->elements[i];
      if (pool->solvables[p].repo == installed)
	{
#ifdef ENABLE_LINKED_PKGS
	  const char *name = pool_id2str(pool, pool->solvables[p].name);
	  if (strncmp(name, "pattern:", 8) == 0 || strncmp(name, "application:", 12) == 0)
	    {
	      queue_push(updatepkgs_filtered, p);
	      continue;
	    }
#endif
	  if (MAPTST(userinstalled, p - installed->start))
	    queue_push(updatepkgs_filtered, p);
	}
    }
#ifdef CLEANDEPSDEBUG
  printf("SEEDS FINAL\n");
  for (i = 0; i < updatepkgs_filtered->count; i++)
    printf("  - %s\n", pool_solvid2str(pool, updatepkgs_filtered->elements[i]));
#endif
}

/*
 * Find all installed packages that are no longer
 * needed regarding the current solver job.
 *
 * The algorithm is:
 * - remove pass: remove all packages that could have
 *   been dragged in by the obsoleted packages.
 *   i.e. if package A is obsolete and contains "Requires: B",
 *   also remove B, as installing A will have pulled in B.
 *   after this pass, we have a set of still installed packages
 *   with broken dependencies.
 * - add back pass:
 *   now add back all packages that the still installed packages
 *   require.
 *
 * The cleandeps packages are the packages removed in the first
 * pass and not added back in the second pass.
 *
 * If we search for unneeded packages (unneeded is true), we
 * simply remove all packages except the userinstalled ones in
 * the first pass.
 */
void
solver_createcleandepsmap(Solver *solv, Map *cleandepsmap, int unneeded)
{
  Pool *pool = solv->pool;
  Repo *installed = solv->installed;
  Queue *job = &solv->job;
  Map userinstalled;
  Map im;
  Map installedm;
  Rule *r;
  Id rid, how, what, select;
  Id p, pp, ip, jp;
  Id req, *reqp, sup, *supp;
  Solvable *s;
  Queue iq, iqcopy, xsuppq;
  Queue updatepkgs_filtered;
  int i;

  map_empty(cleandepsmap);
  if (!installed || installed->end == installed->start)
    return;
  map_init(&userinstalled, installed->end - installed->start);
  map_init(&im, pool->nsolvables);
  map_init(&installedm, pool->nsolvables);
  queue_init(&iq);
  queue_init(&xsuppq);

  for (i = 0; i < job->count; i += 2)
    {
      how = job->elements[i];
      if ((how & SOLVER_JOBMASK) == SOLVER_USERINSTALLED)
	{
	  what = job->elements[i + 1];
	  select = how & SOLVER_SELECTMASK;
	  if (select == SOLVER_SOLVABLE_ALL || (select == SOLVER_SOLVABLE_REPO && what == installed->repoid))
	    {
	      FOR_REPO_SOLVABLES(installed, p, s)
	        MAPSET(&userinstalled, p - installed->start);
	    }
	  FOR_JOB_SELECT(p, pp, select, what)
	    if (pool->solvables[p].repo == installed)
	      MAPSET(&userinstalled, p - installed->start);
	}
      if ((how & (SOLVER_JOBMASK | SOLVER_SELECTMASK)) == (SOLVER_ERASE | SOLVER_SOLVABLE_PROVIDES))
	{
	  what = job->elements[i + 1];
	  if (ISRELDEP(what))
	    {
	      Reldep *rd = GETRELDEP(pool, what);
	      if (rd->flags != REL_NAMESPACE)
		continue;
	      if (rd->evr == 0)
		{
		  queue_pushunique(&iq, rd->name);
		  continue;
		}
	      FOR_PROVIDES(p, pp, what)
		if (p)
		  break;
	      if (p)
		continue;
	      queue_pushunique(&iq, what);
	    }
	}
    }

  /* have special namespace cleandeps erases */
  if (iq.count)
    {
      for (ip = installed->start; ip < installed->end; ip++)
	{
	  s = pool->solvables + ip;
	  if (s->repo != installed)
	    continue;
	  if (!s->supplements)
	    continue;
	  supp = s->repo->idarraydata + s->supplements;
	  while ((sup = *supp++) != 0)
	    if (ISRELDEP(sup) && check_xsupp(solv, &iq, sup) && !check_xsupp(solv, 0, sup))
	      {
#ifdef CLEANDEPSDEBUG
		printf("xsupp %s from %s\n", pool_dep2str(pool, sup), pool_solvid2str(pool, ip));
#endif
	        queue_pushunique(&xsuppq, sup);
	      }
	}
      queue_empty(&iq);
    }

  /* also add visible patterns to userinstalled for openSUSE */
  if (1)
    {
      Dataiterator di;
      dataiterator_init(&di, pool, 0, 0, SOLVABLE_ISVISIBLE, 0, 0);
      while (dataiterator_step(&di))
	{
	  Id *dp;
	  if (di.solvid <= 0)
	    continue;
	  s = pool->solvables + di.solvid;
	  if (!s->repo || !s->requires)
	    continue;
	  if (s->repo != installed && !pool_installable(pool, s))
	    continue;
	  if (strncmp(pool_id2str(pool, s->name), "pattern:", 8) != 0)
	    continue;
	  dp = s->repo->idarraydata + s->requires;
	  for (dp = s->repo->idarraydata + s->requires; *dp; dp++)
	    FOR_PROVIDES(p, pp, *dp)
	      if (pool->solvables[p].repo == installed)
		{
		  if (strncmp(pool_id2str(pool, pool->solvables[p].name), "pattern", 7) != 0)
		    continue;
		  MAPSET(&userinstalled, p - installed->start);
		}
	}
      dataiterator_free(&di);
    }
  if (1)
    {
      /* all products and their buddies are userinstalled */
      for (p = installed->start; p < installed->end; p++)
	{
	  Solvable *s = pool->solvables + p;
	  if (s->repo != installed)
	    continue;
	  if (!strncmp("product:", pool_id2str(pool, s->name), 8))
	    {
	      MAPSET(&userinstalled, p - installed->start);
#ifdef ENABLE_LINKED_PKGS
	      if (solv->instbuddy && solv->instbuddy[p - installed->start] > 1)
		{
		  Id buddy = solv->instbuddy[p - installed->start];
		  if (buddy >= installed->start && buddy < installed->end)
		    MAPSET(&userinstalled, buddy - installed->start);
		}
#endif
	    }
	}
    }

  /* add all positive elements (e.g. locks) to "userinstalled" */
  for (rid = solv->jobrules; rid < solv->jobrules_end; rid++)
    {
      r = solv->rules + rid;
      if (r->d < 0)
	continue;
      i = solv->ruletojob.elements[rid - solv->jobrules];
      if ((job->elements[i] & SOLVER_CLEANDEPS) == SOLVER_CLEANDEPS)
	continue;
      FOR_RULELITERALS(p, jp, r)
	if (p > 0 && pool->solvables[p].repo == installed)
	  MAPSET(&userinstalled, p - installed->start);
    }

  /* add all cleandeps candidates to iq */
  for (rid = solv->jobrules; rid < solv->jobrules_end; rid++)
    {
      r = solv->rules + rid;
      if (r->d < 0)				/* disabled? */
	continue;
      if (r->d == 0 && r->p < 0 && r->w2 == 0)	/* negative assertion (erase job)? */
	{
	  p = -r->p;
	  if (pool->solvables[p].repo != installed)
	    continue;
	  MAPCLR(&userinstalled, p - installed->start);
	  if (unneeded)
	    continue;
	  i = solv->ruletojob.elements[rid - solv->jobrules];
	  how = job->elements[i];
	  if ((how & (SOLVER_JOBMASK|SOLVER_CLEANDEPS)) == (SOLVER_ERASE|SOLVER_CLEANDEPS))
	    queue_push(&iq, p);
	}
      else if (r->p > 0)			/* install job */
	{
	  if (unneeded)
	    continue;
	  i = solv->ruletojob.elements[rid - solv->jobrules];
	  if ((job->elements[i] & SOLVER_CLEANDEPS) == SOLVER_CLEANDEPS)
	    {
	      /* check if the literals all obsolete some installed package */
	      Map om;
	      int iqstart;

	      /* just one installed literal */
	      if (r->d == 0 && r->w2 == 0 && pool->solvables[r->p].repo == installed)
		continue;
	      /* multiversion is bad */
	      if (solv->multiversion.size && !solv->keepexplicitobsoletes)
		{
		  FOR_RULELITERALS(p, jp, r)
		    if (MAPTST(&solv->multiversion, p))
		      break;
		  if (p)
		    continue;
		}

	      om.size = 0;
	      iqstart = iq.count;
	      FOR_RULELITERALS(p, jp, r)
		{
		  if (p < 0)
		    {
		      queue_truncate(&iq, iqstart);	/* abort */
		      break;
		    }
		  if (pool->solvables[p].repo == installed)
		    {
		      if (iq.count == iqstart)
			queue_push(&iq, p);
		      else
			{
			  for (i = iqstart; i < iq.count; i++)
			    if (iq.elements[i] == p)
			      break;
			  queue_truncate(&iq, iqstart);
			  if (i < iq.count)
			    queue_push(&iq, p);
			}
		    }
		  else
		    solver_intersect_obsoleted(solv, p, &iq, iqstart, &om);
		  if (iq.count == iqstart)
		    break;
		}
	      if (om.size)
	        map_free(&om);
	    }
	}
    }
  queue_init_clone(&iqcopy, &iq);

  if (!unneeded)
    {
      if (solv->cleandeps_updatepkgs)
	for (i = 0; i < solv->cleandeps_updatepkgs->count; i++)
	  queue_push(&iq, solv->cleandeps_updatepkgs->elements[i]);
    }

  if (unneeded)
    queue_empty(&iq);	/* just in case... */

  /* clear userinstalled bit for the packages we really want to delete/update */
  for (i = 0; i < iq.count; i++)
    {
      p = iq.elements[i];
      if (pool->solvables[p].repo != installed)
	continue;
      MAPCLR(&userinstalled, p - installed->start);
    }

  for (p = installed->start; p < installed->end; p++)
    {
      if (pool->solvables[p].repo != installed)
	continue;
      MAPSET(&installedm, p);
      if (pool->considered && !MAPTST(pool->considered, p))
	MAPSET(&userinstalled, p - installed->start);	/* we may not remove those */
      if (unneeded && !MAPTST(&userinstalled, p - installed->start))
	continue;
      MAPSET(&im, p);
    }
  MAPSET(&installedm, SYSTEMSOLVABLE);
  MAPSET(&im, SYSTEMSOLVABLE);

  if (!unneeded && solv->cleandeps_updatepkgs)
    {
      /* find update "seeds" */
      queue_init(&updatepkgs_filtered);
      find_update_seeds(solv, &updatepkgs_filtered, &userinstalled);
    }

#ifdef CLEANDEPSDEBUG
  printf("REMOVE PASS\n");
#endif

  for (;;)
    {
      if (!iq.count)
	{
	  if (unneeded)
	    break;
	  /* supplements pass */
	  for (ip = installed->start; ip < installed->end; ip++)
	    {
	      if (!MAPTST(&installedm, ip))
		continue;
	      s = pool->solvables + ip;
	      if (!s->supplements)
		continue;
	      if (!MAPTST(&im, ip))
		continue;
	      if (MAPTST(&userinstalled, ip - installed->start))
		continue;
	      supp = s->repo->idarraydata + s->supplements;
	      while ((sup = *supp++) != 0)
		if (solver_dep_possible(solv, sup, &im))
		  break;
	      if (!sup)
		{
		  supp = s->repo->idarraydata + s->supplements;
		  while ((sup = *supp++) != 0)
		    if (solver_dep_possible(solv, sup, &installedm) || (xsuppq.count && queue_contains(&xsuppq, sup)))
		      {
		        /* no longer supplemented, also erase */
			int iqcount = iq.count;
			/* pin packages, see comment above dep_pkgcheck */
			dep_pkgcheck(solv, sup, &im, &iq);
			for (i = iqcount; i < iq.count; i++)
			  {
			    Id pqp = iq.elements[i];
			    if (pool->solvables[pqp].repo == installed)
			      MAPSET(&userinstalled, pqp - installed->start);
			  }
			queue_truncate(&iq, iqcount);
#ifdef CLEANDEPSDEBUG
		        printf("%s supplemented [%s]\n", pool_solvid2str(pool, ip), pool_dep2str(pool, sup));
#endif
		        queue_push(&iq, ip);
		      }
		}
	    }
	  if (!iq.count)
	    break;	/* no supplementing package found, we're done */
	}
      ip = queue_shift(&iq);
      s = pool->solvables + ip;
      if (!MAPTST(&im, ip))
	continue;
      if (!MAPTST(&installedm, ip))
	continue;
      if (s->repo == installed && MAPTST(&userinstalled, ip - installed->start))
	continue;
      MAPCLR(&im, ip);
#ifdef CLEANDEPSDEBUG
      printf("removing %s\n", pool_solvable2str(pool, s));
#endif
      if (s->requires)
	{
	  reqp = s->repo->idarraydata + s->requires;
	  while ((req = *reqp++) != 0)
	    {
	      if (req == SOLVABLE_PREREQMARKER)
		continue;
#ifdef ENABLE_COMPLEX_DEPS
	      if (pool_is_complex_dep(pool, req))
		{
		  complex_cleandeps_remove(pool, ip, req, &im, &installedm, &iq);
		  continue;
		}
#endif
	      FOR_PROVIDES(p, pp, req)
		{
		  if (p != SYSTEMSOLVABLE && MAPTST(&im, p))
		    {
#ifdef CLEANDEPSDEBUG
		      printf("%s requires %s\n", pool_solvid2str(pool, ip), pool_solvid2str(pool, p));
#endif
		      queue_push(&iq, p);
		    }
		}
	    }
	}
      if (s->recommends)
	{
	  reqp = s->repo->idarraydata + s->recommends;
	  while ((req = *reqp++) != 0)
	    {
#ifdef ENABLE_COMPLEX_DEPS
	      if (pool_is_complex_dep(pool, req))
		{
		  complex_cleandeps_remove(pool, ip, req, &im, &installedm, &iq);
		  continue;
		}
#endif
	      FOR_PROVIDES(p, pp, req)
		{
		  if (p != SYSTEMSOLVABLE && MAPTST(&im, p))
		    {
#ifdef CLEANDEPSDEBUG
		      printf("%s recommends %s\n", pool_solvid2str(pool, ip), pool_solvid2str(pool, p));
#endif
		      queue_push(&iq, p);
		    }
		}
	    }
	}
    }

  /* turn userinstalled into remove set for pruning */
  map_empty(&userinstalled);
  for (rid = solv->jobrules; rid < solv->jobrules_end; rid++)
    {
      r = solv->rules + rid;
      if (r->p >= 0 || r->d != 0 || r->w2 != 0)
	continue;	/* disabled or not erase */
      p = -r->p;
      MAPCLR(&im, p);
      if (pool->solvables[p].repo == installed)
        MAPSET(&userinstalled, p - installed->start);
    }
  if (!unneeded && solv->cleandeps_updatepkgs)
    {
      for (i = 0; i < solv->cleandeps_updatepkgs->count; i++)
	{
	  p = solv->cleandeps_updatepkgs->elements[i];
	  if (pool->solvables[p].repo == installed)
	    MAPSET(&userinstalled, p - installed->start);
	}
    }
  MAPSET(&im, SYSTEMSOLVABLE);	/* in case we cleared it above */
  for (p = installed->start; p < installed->end; p++)
    if (MAPTST(&im, p))
      queue_push(&iq, p);
  for (rid = solv->jobrules; rid < solv->jobrules_end; rid++)
    {
      r = solv->rules + rid;
      if (r->d < 0)
	continue;
      FOR_RULELITERALS(p, jp, r)
	if (p > 0)
          queue_push(&iq, p);
    }
  /* also put directly addressed packages on the install queue
   * so we can mark patterns as installed */
  for (i = 0; i < job->count; i += 2)
    {
      how = job->elements[i];
      if ((how & SOLVER_JOBMASK) == SOLVER_USERINSTALLED)
	{
	  what = job->elements[i + 1];
	  select = how & SOLVER_SELECTMASK;
	  if (select == SOLVER_SOLVABLE && pool->solvables[what].repo != installed)
            queue_push(&iq, what);
	}
    }

#ifdef CLEANDEPSDEBUG
  printf("ADDBACK PASS\n");
#endif
  for (;;)
    {
      if (!iq.count)
	{
	  /* supplements pass */
	  for (ip = installed->start; ip < installed->end; ip++)
	    {
	      if (!MAPTST(&installedm, ip))
		continue;
	      if (MAPTST(&userinstalled, ip - installed->start))
	        continue;
	      s = pool->solvables + ip;
	      if (!s->supplements)
		continue;
	      if (MAPTST(&im, ip))
		continue;
	      supp = s->repo->idarraydata + s->supplements;
	      while ((sup = *supp++) != 0)
		if (solver_dep_possible(solv, sup, &im))
		  break;
	      if (sup)
		{
#ifdef CLEANDEPSDEBUG
		  printf("%s supplemented\n", pool_solvid2str(pool, ip));
#endif
		  MAPSET(&im, ip);
		  queue_push(&iq, ip);
		}
	    }
	  if (!iq.count)
	    break;
	}
      ip = queue_shift(&iq);
      s = pool->solvables + ip;
#ifdef CLEANDEPSDEBUG
      printf("adding back %s\n", pool_solvable2str(pool, s));
#endif
      if (s->repo == installed && pool->implicitobsoleteusescolors)
	{
	  Id a, bestarch = 0;
	  FOR_PROVIDES(p, pp, s->name)
	    {
	      Solvable *ps = pool->solvables + p;
	      if (ps->name != s->name || ps->repo == installed)
	        continue;
	      a = ps->arch;
	      a = (a <= pool->lastarch) ? pool->id2arch[a] : 0;
	      if (a && a != 1 && (!bestarch || a < bestarch))
		bestarch = a;
	    }
	  if (bestarch && (s->arch > pool->lastarch || pool->id2arch[s->arch] != bestarch))
	    {
	      FOR_PROVIDES(p, pp, s->name)
		{
		  Solvable *ps = pool->solvables + p;
		  if (ps->repo == installed && ps->name == s->name && ps->evr == s->evr && ps->arch != s->arch && ps->arch < pool->lastarch && pool->id2arch[ps->arch] == bestarch)
		    if (!MAPTST(&im, p))
		      {
#ifdef CLEANDEPSDEBUG
		        printf("%s lockstep %s\n", pool_solvid2str(pool, ip), pool_solvid2str(pool, p));
#endif
		        MAPSET(&im, p);
		        queue_push(&iq, p);
		      }
		}
	    }
	}
      if (s->requires)
	{
	  reqp = s->repo->idarraydata + s->requires;
	  while ((req = *reqp++) != 0)
	    {
#ifdef ENABLE_COMPLEX_DEPS
	      if (pool_is_complex_dep(pool, req))
		{
		  complex_cleandeps_addback(pool, ip, req, &im, &installedm, &iq, &userinstalled);
		  continue;
		}
#endif
	      FOR_PROVIDES(p, pp, req)
		if (p == ip)
		  break;
	      if (p)
		continue;
	      FOR_PROVIDES(p, pp, req)
		{
		  if (MAPTST(&im, p))
		    continue;
		  if (MAPTST(&installedm, p))
		    {
		      if (p == ip)
			continue;
		      if (MAPTST(&userinstalled, p - installed->start))
			continue;
#ifdef CLEANDEPSDEBUG
		      printf("%s requires %s\n", pool_solvid2str(pool, ip), pool_solvid2str(pool, p));
#endif
		      MAPSET(&im, p);
		      queue_push(&iq, p);
		    }
		}
	    }
	}
      if (s->recommends)
	{
	  reqp = s->repo->idarraydata + s->recommends;
	  while ((req = *reqp++) != 0)
	    {
#ifdef ENABLE_COMPLEX_DEPS
	      if (pool_is_complex_dep(pool, req))
		{
		  complex_cleandeps_addback(pool, ip, req, &im, &installedm, &iq, &userinstalled);
		  continue;
		}
#endif
	      FOR_PROVIDES(p, pp, req)
		if (p == ip)
		  break;
	      if (p)
		continue;
	      FOR_PROVIDES(p, pp, req)
		{
		  if (MAPTST(&im, p))
		    continue;
		  if (MAPTST(&installedm, p))
		    {
		      if (p == ip)
			continue;
		      if (MAPTST(&userinstalled, p - installed->start))
			continue;
#ifdef CLEANDEPSDEBUG
		      printf("%s recommends %s\n", pool_solvid2str(pool, ip), pool_solvid2str(pool, p));
#endif
		      MAPSET(&im, p);
		      queue_push(&iq, p);
		    }
		}
	    }
	}
    }

  queue_free(&iq);
  /* make sure the updatepkgs and mistakes are not in the cleandeps map */
  if (!unneeded && solv->cleandeps_updatepkgs)
    {
      for (i = 0; i < updatepkgs_filtered.count; i++)
        MAPSET(&im, updatepkgs_filtered.elements[i]);
      queue_free(&updatepkgs_filtered);
    }
  if (solv->cleandeps_mistakes)
    for (i = 0; i < solv->cleandeps_mistakes->count; i++)
      MAPSET(&im, solv->cleandeps_mistakes->elements[i]);
  /* also remove original iq packages */
  for (i = 0; i < iqcopy.count; i++)
    MAPSET(&im, iqcopy.elements[i]);
  queue_free(&iqcopy);
  for (p = installed->start; p < installed->end; p++)
    {
      if (pool->solvables[p].repo != installed)
	continue;
      if (pool->considered && !MAPTST(pool->considered, p))
          continue;
      if (!MAPTST(&im, p))
        MAPSET(cleandepsmap, p - installed->start);
    }
  map_free(&im);
  map_free(&installedm);
  map_free(&userinstalled);
  queue_free(&xsuppq);
#ifdef CLEANDEPSDEBUG
  printf("=== final cleandeps map:\n");
  for (p = installed->start; p < installed->end; p++)
    if (MAPTST(cleandepsmap, p - installed->start))
      printf("  - %s\n", pool_solvid2str(pool, p));
#endif
}

void
solver_get_unneeded(Solver *solv, Queue *unneededq, int filtered)
{
  Repo *installed = solv->installed;
  int i;
  Map cleandepsmap;

  queue_empty(unneededq);
  if (!installed || installed->end == installed->start)
    return;

  map_init(&cleandepsmap, installed->end - installed->start);
  solver_createcleandepsmap(solv, &cleandepsmap, 1);
  for (i = installed->start; i < installed->end; i++)
    if (MAPTST(&cleandepsmap, i - installed->start))
      queue_push(unneededq, i);

  if (filtered)
    filter_unneeded(solv, unneededq, &cleandepsmap, 0);
  map_free(&cleandepsmap);
}

static void
add_cleandeps_mistake(Solver *solv, Id p)
{
 if (!solv->cleandeps_mistakes)
    {    
      solv->cleandeps_mistakes = solv_calloc(1, sizeof(Queue));
      queue_init(solv->cleandeps_mistakes);
    }    
  queue_push(solv->cleandeps_mistakes, p); 
  MAPCLR(&solv->cleandepsmap, p - solv->installed->start);
  solver_reenablepolicyrules_cleandeps(solv, p); 
}

static inline int
cleandeps_rule_is_true(Solver *solv, Rule *r)
{
  Pool *pool = solv->pool;
  Id p, pp;
  FOR_RULELITERALS(p, pp, r)
    if (p > 0 && solv->decisionmap[p] > 0)
      return 1;
  return 0;
}

int
solver_check_cleandeps_mistakes(Solver *solv)
{
  Pool *pool = solv->pool;
  Repo *installed = solv->installed;
  Rule *fr;
  int i, j, nj;
  int mademistake = 0;

  if (!solv->cleandepsmap.size || !installed)
    return 0;
  /* check for mistakes */
  policy_update_recommendsmap(solv);
  for (i = installed->start; i < installed->end; i++)
    {
      if (pool->solvables[i].repo != installed)
	continue;
      if (solv->decisionmap[i] > 0)
	{
	  Id req, *reqp;
	  Solvable *s = pool->solvables + i;
	  /* kept package, check requires. we need to do this for things like requires(pre) */
	  reqp = s->repo->idarraydata + s->requires;
	  while ((req = *reqp++) != 0)  
	    {
	      Id p2, pp2;
	      FOR_PROVIDES(p2, pp2, req)
		{
		  if (pool->solvables[p2].repo != installed)
		    continue;
		  if (p2 == i || solv->decisionmap[p2] > 0)
		    continue;
		  if (!MAPTST(&solv->cleandepsmap, p2 - installed->start))
		    continue;
		  POOL_DEBUG(SOLV_DEBUG_SOLVER, "cleandeps requires mistake: %s %s %s\n", pool_solvid2str(pool, i), pool_dep2str(pool, req), pool_solvid2str(pool, p2));
		  add_cleandeps_mistake(solv, p2);
		  mademistake = 1;
		}
	    }
	}
      if (!MAPTST(&solv->cleandepsmap, i - installed->start))
	continue;
      /* a mistake is when the featurerule is true but the updaterule is false */
      fr = solv->rules + solv->featurerules + (i - installed->start);
      if (!fr->p)
        fr = solv->rules + solv->updaterules + (i - installed->start);
      if (!fr->p)
	continue;
      if (!cleandeps_rule_is_true(solv, fr))
	{
	  /* feature rule is not true, thus we cleandeps erased the package */
	  /* check if the package is recommended/supplemented. if yes, we made a mistake. */
	  if (!MAPTST(&solv->recommendsmap, i) && !solver_is_supplementing(solv, pool->solvables + i))
	    continue;	/* feature rule is not true */
	  POOL_DEBUG(SOLV_DEBUG_SOLVER, "cleandeps recommends mistake: ");
	  solver_printruleclass(solv, SOLV_DEBUG_SOLVER, fr);
	}
      else
	{
	  Rule *r = solv->rules + solv->updaterules + (i - installed->start);
	  if (!r->p || r == fr || cleandeps_rule_is_true(solv, r))
	    {
	      /* update rule is true, check best rules */
	      if (!solv->bestrules_pkg)
		continue;
	      nj = solv->bestrules_end - solv->bestrules;
	      for (j = 0; j < nj; j++)
		if (solv->bestrules_pkg[j] == i)
		  {
		    r = solv->rules + solv->bestrules + j;
		    if (!cleandeps_rule_is_true(solv, r))
		      break;
		  }
	      if (j == nj)
		continue;
	    }
	  POOL_DEBUG(SOLV_DEBUG_SOLVER, "cleandeps mistake: ");
	  solver_printruleclass(solv, SOLV_DEBUG_SOLVER, r);
	  POOL_DEBUG(SOLV_DEBUG_SOLVER, "feature rule: ");
	  solver_printruleclass(solv, SOLV_DEBUG_SOLVER, fr);
	}
      add_cleandeps_mistake(solv, i);
      mademistake = 1;
    }
  return mademistake;
}
