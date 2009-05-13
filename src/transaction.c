/*
 * Copyright (c) 2007-2009, Novell Inc.
 *
 * This program is licensed under the BSD license, read LICENSE.BSD
 * for further information
 */

/*
 * transaction.c
 *
 * Transaction handling
 */

#include <stdio.h>
#include <stdlib.h>
#include <unistd.h>
#include <string.h>
#include <assert.h>

#include "transaction.h"
#include "solver.h"
#include "bitmap.h"
#include "pool.h"
#include "evr.h"
#include "util.h"

static int
obsq_sortcmp(const void *ap, const void *bp, void *dp)
{
  Id a, b, oa, ob;
  Pool *pool = dp;
  Solvable *s, *oas, *obs;
  int r;

  a = ((Id *)ap)[0];
  oa = ((Id *)ap)[1];
  b = ((Id *)bp)[0];
  ob = ((Id *)bp)[1];
  if (a != b)
    return a - b;
  if (oa == ob)
    return 0;
  s = pool->solvables + a;
  oas = pool->solvables + oa;
  obs = pool->solvables + ob;
  if (oas->name != obs->name)
    {
      if (oas->name == s->name)
        return -1;
      if (obs->name == s->name)
        return 1;
      return strcmp(id2str(pool, oas->name), id2str(pool, obs->name));
    }
  r = evrcmp(pool, oas->evr, obs->evr, EVRCMP_COMPARE);
  if (r)
    return -r;	/* highest version first */
  return oa - ob;
}

void
solver_transaction_all_pkgs(Solver *solv, Id p, Queue *pkgs)
{
  Pool *pool = solv->pool;
  Solvable *s = pool->solvables + p;
  Queue *ti = &solv->transaction_info;
  Id q;
  int i;

  queue_empty(pkgs);
  if (p <= 0 || !s->repo)
    return;
  if (s->repo == solv->installed)
    {
      q = solv->transaction_installed[p - solv->installed->start];
      if (!q)
	return;
      if (q > 0)
	{
	  queue_push(pkgs, q);
	  return;
	}
      /* find which packages obsolete us */
      for (i = 0; i < ti->count; i += 2)
	if (ti->elements[i + 1] == p)
	  {
	    queue_push(pkgs, p);
	    queue_push(pkgs, ti->elements[i]);
	  }
      /* sort obsoleters */
      if (pkgs->count > 2)
	sat_sort(pkgs->elements, pkgs->count / 2, 2 * sizeof(Id), obsq_sortcmp, pool);
      for (i = 0; i < pkgs->count; i += 2)
	pkgs->elements[i / 2] = pkgs->elements[i + 1];
      pkgs->count /= 2;
    }
  else
    {
      /* find the packages we obsolete */
      for (i = 0; i < ti->count; i += 2)
	{
	  if (ti->elements[i] == p)
	    queue_push(pkgs, ti->elements[i + 1]);
	  else if (pkgs->count)
	    break;
	}
    }
}

Id
solver_transaction_pkg(Solver *solv, Id p)
{
  Pool *pool = solv->pool;
  Solvable *s = pool->solvables + p;
  Queue ti;
  Id tibuf[5];

  if (p <= 0 || !s->repo)
    return 0;
  if (s->repo == solv->installed)
    {
      p = solv->transaction_installed[p - solv->installed->start];
      return p < 0 ? -p : p;
    }
  queue_init_buffer(&ti, tibuf, sizeof(tibuf)/sizeof(*tibuf));
  solver_transaction_all_pkgs(solv, p, &ti);
  p = ti.count ? ti.elements[0] : 0;
  queue_free(&ti);
  return p;
}

/* type filtering, needed if either not all packages are shown
 * or replaces are not shown, as otherwise parts of the
 * transaction might not be shown to the user */

Id
solver_transaction_filter(Solver *solv, Id type, Id p, int flags)
{
  Pool *pool = solv->pool;
  Solvable *s = pool->solvables + p;
  Queue oq, rq;
  Id q;
  int i, j, ref = 0;

  if (type == SOLVER_TRANSACTION_ERASE || type == SOLVER_TRANSACTION_INSTALL || type == SOLVER_TRANSACTION_MULTIINSTALL)
    return type;

  if (s->repo == pool->installed && (flags & SOLVER_TRANSACTION_SHOW_ACTIVE) == 0)
    {
      /* erase element */
      if ((flags & SOLVER_TRANSACTION_SHOW_REPLACES) == 0 && type == SOLVER_TRANSACTION_REPLACED)
	type = SOLVER_TRANSACTION_ERASE;
      return type;
    }
  if (s->repo != pool->installed && (flags & SOLVER_TRANSACTION_SHOW_ACTIVE) != 0)
    {
      if ((flags & SOLVER_TRANSACTION_SHOW_REPLACES) == 0 && type == SOLVER_TRANSACTION_REPLACE)
	type = SOLVER_TRANSACTION_INSTALL;
      return type;
    }

  /* most of the time there's only one reference, so check it first */
  q = solver_transaction_pkg(solv, p);
  if ((flags & SOLVER_TRANSACTION_SHOW_REPLACES) == 0)
    {
      Solvable *sq = pool->solvables + q;
      if (sq->name != s->name)
	{
	  if (s->repo == pool->installed)
	    return SOLVER_TRANSACTION_ERASE;
	  else if (type == SOLVER_TRANSACTION_MULTIREINSTALL)
	    return SOLVER_TRANSACTION_MULTIINSTALL;
	  else
	    return SOLVER_TRANSACTION_INSTALL;
	}
    }
  if (solver_transaction_pkg(solv, q) == p)
    return type;

  /* too bad, a miss. check em all */
  queue_init(&oq);
  queue_init(&rq);
  solver_transaction_all_pkgs(solv, p, &oq);
  for (i = 0; i < oq.count; i++)
    {
      q = oq.elements[i];
      if ((flags & SOLVER_TRANSACTION_SHOW_REPLACES) == 0)
	{
	  Solvable *sq = pool->solvables + q;
	  if (sq->name != s->name)
	    continue;
	}
      /* check if we are referenced? */
      if ((flags & SOLVER_TRANSACTION_SHOW_ALL) != 0)
	{
	  solver_transaction_all_pkgs(solv, q, &rq);
	  for (j = 0; j < rq.count; j++)
	    if (rq.elements[j] == p)
	      {
	        ref = 1;
	        break;
	      }
	  if (ref)
	    break;
	}
      else if (solver_transaction_pkg(solv, q) == p)
        {
	  ref = 1;
	  break;
        }
    }
  queue_free(&oq);
  queue_free(&rq);

  if (!ref)
    {
      if (s->repo == pool->installed)
	type = SOLVER_TRANSACTION_ERASE;
      else if (type == SOLVER_TRANSACTION_MULTIREINSTALL)
	type = SOLVER_TRANSACTION_MULTIINSTALL;
      else
	type = SOLVER_TRANSACTION_INSTALL;
    }
  return type;
}

static void
create_transaction_info(Solver *solv)
{
  Pool *pool = solv->pool;
  Queue *ti = &solv->transaction_info;
  Repo *installed = solv->installed;
  int i, j, noobs;
  Id p, p2, pp2;
  Solvable *s, *s2;

  queue_empty(ti);
  if (!installed)
    return;	/* no info needed */
  for (i = 0; i < solv->decisionq.count; i++)
    {
      p = solv->decisionq.elements[i];
      if (p <= 0 || p == SYSTEMSOLVABLE)
	continue;
      s = pool->solvables + p;
      if (s->repo == installed)
	continue;
      noobs = solv->noobsoletes.size && MAPTST(&solv->noobsoletes, p);
      FOR_PROVIDES(p2, pp2, s->name)
	{
	  if (solv->decisionmap[p2] > 0)
	    continue;
	  s2 = pool->solvables + p2;
	  if (s2->repo != installed)
	    continue;
	  if (noobs && (s->name != s2->name || s->evr != s2->evr || s->arch != s2->arch))
	    continue;
	  if (!solv->implicitobsoleteusesprovides && s->name != s2->name)
	    continue;
	  queue_push(ti, p);
	  queue_push(ti, p2);
	}
      if (s->obsoletes && !noobs)
	{
	  Id obs, *obsp = s->repo->idarraydata + s->obsoletes;
	  while ((obs = *obsp++) != 0)
	    {
	      FOR_PROVIDES(p2, pp2, obs)
		{
		  s2 = pool->solvables + p2;
		  if (s2->repo != installed)
		    continue;
		  if (!solv->obsoleteusesprovides && !pool_match_nevr(pool, pool->solvables + p2, obs))
		    continue;
		  queue_push(ti, p);
		  queue_push(ti, p2);
		}
	    }
	}
    }
  sat_sort(ti->elements, ti->count / 2, 2 * sizeof(Id), obsq_sortcmp, pool);
  /* now unify */
  for (i = j = 0; i < ti->count; i += 2)
    {
      if (j && ti->elements[i] == ti->elements[j - 2] && ti->elements[i + 1] == ti->elements[j - 1])
	continue;
      ti->elements[j++] = ti->elements[i];
      ti->elements[j++] = ti->elements[i + 1];
    }
  ti->count = j;

  /* create transaction_installed helper */
  solv->transaction_installed = sat_calloc(installed->end - installed->start, sizeof(Id));
  for (i = 0; i < ti->count; i += 2)
    {
      j = ti->elements[i + 1] - installed->start;
      if (!solv->transaction_installed[j])
	solv->transaction_installed[j] = ti->elements[i];
      else
	{
	  /* more than one package obsoletes us. compare */
	  Id q[4];
	  if (solv->transaction_installed[j] > 0)
	    solv->transaction_installed[j] = -solv->transaction_installed[j];
	  q[0] = q[2] = ti->elements[i + 1];
	  q[1] = ti->elements[i];
	  q[3] = -solv->transaction_installed[j];
	  if (obsq_sortcmp(q, q + 2, pool) < 0)
	    solv->transaction_installed[j] = -ti->elements[i];
	}
    }
}


void
solver_create_transaction(Solver *solv)
{
  Pool *pool = solv->pool;
  Repo *installed = solv->installed;
  int i, r, noobs;
  Id p, p2;
  Solvable *s, *s2;

  queue_empty(&solv->transaction);
  create_transaction_info(solv);

  if (installed)
    {
      FOR_REPO_SOLVABLES(installed, p, s)
	{
	  if (solv->decisionmap[p] > 0)
	    continue;
	  p2 = solver_transaction_pkg(solv, p);
	  if (!p2)
	    queue_push(&solv->transaction, SOLVER_TRANSACTION_ERASE);
	  else
	    {
	      s2 = pool->solvables + p2;
	      if (s->name == s2->name)
		{
		  if (s->evr == s2->evr && solvable_identical(s, s2))
		    queue_push(&solv->transaction, SOLVER_TRANSACTION_REINSTALLED);
		  else
		    {
		      r = evrcmp(pool, s->evr, s2->evr, EVRCMP_COMPARE);
		      if (r < 0)
			queue_push(&solv->transaction, SOLVER_TRANSACTION_UPGRADED);
		      else if (r > 0)
			queue_push(&solv->transaction, SOLVER_TRANSACTION_DOWNGRADED);
		      else
			queue_push(&solv->transaction, SOLVER_TRANSACTION_CHANGED);
		    }
		}
	      else
		queue_push(&solv->transaction, SOLVER_TRANSACTION_REPLACED);
	    }
	  queue_push(&solv->transaction, p);
	}
    }
  for (i = 0; i < solv->decisionq.count; i++)
    {
      p = solv->decisionq.elements[i];
      if (p < 0 || p == SYSTEMSOLVABLE)
	continue;
      s = pool->solvables + p;
      if (solv->installed && s->repo == solv->installed)
	continue;
      noobs = solv->noobsoletes.size && MAPTST(&solv->noobsoletes, p);
      p2 = solver_transaction_pkg(solv, p);
      if (noobs)
	queue_push(&solv->transaction, p2 ? SOLVER_TRANSACTION_MULTIREINSTALL : SOLVER_TRANSACTION_MULTIINSTALL);
      else if (!p2)
	queue_push(&solv->transaction, SOLVER_TRANSACTION_INSTALL);
      else
	{
	  s2 = pool->solvables + p2;
	  if (s->name == s2->name)
	    {
	      if (s->evr == s2->evr && solvable_identical(s, s2))
		queue_push(&solv->transaction, SOLVER_TRANSACTION_REINSTALL);
	      else
		{
		  r = evrcmp(pool, s->evr, s2->evr, EVRCMP_COMPARE);
		  if (r > 0)
		    queue_push(&solv->transaction, SOLVER_TRANSACTION_UPGRADE);
		  else if (r < 0)
		    queue_push(&solv->transaction, SOLVER_TRANSACTION_DOWNGRADE);
		  else
		    queue_push(&solv->transaction, SOLVER_TRANSACTION_CHANGE);
		}
	    }
	  else
	    queue_push(&solv->transaction, SOLVER_TRANSACTION_REPLACE);
	}
      queue_push(&solv->transaction, p);
    }
}

#define TYPE_REQ    (1<<0)
#define TYPE_PREREQ (1<<1)
#define TYPE_CON    (1<<2)
#define TYPE_ERASE  (1<<3)

#define EDGEDATA_BLOCK 127

struct transel {
  Id p;
  Id edges;
};

struct orderdata {
  Solver *solv;
  struct transel *tes;
  int ntes;
  Id *edgedata;
  int nedgedata;
};

static void
addedge(struct orderdata *od, Id from, Id to, int type)
{
  Solver *solv = od->solv;
  Pool *pool = solv->pool;
  Solvable *s;
  struct transel *te;
  int i;

  // printf("addedge %d %d type %d\n", from, to, type);
  s = pool->solvables + from;
  if (s->repo == solv->installed && solv->transaction_installed[from - solv->installed->start])
    {
      /* passive, map to active */
      if (solv->transaction_installed[from - solv->installed->start] > 0)
	from = solv->transaction_installed[from - solv->installed->start];
      else
	{
	  Queue ti;
	  Id tibuf[5];
	  queue_init_buffer(&ti, tibuf, sizeof(tibuf)/sizeof(*tibuf));
	  solver_transaction_all_pkgs(solv, from, &ti);
	  for (i = 0; i < ti.count; i++)
	    addedge(od, ti.elements[i], to, type);
	  queue_free(&ti);
	}
      return;
    }
  s = pool->solvables + to;
  if (s->repo == solv->installed && solv->transaction_installed[to - solv->installed->start])
    {
      /* passive, map to active */
      if (solv->transaction_installed[to - solv->installed->start] > 0)
	to = solv->transaction_installed[to - solv->installed->start];
      else
	{
	  Queue ti;
	  Id tibuf[5];
	  queue_init_buffer(&ti, tibuf, sizeof(tibuf)/sizeof(*tibuf));
	  solver_transaction_all_pkgs(solv, to, &ti);
	  for (i = 0; i < ti.count; i++)
	    addedge(od, from, ti.elements[i], type);
	  queue_free(&ti);
	  return;
	}
    }

  /* map target to te num */
  for (i = 1, te = od->tes + i; i < od->ntes; i++, te++)
    if (te->p == to)
      break;
  if (i == od->ntes)
    return;
  to = i;

  for (i = 1, te = od->tes + i; i < od->ntes; i++, te++)
    if (te->p == from)
      break;
  if (i == od->ntes)
    return;

  if (i == to)
    return;	/* no edges to ourselfes */

  // printf("edge %d -> %d type %x\n", i, to, type);

  for (i = te->edges; od->edgedata[i]; i += 2)
    if (od->edgedata[i] == to)
      break;
  if (od->edgedata[i])
    {
      od->edgedata[i + 1] |= type;
      return;
    }
  if (i + 1 == od->nedgedata)
    {
      // printf("tail add %d\n", i - te->edges);
      if (!i)
	te->edges = ++i;
      od->edgedata = sat_extend(od->edgedata, od->nedgedata, 3, sizeof(Id), EDGEDATA_BLOCK);
    }
  else
    {
      // printf("extend %d\n", i - te->edges);
      od->edgedata = sat_extend(od->edgedata, od->nedgedata, 3 + (i - te->edges), sizeof(Id), EDGEDATA_BLOCK);
      if (i > te->edges)
	memcpy(od->edgedata + od->nedgedata, od->edgedata + te->edges, sizeof(Id) * (i - te->edges));
      i = od->nedgedata + (i - te->edges);
      te->edges = od->nedgedata;
    }
  od->edgedata[i] = to;
  od->edgedata[i + 1] = type;
  od->edgedata[i + 2] = 0;
  od->nedgedata = i + 3;
}

void
solver_order_transaction(Solver *solv)
{
  Pool *pool = solv->pool;
  Queue *tr = &solv->transaction;
  Repo *installed = solv->installed;
  Id type, p;
  Solvable *s, *s2;
  Id req, *reqp, con, *conp;
  Id p2, pp2;
  int i, j, pre, numte, numedge;
  struct orderdata od;
  struct transel *te;

  /* create a transaction element for every active component */
  numte = 0;
  for (i = 0; i < tr->count; i += 2)
    {
      p = tr->elements[i + 1];
      s = pool->solvables + p;
      if (s->repo != installed || !solv->transaction_installed[p - solv->installed->start])
	numte++;
    }
  if (!numte)
    return;	/* nothing to do... */


  printf("numte = %d\n", numte);
  numte++;	/* leave first one zero */
  od.solv = solv;
  od.ntes = numte;
  od.tes = sat_calloc(numte, sizeof(*od.tes));
  od.edgedata = sat_extend(0, 0, 1, sizeof(Id), EDGEDATA_BLOCK);
  od.edgedata[0] = 0;
  od.nedgedata = 1;

  for (i = 0, te = od.tes + 1; i < tr->count; i += 2)
    {
      p = tr->elements[i + 1];
      s = pool->solvables + p;
      if (s->repo == installed && solv->transaction_installed[p - solv->installed->start])
	continue;
      te->p = p;
      te++;
    }

  /* create dependency graph */
  for (i = 0; i < tr->count; i += 2)
    {
      type = tr->elements[i];
      p = tr->elements[i + 1];
      s = pool->solvables + p;
      if (s->requires)
	{
	  reqp = s->repo->idarraydata + s->requires;
	  pre = TYPE_REQ;
	  while ((req = *reqp++) != 0)
	    {
	      int eraseonly = 0;
	      if (req == SOLVABLE_PREREQMARKER)
		{
		  pre = TYPE_PREREQ;
		  continue;
		}
#if 1
	      if (s->repo == installed && pre != TYPE_PREREQ)
		continue;
#endif
	      FOR_PROVIDES(p2, pp2, req)
		{
		  if (p2 == p)
		    continue;
		  s2 = pool->solvables + p2;
		  if (!s2->repo)
		    continue;
		  if (s2->repo == installed && solv->decisionmap[p2] > 0)
		    continue;
		  if (s2->repo != installed && solv->decisionmap[p2] < 0)
		    continue;	/* not interesting */
		  if (s->repo == installed)
		    {
		      /* we're uninstalling s */
		      if (s2->repo == installed)
			{
			  if (eraseonly == 0)
			    eraseonly = 1;
			}
		      if (s2->repo != installed)
			{
			  /* update p2 before erasing p */
#if 1
		          addedge(&od, p, p2, pre);
#endif
			  eraseonly = -1;
			}
		    }
		  else
		    {
		      /* install p2 before installing p */
		      if (s2->repo != installed)
		        addedge(&od, p, p2, pre);
		    }
		}
	      if (eraseonly == 1)
		{
		  printf("eraseonlyedge for %s req %s\n", solvable2str(pool, s), dep2str(pool, req));
		  /* need edges to uninstalled pkgs */
#if 1
		  FOR_PROVIDES(p2, pp2, req)
		    {
		      if (p2 == p)
			continue;
		      s2 = pool->solvables + p2;
		      if (!s2->repo || s2->repo != installed)
			continue;
		      if (solv->decisionmap[p2] > 0)
			continue;
#if 0
		      addedge(&od, p2, p, pre);
#else
		      addedge(&od, p2, p, TYPE_ERASE);
#endif
		    }
#endif
		}
	    }
	}
      if (s->conflicts)
	{
	  conp = s->repo->idarraydata + s->conflicts;
	  while ((con = *conp++) != 0)
	    {
#if 1
	      FOR_PROVIDES(p2, pp2, con)
		{
		  if (p2 == p)
		    continue;
		  s2 = pool->solvables + p2;
		  if (!s2->repo)
		    continue;
		  if (s->repo == installed)
		    {
		      if (s2->repo != installed && solv->decisionmap[p2] >= 0)
			{
			  /* deinstall p before installing p2 */
			  addedge(&od, p2, p, TYPE_CON);
			}
		    }
		  else
		    {
		      if (s2->repo == installed && solv->decisionmap[p2] < 0)
			{
			  /* deinstall p2 before installing p */
#if 1
			  addedge(&od, p, p2, TYPE_CON);
#endif
			}
		    }

		}
#endif
	    }
	}
    }
  numedge = 0;
  for (i = 1, te = od.tes + i; i < numte; i++, te++)
    {
      printf("TE #%d, %d(%s)\n", i, te->p, solvid2str(pool, te->p));
      for (j = te->edges; od.edgedata[j]; j += 2)
        {
	  struct transel *te2 = od.tes + od.edgedata[j];
	  printf("  depends %x on TE %d, %d(%s)\n", od.edgedata[j + 1], od.edgedata[j], te2->p, solvid2str(pool, te2->p));
          numedge++;
 	}
    }
  printf("TEs: %d, Edges: %d, Space: %d\n", numte - 1, numedge, od.nedgedata / 2);
}
