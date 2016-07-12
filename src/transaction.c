/*
 * Copyright (c) 2007-2015, SUSE LLC
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
#include "poolarch.h"
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
      /* bring "same name" obsoleters (i.e. upgraders) to front */
      if (oas->name == s->name)
        return -1;
      if (obs->name == s->name)
        return 1;
      return strcmp(pool_id2str(pool, oas->name), pool_id2str(pool, obs->name));
    }
  r = pool_evrcmp(pool, oas->evr, obs->evr, EVRCMP_COMPARE);
  if (r)
    return -r;	/* highest version first */
  if (oas->arch != obs->arch)
    {
      /* bring same arch to front */
      if (oas->arch == s->arch)
        return -1;
      if (obs->arch == s->arch)
        return 1;
    }
  return oa - ob;
}

void
transaction_all_obs_pkgs(Transaction *trans, Id p, Queue *pkgs)
{
  Pool *pool = trans->pool;
  Solvable *s = pool->solvables + p;
  Queue *ti = &trans->transaction_info;
  Id q;
  int i;

  queue_empty(pkgs);
  if (p <= 0 || !s->repo)
    return;
  if (s->repo == pool->installed)
    {
      q = trans->transaction_installed[p - pool->installed->start];
      if (!q)
	return;
      if (q > 0)
	{
	  /* only a single obsoleting package */
	  queue_push(pkgs, q);
	  return;
	}
      /* find which packages obsolete us */
      for (i = 0; i < ti->count; i += 2)
	if (ti->elements[i + 1] == p)
	  queue_push2(pkgs, p, ti->elements[i]);
      /* sort obsoleters */
      if (pkgs->count > 2)
	solv_sort(pkgs->elements, pkgs->count / 2, 2 * sizeof(Id), obsq_sortcmp, pool);
      for (i = 0; i < pkgs->count; i += 2)
	pkgs->elements[i / 2] = pkgs->elements[i + 1];
      queue_truncate(pkgs, pkgs->count / 2);
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
transaction_obs_pkg(Transaction *trans, Id p)
{
  Pool *pool = trans->pool;
  Solvable *s = pool->solvables + p;
  Queue *ti;
  int i;

  if (p <= 0 || !s->repo)
    return 0;
  if (s->repo == pool->installed)
    {
      p = trans->transaction_installed[p - pool->installed->start];
      return p < 0 ? -p : p;
    }
  ti = &trans->transaction_info;
  for (i = 0; i < ti->count; i += 2)
    if (ti->elements[i] == p)
      return ti->elements[i + 1];
  return 0;
}


/*
 * calculate base type of transaction element
 */

static Id
transaction_base_type(Transaction *trans, Id p)
{
  Pool *pool = trans->pool;
  Solvable *s, *s2;
  int r;
  Id p2;

  if (!MAPTST(&trans->transactsmap, p))
    return SOLVER_TRANSACTION_IGNORE;
  p2 = transaction_obs_pkg(trans, p);
  if (pool->installed && pool->solvables[p].repo == pool->installed)
    {
      /* erase */
      if (!p2)
	return SOLVER_TRANSACTION_ERASE;
      s = pool->solvables + p;
      s2 = pool->solvables + p2;
      if (s->name == s2->name)
	{
	  if (s->evr == s2->evr && solvable_identical(s, s2))
	    return SOLVER_TRANSACTION_REINSTALLED;
	  r = pool_evrcmp(pool, s->evr, s2->evr, EVRCMP_COMPARE);
	  if (r < 0)
	    return SOLVER_TRANSACTION_UPGRADED;
	  else if (r > 0)
	    return SOLVER_TRANSACTION_DOWNGRADED;
	  return SOLVER_TRANSACTION_CHANGED;
	}
      return SOLVER_TRANSACTION_OBSOLETED;
    }
  else
    {
      /* install or multiinstall */
      int multi = trans->multiversionmap.size && MAPTST(&trans->multiversionmap, p);
      if (multi)
	{
	  if (p2)
	    {
	      s = pool->solvables + p;
	      s2 = pool->solvables + p2;
	      if (s->name == s2->name && s->arch == s2->arch && s->evr == s2->evr)
		return SOLVER_TRANSACTION_MULTIREINSTALL;
	    }
	  return SOLVER_TRANSACTION_MULTIINSTALL;
	}
      if (!p2)
	return SOLVER_TRANSACTION_INSTALL;
      s = pool->solvables + p;
      s2 = pool->solvables + p2;
      if (s->name == s2->name)
	{
	  if (s->evr == s2->evr && solvable_identical(s, s2))
	    return SOLVER_TRANSACTION_REINSTALL;
	  r = pool_evrcmp(pool, s->evr, s2->evr, EVRCMP_COMPARE);
	  if (r > 0)
	    return SOLVER_TRANSACTION_UPGRADE;
	  else if (r < 0)
	    return SOLVER_TRANSACTION_DOWNGRADE;
	  else
	    return SOLVER_TRANSACTION_CHANGE;
	}
      return SOLVER_TRANSACTION_OBSOLETES;
    }
}

/* these packages do not get installed by the package manager */
static inline int
is_pseudo_package(Pool *pool, Solvable *s)
{
  const char *n = pool_id2str(pool, s->name);
  if (*n == 'p' && !strncmp(n, "patch:", 6))
    return 1;
  if (*n == 'p' && !strncmp(n, "pattern:", 8))
    return 1;
  if (*n == 'p' && !strncmp(n, "product:", 8))
    return 1;
  if (*n == 'a' && !strncmp(n, "application:", 12))
    return 1;
  return 0;
}

/* these packages will never show up installed */
static inline int
is_noinst_pseudo_package(Pool *pool, Solvable *s)
{
  const char *n = pool_id2str(pool, s->name);
  if (!strncmp(n, "patch:", 6))
    return 1;
  if (!strncmp(n, "pattern:", 8))
    {
#if defined(SUSE) && defined(ENABLE_LINKED_PKGS)
      /* unlike normal patterns, autopatterns *can* be installed (via the package link),
         so do not filter them */
      if (s->provides)
	{
	  Id prv, *prvp = s->repo->idarraydata + s->provides;
	  while ((prv = *prvp++) != 0)
	    if (ISRELDEP(prv) && !strcmp(pool_id2str(pool, prv), "autopattern()"))
	      return 0;
	}
#endif
      return 1;
    }
  return 0;
}

static int
obsoleted_by_pseudos_only(Transaction *trans, Id p)
{
  Pool *pool = trans->pool;
  Queue q;
  Id op;
  int i;

  op = transaction_obs_pkg(trans, p);
  if (op && !is_pseudo_package(pool, pool->solvables + op))
    return 0;
  queue_init(&q);
  transaction_all_obs_pkgs(trans, p, &q);
  for (i = 0; i < q.count; i++)
    if (!is_pseudo_package(pool, pool->solvables + q.elements[i]))
      break;
  i = !q.count || i < q.count ? 0 : 1;
  queue_free(&q);
  return i;
}

/*
 * return type of transaction element
 *
 * filtering is needed if either not all packages are shown
 * or replaces are not shown, as otherwise parts of the
 * transaction might not be shown to the user */

Id
transaction_type(Transaction *trans, Id p, int mode)
{
  Pool *pool = trans->pool;
  Solvable *s = pool->solvables + p;
  Queue oq, rq;
  Id type, q;
  int i, j, ref = 0;

  if (!s->repo)
    return SOLVER_TRANSACTION_IGNORE;

  /* XXX: SUSE only? */
  if (!(mode & SOLVER_TRANSACTION_KEEP_PSEUDO) && is_noinst_pseudo_package(pool, s))
    return SOLVER_TRANSACTION_IGNORE;

  type = transaction_base_type(trans, p);

  if (type == SOLVER_TRANSACTION_IGNORE)
    return SOLVER_TRANSACTION_IGNORE;	/* not part of the transaction */

  if ((mode & SOLVER_TRANSACTION_RPM_ONLY) != 0)
    {
      /* application wants to know what to feed to the package manager */
      if (!(mode & SOLVER_TRANSACTION_KEEP_PSEUDO) && is_pseudo_package(pool, s))
	return SOLVER_TRANSACTION_IGNORE;
      if (type == SOLVER_TRANSACTION_ERASE || type == SOLVER_TRANSACTION_INSTALL || type == SOLVER_TRANSACTION_MULTIINSTALL)
	return type;
      if (s->repo == pool->installed)
	{
	  /* check if we're a real package that is obsoleted by pseudos */
	  if (!is_pseudo_package(pool, s) && obsoleted_by_pseudos_only(trans, s - pool->solvables))
	    return SOLVER_TRANSACTION_ERASE;
	  return SOLVER_TRANSACTION_IGNORE;	/* ignore as we're being obsoleted */
	}
      if (type == SOLVER_TRANSACTION_MULTIREINSTALL)
	return SOLVER_TRANSACTION_MULTIINSTALL;
      return SOLVER_TRANSACTION_INSTALL;
    }

  if ((mode & SOLVER_TRANSACTION_SHOW_MULTIINSTALL) == 0)
    {
      /* application wants to make no difference between install
       * and multiinstall */
      if (type == SOLVER_TRANSACTION_MULTIINSTALL)
        type = SOLVER_TRANSACTION_INSTALL;
      if (type == SOLVER_TRANSACTION_MULTIREINSTALL)
        type = SOLVER_TRANSACTION_REINSTALL;
    }

  if ((mode & SOLVER_TRANSACTION_CHANGE_IS_REINSTALL) != 0)
    {
      /* application wants to make no difference between change
       * and reinstall */
      if (type == SOLVER_TRANSACTION_CHANGED)
	type = SOLVER_TRANSACTION_REINSTALLED;
      else if (type == SOLVER_TRANSACTION_CHANGE)
	type = SOLVER_TRANSACTION_REINSTALL;
    }

  if (type == SOLVER_TRANSACTION_ERASE || type == SOLVER_TRANSACTION_INSTALL || type == SOLVER_TRANSACTION_MULTIINSTALL)
    return type;

  if (s->repo == pool->installed && (mode & SOLVER_TRANSACTION_SHOW_ACTIVE) == 0)
    {
      /* erase element and we're showing the passive side */
      if (type == SOLVER_TRANSACTION_OBSOLETED && (mode & SOLVER_TRANSACTION_SHOW_OBSOLETES) == 0)
	type = SOLVER_TRANSACTION_ERASE;
      if (type == SOLVER_TRANSACTION_OBSOLETED && (mode & SOLVER_TRANSACTION_OBSOLETE_IS_UPGRADE) != 0)
	type = SOLVER_TRANSACTION_UPGRADED;
      return type;
    }
  if (s->repo != pool->installed && (mode & SOLVER_TRANSACTION_SHOW_ACTIVE) != 0)
    {
      /* install element and we're showing the active side */
      if (type == SOLVER_TRANSACTION_OBSOLETES && (mode & SOLVER_TRANSACTION_SHOW_OBSOLETES) == 0)
	type = SOLVER_TRANSACTION_INSTALL;
      if (type == SOLVER_TRANSACTION_OBSOLETES && (mode & SOLVER_TRANSACTION_OBSOLETE_IS_UPGRADE) != 0)
	type = SOLVER_TRANSACTION_UPGRADE;
      return type;
    }

  /* the element doesn't match the show mode */

  /* if we're showing all references, we can ignore this package */
  if ((mode & (SOLVER_TRANSACTION_SHOW_ALL|SOLVER_TRANSACTION_SHOW_OBSOLETES)) == (SOLVER_TRANSACTION_SHOW_ALL|SOLVER_TRANSACTION_SHOW_OBSOLETES))
    return SOLVER_TRANSACTION_IGNORE;

  /* we're not showing all refs. check if some other package
   * references us. If yes, it's safe to ignore this package,
   * otherwise we need to map the type */

  /* most of the time there's only one reference, so check it first */
  q = transaction_obs_pkg(trans, p);

  if ((mode & SOLVER_TRANSACTION_SHOW_OBSOLETES) == 0)
    {
      Solvable *sq = pool->solvables + q;
      if (sq->name != s->name)
	{
	  /* it's a replace but we're not showing replaces. map type. */
	  if (s->repo == pool->installed)
	    return SOLVER_TRANSACTION_ERASE;
	  else if (type == SOLVER_TRANSACTION_MULTIREINSTALL)
	    return SOLVER_TRANSACTION_MULTIINSTALL;
	  else
	    return SOLVER_TRANSACTION_INSTALL;
	}
    }

  /* if there's a match, p will be shown when q
   * is processed */
  if (transaction_obs_pkg(trans, q) == p)
    return SOLVER_TRANSACTION_IGNORE;

  /* too bad, a miss. check em all */
  queue_init(&oq);
  queue_init(&rq);
  transaction_all_obs_pkgs(trans, p, &oq);
  for (i = 0; i < oq.count; i++)
    {
      q = oq.elements[i];
      if ((mode & SOLVER_TRANSACTION_SHOW_OBSOLETES) == 0)
	{
	  Solvable *sq = pool->solvables + q;
	  if (sq->name != s->name)
	    continue;
	}
      /* check if we are referenced? */
      if ((mode & SOLVER_TRANSACTION_SHOW_ALL) != 0)
	{
	  transaction_all_obs_pkgs(trans, q, &rq);
	  for (j = 0; j < rq.count; j++)
	    if (rq.elements[j] == p)
	      {
	        ref = 1;
	        break;
	      }
	  if (ref)
	    break;
	}
      else if (transaction_obs_pkg(trans, q) == p)
        {
	  ref = 1;
	  break;
        }
    }
  queue_free(&oq);
  queue_free(&rq);

  if (!ref)
    {
      /* we're not referenced. map type */
      if (s->repo == pool->installed)
	return SOLVER_TRANSACTION_ERASE;
      else if (type == SOLVER_TRANSACTION_MULTIREINSTALL)
	return SOLVER_TRANSACTION_MULTIINSTALL;
      else
	return SOLVER_TRANSACTION_INSTALL;
    }
  /* there was a ref, so p is shown with some other package */
  return SOLVER_TRANSACTION_IGNORE;
}



static int
classify_cmp(const void *ap, const void *bp, void *dp)
{
  Transaction *trans = dp;
  Pool *pool = trans->pool;
  const Id *a = ap;
  const Id *b = bp;
  int r;

  r = a[0] - b[0];
  if (r)
    return r;
  r = a[2] - b[2];
  if (r)
    return a[2] && b[2] ? strcmp(pool_id2str(pool, a[2]), pool_id2str(pool, b[2])) : r;
  r = a[3] - b[3];
  if (r)
    return a[3] && b[3] ? strcmp(pool_id2str(pool, a[3]), pool_id2str(pool, b[3])) : r;
  return 0;
}

static int
classify_cmp_pkgs(const void *ap, const void *bp, void *dp)
{
  Transaction *trans = dp;
  Pool *pool = trans->pool;
  Id a = *(Id *)ap;
  Id b = *(Id *)bp;
  Solvable *sa, *sb;

  sa = pool->solvables + a;
  sb = pool->solvables + b;
  if (sa->name != sb->name)
    return strcmp(pool_id2str(pool, sa->name), pool_id2str(pool, sb->name));
  if (sa->evr != sb->evr)
    {
      int r = pool_evrcmp(pool, sa->evr, sb->evr, EVRCMP_COMPARE);
      if (r)
	return r;
    }
  return a - b;
}

static inline void
queue_push4(Queue *q, Id id1, Id id2, Id id3, Id id4)
{
  queue_push(q, id1);
  queue_push(q, id2);
  queue_push(q, id3);
  queue_push(q, id4);
}

static inline void
queue_unshift4(Queue *q, Id id1, Id id2, Id id3, Id id4)
{
  queue_unshift(q, id4);
  queue_unshift(q, id3);
  queue_unshift(q, id2);
  queue_unshift(q, id1);
}

void
transaction_classify(Transaction *trans, int mode, Queue *classes)
{
  Pool *pool = trans->pool;
  int ntypes[SOLVER_TRANSACTION_MAXTYPE + 1];
  Solvable *s, *sq;
  Id v, vq, type, p, q;
  int i, j;

  queue_empty(classes);
  memset(ntypes, 0, sizeof(ntypes));
  /* go through transaction and classify each step */
  for (i = 0; i < trans->steps.count; i++)
    {
      p = trans->steps.elements[i];
      s = pool->solvables + p;
      type = transaction_type(trans, p, mode);
      ntypes[type]++;
      if (!pool->installed || s->repo != pool->installed)
	continue;
      /* don't report vendor/arch changes if we were mapped to erase. */
      if (type == SOLVER_TRANSACTION_ERASE)
	continue;
      /* look at arch/vendor changes */
      q = transaction_obs_pkg(trans, p);
      if (!q)
	continue;
      sq = pool->solvables + q;

      v = s->arch;
      vq = sq->arch;
      if (v != vq)
	{
	  if ((mode & SOLVER_TRANSACTION_MERGE_ARCHCHANGES) != 0)
	    v = vq = 0;
	  for (j = 0; j < classes->count; j += 4)
	    if (classes->elements[j] == SOLVER_TRANSACTION_ARCHCHANGE && classes->elements[j + 2] == v && classes->elements[j + 3] == vq)
	      break;
	  if (j == classes->count)
	    queue_push4(classes, SOLVER_TRANSACTION_ARCHCHANGE, 1, v, vq);
	  else
	    classes->elements[j + 1]++;
	}

      v = s->vendor ? s->vendor : 1;
      vq = sq->vendor ? sq->vendor : 1;
      if (v != vq)
	{
	  if ((mode & SOLVER_TRANSACTION_MERGE_VENDORCHANGES) != 0)
	    v = vq = 0;
	  for (j = 0; j < classes->count; j += 4)
	    if (classes->elements[j] == SOLVER_TRANSACTION_VENDORCHANGE && classes->elements[j + 2] == v && classes->elements[j + 3] == vq)
	      break;
	  if (j == classes->count)
	    queue_push4(classes, SOLVER_TRANSACTION_VENDORCHANGE, 1, v, vq);
	  else
	    classes->elements[j + 1]++;
	}
    }
  /* now sort all vendor/arch changes */
  if (classes->count > 4)
    solv_sort(classes->elements, classes->count / 4, 4 * sizeof(Id), classify_cmp, trans);
  /* finally add all classes. put erases last */
  i = SOLVER_TRANSACTION_ERASE;
  if (ntypes[i])
    queue_unshift4(classes, i, ntypes[i], 0, 0);
  for (i = SOLVER_TRANSACTION_MAXTYPE; i > 0; i--)
    {
      if (!ntypes[i])
	continue;
      if (i == SOLVER_TRANSACTION_ERASE)
	continue;
      queue_unshift4(classes, i, ntypes[i], 0, 0);
    }
}

void
transaction_classify_pkgs(Transaction *trans, int mode, Id class, Id from, Id to, Queue *pkgs)
{
  Pool *pool = trans->pool;
  int i;
  Id type, p, q;
  Solvable *s, *sq;

  queue_empty(pkgs);
  for (i = 0; i < trans->steps.count; i++)
    {
      p = trans->steps.elements[i];
      s = pool->solvables + p;
      if (class <= SOLVER_TRANSACTION_MAXTYPE)
	{
	  type = transaction_type(trans, p, mode);
	  if (type == class)
	    queue_push(pkgs, p);
	  continue;
	}
      if (!pool->installed || s->repo != pool->installed)
	continue;
      q = transaction_obs_pkg(trans, p);
      if (!q)
	continue;
      sq = pool->solvables + q;
      if (class == SOLVER_TRANSACTION_ARCHCHANGE)
	{
	  if ((!from && !to) || (s->arch == from && sq->arch == to))
	    queue_push(pkgs, p);
	  continue;
	}
      if (class == SOLVER_TRANSACTION_VENDORCHANGE)
	{
	  Id v = s->vendor ? s->vendor : 1;
	  Id vq = sq->vendor ? sq->vendor : 1;
	  if ((!from && !to) || (v == from && vq == to))
	    queue_push(pkgs, p);
	  continue;
	}
    }
  if (pkgs->count > 1)
    solv_sort(pkgs->elements, pkgs->count, sizeof(Id), classify_cmp_pkgs, trans);
}

static void
create_transaction_info(Transaction *trans, Queue *decisionq)
{
  Pool *pool = trans->pool;
  Queue *ti = &trans->transaction_info;
  Repo *installed = pool->installed;
  int i, j, multi;
  Id p, p2, pp2;
  Solvable *s, *s2;

  queue_empty(ti);
  trans->transaction_installed = solv_free(trans->transaction_installed);
  if (!installed)
    return;	/* no info needed */
  for (i = 0; i < decisionq->count; i++)
    {
      p = decisionq->elements[i];
      if (p <= 0 || p == SYSTEMSOLVABLE)
	continue;
      s = pool->solvables + p;
      if (!s->repo || s->repo == installed)
	continue;
      multi = trans->multiversionmap.size && MAPTST(&trans->multiversionmap, p);
      FOR_PROVIDES(p2, pp2, s->name)
	{
	  if (!MAPTST(&trans->transactsmap, p2))
	    continue;
	  s2 = pool->solvables + p2;
	  if (s2->repo != installed)
	    continue;
	  if (multi && (s->name != s2->name || s->evr != s2->evr || s->arch != s2->arch))
	    continue;
	  if (!pool->implicitobsoleteusesprovides && s->name != s2->name)
	    continue;
	  if (pool->implicitobsoleteusescolors && !pool_colormatch(pool, s, s2))
	    continue;
	  queue_push2(ti, p, p2);
	}
      if (s->obsoletes && !multi)
	{
	  Id obs, *obsp = s->repo->idarraydata + s->obsoletes;
	  while ((obs = *obsp++) != 0)
	    {
	      FOR_PROVIDES(p2, pp2, obs)
		{
		  if (!MAPTST(&trans->transactsmap, p2))
		    continue;
		  s2 = pool->solvables + p2;
		  if (s2->repo != installed)
		    continue;
		  if (!pool->obsoleteusesprovides && !pool_match_nevr(pool, s2, obs))
		    continue;
		  if (pool->obsoleteusescolors && !pool_colormatch(pool, s, s2))
		    continue;
		  queue_push2(ti, p, p2);
		}
	    }
	}
    }
  if (ti->count > 2)
    {
      /* sort and unify */
      solv_sort(ti->elements, ti->count / 2, 2 * sizeof(Id), obsq_sortcmp, pool);
      for (i = j = 2; i < ti->count; i += 2)
	{
	  if (ti->elements[i] == ti->elements[j - 2] && ti->elements[i + 1] == ti->elements[j - 1])
	    continue;
	  ti->elements[j++] = ti->elements[i];
	  ti->elements[j++] = ti->elements[i + 1];
	}
      queue_truncate(ti, j);
    }

  /* create transaction_installed helper */
  /*   entry > 0: exactly one obsoleter, entry < 0: multiple obsoleters, -entry is "best" */
  trans->transaction_installed = solv_calloc(installed->end - installed->start, sizeof(Id));
  for (i = 0; i < ti->count; i += 2)
    {
      j = ti->elements[i + 1] - installed->start;
      if (!trans->transaction_installed[j])
	trans->transaction_installed[j] = ti->elements[i];
      else
	{
	  /* more than one package obsoletes us. compare to find "best" */
	  Id q[4];
	  if (trans->transaction_installed[j] > 0)
	    trans->transaction_installed[j] = -trans->transaction_installed[j];
	  q[0] = q[2] = ti->elements[i + 1];
	  q[1] = ti->elements[i];
	  q[3] = -trans->transaction_installed[j];
	  if (obsq_sortcmp(q, q + 2, pool) < 0)
	    trans->transaction_installed[j] = -ti->elements[i];
	}
    }
}

/* create a transaction from the decisionq */
Transaction *
transaction_create_decisionq(Pool *pool, Queue *decisionq, Map *multiversionmap)
{
  Repo *installed = pool->installed;
  int i, needmulti;
  Id p;
  Solvable *s;
  Transaction *trans;

  trans = transaction_create(pool);
  if (multiversionmap && !multiversionmap->size)
    multiversionmap = 0;	/* ignore empty map */
  queue_empty(&trans->steps);
  map_init(&trans->transactsmap, pool->nsolvables);
  needmulti = 0;
  for (i = 0; i < decisionq->count; i++)
    {
      p = decisionq->elements[i];
      s = pool->solvables + (p > 0 ? p : -p);
      if (!s->repo)
	continue;
      if (installed && s->repo == installed && p < 0)
	MAPSET(&trans->transactsmap, -p);
      if (!(installed && s->repo == installed) && p > 0)
	{
	  MAPSET(&trans->transactsmap, p);
	  if (multiversionmap && MAPTST(multiversionmap, p))
	    needmulti = 1;
	}
    }
  MAPCLR(&trans->transactsmap, SYSTEMSOLVABLE);
  if (needmulti)
    map_init_clone(&trans->multiversionmap, multiversionmap);

  create_transaction_info(trans, decisionq);

  if (installed)
    {
      FOR_REPO_SOLVABLES(installed, p, s)
	{
	  if (MAPTST(&trans->transactsmap, p))
	    queue_push(&trans->steps, p);
	}
    }
  for (i = 0; i < decisionq->count; i++)
    {
      p = decisionq->elements[i];
      if (p > 0 && MAPTST(&trans->transactsmap, p))
        queue_push(&trans->steps, p);
    }
  return trans;
}

int
transaction_installedresult(Transaction *trans, Queue *installedq)
{
  Pool *pool = trans->pool;
  Repo *installed = pool->installed;
  Solvable *s;
  int i, cutoff;
  Id p;

  queue_empty(installedq);
  /* first the new installs, than the kept packages */
  for (i = 0; i < trans->steps.count; i++)
    {
      p = trans->steps.elements[i];
      s = pool->solvables + p;
      if (installed && s->repo == installed)
	continue;
      queue_push(installedq, p);
    }
  cutoff = installedq->count;
  if (installed)
    {
      FOR_REPO_SOLVABLES(installed, p, s)
	if (!MAPTST(&trans->transactsmap, p))
          queue_push(installedq, p);
    }
  return cutoff;
}

static void
transaction_make_installedmap(Transaction *trans, Map *installedmap)
{
  Pool *pool = trans->pool;
  Repo *installed = pool->installed;
  Solvable *s;
  Id p;
  int i;

  map_init(installedmap, pool->nsolvables);
  for (i = 0; i < trans->steps.count; i++)
    {
      p = trans->steps.elements[i];
      s = pool->solvables + p;
      if (!installed || s->repo != installed)
        MAPSET(installedmap, p);
    }
  if (installed)
    {
      FOR_REPO_SOLVABLES(installed, p, s)
	if (!MAPTST(&trans->transactsmap, p))
          MAPSET(installedmap, p);
    }
}

int
transaction_calc_installsizechange(Transaction *trans)
{
  Map installedmap;
  int change;

  transaction_make_installedmap(trans, &installedmap);
  change = pool_calc_installsizechange(trans->pool, &installedmap);
  map_free(&installedmap);
  return change;
}

void
transaction_calc_duchanges(Transaction *trans, DUChanges *mps, int nmps)
{
  Map installedmap;

  transaction_make_installedmap(trans, &installedmap);
  pool_calc_duchanges(trans->pool, &installedmap, mps, nmps);
  map_free(&installedmap);
}

Transaction *
transaction_create(Pool *pool)
{
  Transaction *trans = solv_calloc(1, sizeof(*trans));
  trans->pool = pool;
  return trans;
}

Transaction *
transaction_create_clone(Transaction *srctrans)
{
  Transaction *trans = transaction_create(srctrans->pool);
  queue_init_clone(&trans->steps, &srctrans->steps);
  queue_init_clone(&trans->transaction_info, &srctrans->transaction_info);
  if (srctrans->transaction_installed)
    {
      Repo *installed = srctrans->pool->installed;
      trans->transaction_installed = solv_memdup2(srctrans->transaction_installed, installed->end - installed->start, sizeof(Id));
    }
  map_init_clone(&trans->transactsmap, &srctrans->transactsmap);
  map_init_clone(&trans->multiversionmap, &srctrans->multiversionmap);
  if (srctrans->orderdata)
    transaction_clone_orderdata(trans, srctrans);
  return trans;
}

void
transaction_free(Transaction *trans)
{
  queue_free(&trans->steps);
  queue_free(&trans->transaction_info);
  trans->transaction_installed = solv_free(trans->transaction_installed);
  map_free(&trans->transactsmap);
  map_free(&trans->multiversionmap);
  if (trans->orderdata)
    transaction_free_orderdata(trans);
  free(trans);
}

