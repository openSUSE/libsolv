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

#define TYPE_BROKEN	(1<<0)
#define TYPE_CON    	(1<<1)

#define TYPE_REQ_P    	(1<<2)
#define TYPE_PREREQ_P 	(1<<3)

#define TYPE_REQ    	(1<<4)
#define TYPE_PREREQ 	(1<<5)

#define EDGEDATA_BLOCK	127

struct transel {
  Id p;
  Id type;
  Id edges;
  Id invedges;
  Id mark;
  Id medianr;
  Id ddeg;	/* unused */
};

struct orderdata {
  Solver *solv;
  struct transel *tes;
  int ntes;
  Id *edgedata;
  int nedgedata;
  Id *invedgedata;
  Map cycletes;
};

static int
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
	  int ret = 0;
	  Queue ti;
	  Id tibuf[5];

	  queue_init_buffer(&ti, tibuf, sizeof(tibuf)/sizeof(*tibuf));
	  solver_transaction_all_pkgs(solv, from, &ti);
	  for (i = 0; i < ti.count; i++)
	    ret |= addedge(od, ti.elements[i], to, type);
	  queue_free(&ti);
	  return ret;
	}
    }
  s = pool->solvables + to;
  if (s->repo == solv->installed && solv->transaction_installed[to - solv->installed->start])
    {
      /* passive, map to active */
      if (solv->transaction_installed[to - solv->installed->start] > 0)
	to = solv->transaction_installed[to - solv->installed->start];
      else
	{
	  int ret = 0;
	  Queue ti;
	  Id tibuf[5];

	  queue_init_buffer(&ti, tibuf, sizeof(tibuf)/sizeof(*tibuf));
	  solver_transaction_all_pkgs(solv, to, &ti);
	  for (i = 0; i < ti.count; i++)
	    ret |= addedge(od, from, ti.elements[i], type);
	  queue_free(&ti);
	  return ret;
	}
    }

  /* map target to te num */
  for (i = 1, te = od->tes + i; i < od->ntes; i++, te++)
    if (te->p == to)
      break;
  if (i == od->ntes)
    return 0;
  to = i;

  for (i = 1, te = od->tes + i; i < od->ntes; i++, te++)
    if (te->p == from)
      break;
  if (i == od->ntes)
    return 0;

  if (i == to)
    return 0;	/* no edges to ourselfes */

  /* printf("edge %d(%s) -> %d(%s) type %x\n", i, solvid2str(pool, od->tes[i].p), to, solvid2str(pool, od->tes[to].p), type); */

  for (i = te->edges; od->edgedata[i]; i += 2)
    if (od->edgedata[i] == to)
      break;
  /* test of brokenness */
  if (type == TYPE_BROKEN)
    return od->edgedata[i] && (od->edgedata[i + 1] & TYPE_BROKEN) != 0 ? 1 : 0;
  if (od->edgedata[i])
    {
      od->edgedata[i + 1] |= type;
      return 0;
    }
  if (i + 1 == od->nedgedata)
    {
      /* printf("tail add %d\n", i - te->edges); */
      if (!i)
	te->edges = ++i;
      od->edgedata = sat_extend(od->edgedata, od->nedgedata, 3, sizeof(Id), EDGEDATA_BLOCK);
    }
  else
    {
      /* printf("extend %d\n", i - te->edges); */
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
  te->ddeg++;
  od->tes[to].ddeg--;
  return 0;
}

static int
havechoice(struct orderdata *od, Id p, Id q1, Id q2)
{
  Solver *solv = od->solv;
  Id ti1buf[5], ti2buf[5];
  Queue ti1, ti2;
  int i, j;

  /* both q1 and q2 are uninstalls. check if their TEs intersect */
  /* common case: just one TE for both packages */
  printf("havechoice %d %d %d\n", p, q1, q2);
  if (solv->transaction_installed[q1 - solv->installed->start] == 0)
    return 1;
  if (solv->transaction_installed[q2 - solv->installed->start] == 0)
    return 1;
  if (solv->transaction_installed[q1 - solv->installed->start] == solv->transaction_installed[q2 - solv->installed->start])
    return 0;
  if (solv->transaction_installed[q1 - solv->installed->start] > 0 && solv->transaction_installed[q2 - solv->installed->start] > 0)
    return 1;
  queue_init_buffer(&ti1, ti1buf, sizeof(ti1buf)/sizeof(*ti1buf));
  solver_transaction_all_pkgs(solv, q1, &ti1);
  queue_init_buffer(&ti2, ti2buf, sizeof(ti2buf)/sizeof(*ti2buf));
  solver_transaction_all_pkgs(solv, q2, &ti2);
  for (i = 0; i < ti1.count; i++)
    for (j = 0; j < ti2.count; j++)
      if (ti1.elements[i] == ti2.elements[j])
	{
	  /* found a common edge */
	  queue_free(&ti1);
	  queue_free(&ti2);
	  return 0;
	}
  queue_free(&ti1);
  queue_free(&ti2);
  return 1;
}

static void
addsolvableedges(struct orderdata *od, Solvable *s)
{
  Solver *solv = od->solv;
  Pool *pool = solv->pool;
  Id req, *reqp, con, *conp;
  Id p, p2, pp2;
  int i, j, pre, numins;
  Repo *installed = solv->installed;
  Solvable *s2;
  Queue reqq;

  p = s - pool->solvables;
  queue_init(&reqq);
  if (s->requires)
    {
      reqp = s->repo->idarraydata + s->requires;
      pre = TYPE_REQ;
      while ((req = *reqp++) != 0)
	{
	  if (req == SOLVABLE_PREREQMARKER)
	    {
	      pre = TYPE_PREREQ;
	      continue;
	    }
	  queue_empty(&reqq);
	  numins = 0;	/* number of packages to be installed providing it */
	  FOR_PROVIDES(p2, pp2, req)
	    {
	      s2 = pool->solvables + p2;
	      if (p2 == p)
		{
		  reqq.count = 0;	/* self provides */
		  break;
		}
	      if (s2->repo == installed && solv->decisionmap[p2] > 0)
		{
		  reqq.count = 0;	/* provided by package that stays installed */
		  break;
		}
	      if (s2->repo != installed && solv->decisionmap[p2] <= 0)
		continue;		/* package stays uninstalled */
	      
	      if (s->repo == installed)
		{
		  /* s gets uninstalled */
		  queue_pushunique(&reqq, p2);
		  if (s2->repo != installed)
		    numins++;
		}
	      else
		{
		  if (s2->repo == installed)
		    continue;	/* s2 gets uninstalled */
		  queue_pushunique(&reqq, p2);
		  /* EDGE s(A) -> s2(A) */
		}
	    }
	  if (numins && reqq.count)
	    {
	      /* no mixed types, remove all deps on uninstalls */
	      for (i = j = 0; i < reqq.count; i++)
		if (pool->solvables[reqq.elements[i]].repo != installed)
		  reqq.elements[j++] = reqq.elements[i];
	      reqq.count = j;
	    }
          if (!reqq.count)
	    continue;
          for (i = 0; i < reqq.count; i++)
	    {
	      int choice = 0;
	      p2 = reqq.elements[i];
	      if (pool->solvables[p2].repo != installed)
		{
		  /* all elements of reqq are installs, thus have different TEs */
		  choice = reqq.count - 1;
		  if (pool->solvables[p].repo != installed)
		    {
#if 0
		      printf("add inst edge choice %d (%s -> %s -> %s)\n", choice, solvid2str(pool, p), dep2str(pool, req), solvid2str(pool, p2));
#endif
		      addedge(od, p, p2, pre);
		    }
		  else
		    {
#if 0
		      printf("add uninst->inst edge choice %d (%s -> %s -> %s)\n", choice, solvid2str(pool, p), dep2str(pool, req), solvid2str(pool, p2));
#endif
		      addedge(od, p, p2, pre == TYPE_PREREQ ? TYPE_PREREQ_P : TYPE_REQ_P);
		    }
		}
	      else
		{
#if 1
		  choice = 0;
		  for (j = 0; j < reqq.count; j++)
		    {
		      if (i == j)
			continue;
		      if (havechoice(od, p, reqq.elements[i], reqq.elements[j]))
			choice++;
		    }
#endif
#if 0
		  printf("add uninst->uninst edge choice %d (%s -> %s -> %s)\n", choice, solvid2str(pool, p), dep2str(pool, req), solvid2str(pool, p2));
#endif
	          addedge(od, p2, p, pre == TYPE_PREREQ ? TYPE_PREREQ_P : TYPE_REQ_P);
		}
	    }
	}
    }
  if (s->conflicts)
    {
      conp = s->repo->idarraydata + s->conflicts;
      while ((con = *conp++) != 0)
	{
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
		      addedge(od, p2, p, TYPE_CON);
		    }
		}
	      else
		{
		  if (s2->repo == installed && solv->decisionmap[p2] < 0)
		    {
		      /* deinstall p2 before installing p */
		      addedge(od, p, p2, TYPE_CON);
		    }
		}

	    }
	}
    }
  queue_free(&reqq);
}

static inline int
haveprereq(Pool *pool, Id solvid)
{
  Solvable *s = pool->solvables + solvid;
  if (s->requires)
    {
      Id req, *reqp;
      int inpre = 0;
      reqp = s->repo->idarraydata + s->requires;
      while ((req = *reqp++) != 0)
	{
          if (req == SOLVABLE_PREREQMARKER)
	    {
	      inpre = 1;
	      continue;
	    }
	  if (inpre && strncmp(id2str(pool, req), "rpmlib(", 7) != 0)
	    return 1;
	}
    }
  return 0;
}

void
breakcycle(struct orderdata *od, Id *cycle)
{
  Pool *pool = od->solv->pool;
  Id ddegmin, ddegmax, ddeg;
  int k, l;
  struct transel *te;

  l = 0;
  ddegmin = ddegmax = 0;
  for (k = 0; cycle[k + 1]; k += 2)
    {
      MAPSET(&od->cycletes, cycle[k]);	/* this te is involved in a cycle */
      ddeg = od->edgedata[cycle[k + 1] + 1];
      if (ddeg > ddegmax)
	ddegmax = ddeg;
      if (!k || ddeg < ddegmin)
	{
	  l = k;
	  ddegmin = ddeg;
	  continue;
	}
      if (ddeg == ddegmin)
	{
	  if (haveprereq(pool, od->tes[cycle[l]].p) && !haveprereq(pool, od->tes[cycle[k]].p))
	    {
	      /* prefer k, as l comes from a package with contains scriptlets */
	      l = k;
	      ddegmin = ddeg;
	      continue;
	    }
	  /* same edge value, check for prereq */
	}
    }

  od->edgedata[cycle[l + 1] + 1] |= TYPE_BROKEN;

#if 1
  if (ddegmin < TYPE_REQ)
    return;
#endif


  /* cycle recorded, print it */
  if ((ddegmax & TYPE_PREREQ) != 0)
    printf("CRITICAL ");
  printf("cycle: --> ");
  for (k = 0; cycle[k + 1]; k += 2)
    {
      te = od->tes +  cycle[k];
      if ((od->edgedata[cycle[k + 1] + 1] & TYPE_BROKEN) != 0)
        printf("%s ##%x##> ", solvid2str(pool, te->p), od->edgedata[cycle[k + 1] + 1]);
      else
        printf("%s --%x--> ", solvid2str(pool, te->p), od->edgedata[cycle[k + 1] + 1]);
    }
  printf("\n");
}

void
solver_order_transaction(Solver *solv)
{
  Pool *pool = solv->pool;
  Queue *tr = &solv->transaction;
  Repo *installed = solv->installed;
  Id type, p;
  Solvable *s;
  int i, j, k, numte, numedge;
  struct orderdata od;
  struct transel *te;
  Queue todo, obsq;
  int cycstart, cycel, broken;
  Id *cycle, *obstypes;
  int oldcount;
  int now;

  POOL_DEBUG(SAT_DEBUG_STATS, "ordering transaction\n");
  now = sat_timems(0);
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

  POOL_DEBUG(SAT_DEBUG_STATS, "transaction elements: %d\n", numte);
  numte++;	/* leave first one zero */
  memset(&od, 0, sizeof(od));
  od.solv = solv;
  od.ntes = numte;
  od.tes = sat_calloc(numte, sizeof(*od.tes));
  od.edgedata = sat_extend(0, 0, 1, sizeof(Id), EDGEDATA_BLOCK);
  od.edgedata[0] = 0;
  od.nedgedata = 1;
  map_init(&od.cycletes, numte);

  for (i = 0, te = od.tes + 1; i < tr->count; i += 2)
    {
      p = tr->elements[i + 1];
      s = pool->solvables + p;
      if (s->repo == installed && solv->transaction_installed[p - solv->installed->start])
	continue;
      te->p = p;
      te->type = tr->elements[i];
      te->medianr = solvable_lookup_num(s, SOLVABLE_MEDIANR, 0);
      te++;
    }

  /* create dependency graph */
  for (i = 0; i < tr->count; i += 2)
    {
      type = tr->elements[i];
      p = tr->elements[i + 1];
      addsolvableedges(&od, pool->solvables + p);
    }

  /* count edges */
  numedge = 0;
  for (i = 1, te = od.tes + i; i < numte; i++, te++)
    for (j = te->edges; od.edgedata[j]; j += 2)
      numedge++;
  POOL_DEBUG(SAT_DEBUG_STATS, "edges: %d, edge space: %d\n", numedge, od.nedgedata / 2);
  
  /* kill all cycles */
  broken = 0;

  queue_init(&todo);
  for (i = numte - 1; i > 0; i--)
    queue_push(&todo, i);

  while (todo.count)
    {
      i = queue_pop(&todo);
      /* printf("- look at TE %d\n", i); */
      if (i < 0)
	{
	  i = -i;
	  od.tes[i].mark = 2;	/* done with that one */
	  continue;
	}
      te = od.tes + i;
      if (te->mark == 2)
	continue;		/* already finished before */
      if (te->mark == 0)
	{
	  int edgestovisit = 0;
	  /* new node, visit edges */
	  for (j = te->edges; (k = od.edgedata[j]) != 0; j += 2)
	    {
	      if ((od.edgedata[j + 1] & TYPE_BROKEN) != 0)
		continue;
	      if (od.tes[k].mark == 2)
		continue;	/* no need to visit again */
	      if (!edgestovisit++)
	        queue_push(&todo, -i);	/* end of edges marker */
	      queue_push(&todo, k);
	    }
	  if (!edgestovisit)
	    te->mark = 2;	/* no edges, done with that one */
	  else
	    te->mark = 1;	/* under investigation */
	  continue;
	}
      /* oh no, we found a cycle */
      /* find start of cycle node (<0) */
      for (j = todo.count - 1; j >= 0; j--)
	if (todo.elements[j] == -i)
	  break;
      assert(j >= 0);
      cycstart = j;
      /* build te/edge chain */
      k = cycstart;
      for (j = k; j < todo.count; j++)
	if (todo.elements[j] < 0)
	  todo.elements[k++] = -todo.elements[j];
      cycel = k - cycstart;
      assert(cycel > 1);
      /* make room for edges, two extra element for cycle loop + terminating 0 */
      while (todo.count < cycstart + 2 * cycel + 2)
	queue_push(&todo, 0);
      cycle = todo.elements + cycstart;
      cycle[cycel] = i;		/* close the loop */
      cycle[2 * cycel + 1] = 0;	/* terminator */
      for (k = cycel; k > 0; k--)
	{
	  cycle[k * 2] = cycle[k];
	  te = od.tes + cycle[k - 1];
	  if (te->mark == 1)
	    te->mark = 0;	/* reset investigation marker */
	  /* printf("searching for edge from %d to %d\n", cycle[k - 1], cycle[k]); */
	  for (j = te->edges; od.edgedata[j]; j += 2)
	    if (od.edgedata[j] == cycle[k])
	      break;
	  assert(od.edgedata[j]);
	  cycle[k * 2 - 1] = j;
	}
      /* now cycle looks like this: */
      /* te1 edge te2 edge te3 ... teN edge te1 0 */
      breakcycle(&od, cycle);
      broken++;
      /* restart with start of cycle */
      todo.count = cycstart + 1;
    }
  POOL_DEBUG(SAT_DEBUG_STATS, "cycles broken: %d\n", broken);

  /* invert all edges */
  for (i = 1, te = od.tes + i; i < numte; i++, te++)
    {
      te->invedges = 1;	/* term 0 */
      te->mark = 0;	/* backref count */
    }

  for (i = 1, te = od.tes + i; i < numte; i++, te++)
    {
      for (j = te->edges; od.edgedata[j]; j += 2)
        {
	  if ((od.edgedata[j + 1] & TYPE_BROKEN) != 0)
	    continue;
	  struct transel *te2 = od.tes + od.edgedata[j];
	  te2->invedges++;
	}
    }
  j = 1;
  for (i = 1, te = od.tes + i; i < numte; i++, te++)
    {
      te->invedges += j;
      j = te->invedges;
    }
  POOL_DEBUG(SAT_DEBUG_STATS, "invedge space: %d\n", j + 1);
  od.invedgedata = sat_calloc(j + 1, sizeof(Id));
  for (i = 1, te = od.tes + i; i < numte; i++, te++)
    {
      for (j = te->edges; od.edgedata[j]; j += 2)
        {
	  if ((od.edgedata[j + 1] & TYPE_BROKEN) != 0)
	    continue;
	  struct transel *te2 = od.tes + od.edgedata[j];
	  od.invedgedata[--te2->invedges] = i;
	  te->mark++;
	}
    }
  /* now the final ordering */
  for (i = 1, te = od.tes + i; i < numte; i++, te++)
    if (te->mark == 0)
      queue_push(&todo, i);
  assert(todo.count > 0);
  if (installed)
    {
      obstypes = sat_calloc(installed->end - installed->start, sizeof(Id));
      for (i = 0; i < tr->count; i += 2)
	{
	  p = tr->elements[i + 1];
	  s = pool->solvables + p;
	  if (s->repo == installed && solv->transaction_installed[p - installed->start])
	    obstypes[p - installed->start] = tr->elements[i];
	}
    }
  else
    obstypes = 0;
  oldcount = tr->count;
  queue_empty(tr);
  queue_init(&obsq);
  
  while (todo.count)
    {
      /* select an i */
      i = todo.count;
      if (installed)
	{
	  for (i = 0; i < todo.count; i++)
	    {
	      j = todo.elements[i];
	      if (pool->solvables[od.tes[j].p].repo == installed)
	        break;
	    }
	}
      if (i == todo.count)
	{
	  for (i = 0; i < todo.count; i++)
	    {
	      j = todo.elements[i];
	      if (MAPTST(&od.cycletes, j))
		break;
	    }
	}
      if (i == todo.count)
        i = 0;
      te = od.tes + todo.elements[i];
      queue_push(tr, te->type);
      queue_push(tr, te->p);
      s = pool->solvables + te->p;
      if (installed && s->repo != installed)
	{
	  queue_empty(&obsq);
	  solver_transaction_all_pkgs(solv, te->p, &obsq);
	  for (j = 0; j < obsq.count; j++)
	    {
	      p = obsq.elements[j];
	      assert(p >= installed->start && p < installed->end);
	      if (obstypes[p - installed->start])
		{
		  queue_push(tr, obstypes[p - installed->start]);
		  queue_push(tr, p);
		  obstypes[p - installed->start] = 0;
		}
	    }
	}
      if (i < todo.count - 1)
        memmove(todo.elements + i, todo.elements + i + 1, (todo.count - 1 - i) * sizeof(Id));
      queue_pop(&todo);
      for (j = te->invedges; od.invedgedata[j]; j++)
	{
	  struct transel *te2 = od.tes + od.invedgedata[j];
	  assert(te2->mark > 0);
	  if (--te2->mark == 0)
	    {
	      queue_push(&todo, od.invedgedata[j]);
	    }
	}
    }
  queue_free(&todo);
  queue_free(&obsq);
  for (i = 1, te = od.tes + i; i < numte; i++, te++)
    assert(te->mark == 0);
  assert(tr->count == oldcount);
  POOL_DEBUG(SAT_DEBUG_STATS, "transaction ordering took %d ms\n", sat_timems(now));
}

