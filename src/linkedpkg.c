/*
 * Copyright (c) 2013, SUSE Inc.
 *
 * This program is licensed under the BSD license, read LICENSE.BSD
 * for further information
 */

/*
 * linkedpkg.c
 *
 * Linked packages are "pseudo" packages that are bound to real packages but
 * contain different information (name/summary/description). They are normally
 * somehow generated from the real packages, either when the repositories are
 * created or automatically from the packages by looking at the provides.
 *
 * We currently support:
 *
 * application:
 *   created from AppStream appdata xml in the repository (which is generated
 *   from files in /usr/share/appdata)
 *
 * product:
 *   created from product data in the repository (which is generated from files
 *   in /etc/products.d). In the future we may switch to using product()
 *   provides of packages.
 *
 * pattern:
 *   created from pattern() provides of packages.
 *
 */

#include <stdio.h>
#include <stdlib.h>
#include <unistd.h>
#include <string.h>
#include <assert.h>

#include "pool.h"
#include "repo.h"
#include "solver.h"
#include "evr.h"
#include "bitmap.h"
#include "linkedpkg.h"

#ifdef ENABLE_LINKED_PKGS

void
find_application_link(Pool *pool, Solvable *s, Id *reqidp, Queue *qr, Id *prvidp, Queue *qp)
{
  Id req = 0;
  Id prv = 0;
  Id p, pp;
  Id pkgname = 0, appdataid = 0;

  /* find appdata requires */
  if (s->requires)
    {
      Id *reqp = s->repo->idarraydata + s->requires;
      while ((req = *reqp++) != 0)            /* go through all requires */
	{
	  if (ISRELDEP(req))
	    continue;
	  if (!strncmp("appdata(", pool_id2str(pool, req), 8))
	    appdataid = req;
	  else
	    pkgname = req;
	}
    }
  req = appdataid ? appdataid : pkgname;
  if (!req)
    return;
  /* find application-appdata provides */
  if (s->provides)
    {
      Id *prvp = s->repo->idarraydata + s->provides;
      const char *reqs = pool_id2str(pool, req);
      const char *prvs;
      while ((prv = *prvp++) != 0)            /* go through all provides */
	{
	  if (ISRELDEP(prv))
	    continue;
	  prvs = pool_id2str(pool, prv);
	  if (strncmp("application-appdata(", prvs, 20))
	    continue;
	  if (appdataid)
	    {
	      if (!strcmp(prvs + 12, reqs))
		break;
	    }
	  else
	    {
	      int reqsl = strlen(reqs);
	      if (!strncmp(prvs + 20, reqs, reqsl) && !strcmp(prvs + 20 + reqsl, ")"))
		break;
	    }
	}
    }
  if (!prv)
    return;	/* huh, no provides found? */
  /* now link em */
  FOR_PROVIDES(p, pp, req)
    if (pool->solvables[p].repo == s->repo)
      if (!pkgname || pool->solvables[p].name == pkgname)
        queue_push(qr, p);
  if (!qr->count && pkgname && appdataid)
    {
      /* huh, no matching package? try without pkgname filter */
      FOR_PROVIDES(p, pp, req)
	if (pool->solvables[p].repo == s->repo)
          queue_push(qr, p);
    }
  if (qp)
    {
      FOR_PROVIDES(p, pp, prv)
	if (pool->solvables[p].repo == s->repo)
	  queue_push(qp, p);
    }
  if (reqidp)
    *reqidp = req;
  if (prvidp)
    *prvidp = prv;
}

void
find_product_link(Pool *pool, Solvable *s, Id *reqidp, Queue *qr, Id *prvidp, Queue *qp)
{
  Id p, pp, namerelid;
  char *str;
  unsigned int sbt = 0;

  /* search for project requires */
  namerelid = 0;
  if (s->requires)
    {
      Id req, *reqp = s->repo->idarraydata + s->requires;
      const char *nn = pool_id2str(pool, s->name);
      int nnl = strlen(nn);
      while ((req = *reqp++) != 0)            /* go through all requires */
	if (ISRELDEP(req))
	  {
	    const char *rn;
	    Reldep *rd = GETRELDEP(pool, req);
	    if (rd->flags != REL_EQ || rd->evr != s->evr)
	      continue;
	    rn = pool_id2str(pool, rd->name);
	    if (!strncmp(rn, "product(", 8) && !strncmp(rn + 8, nn + 8, nnl - 8) && !strcmp( rn + nnl, ")"))
	      {
		namerelid = req;
		break;
	      }
	  }
    }
  if (!namerelid)
    {
      /* too bad. construct from scratch */
      str = pool_tmpjoin(pool, pool_id2str(pool, s->name), ")", 0);
      str[7] = '(';
      namerelid = pool_rel2id(pool, pool_str2id(pool, str, 1), s->evr, REL_EQ, 1);
    }
  FOR_PROVIDES(p, pp, namerelid)
    {
      Solvable *ps = pool->solvables + p;
      if (ps->repo != s->repo || ps->arch != s->arch)
	continue;
      queue_push(qr, p);
    }
  if (qr->count > 1)
    {
      /* multiple providers. try buildtime filter */
      sbt = solvable_lookup_num(s, SOLVABLE_BUILDTIME, 0);
      if (sbt)
	{
	  unsigned int bt;
	  int i, j;
	  int filterqp = 1;
	  for (i = j = 0; i < qr->count; i++)
	    {
	      bt = solvable_lookup_num(pool->solvables + qr->elements[i], SOLVABLE_BUILDTIME, 0);
	      if (!bt)
		filterqp = 0;	/* can't filter */
	      if (!bt || bt == sbt)
		qr->elements[j++] = qr->elements[i];
	    }
	  if (j)
	    qr->count = j;
	  if (!j || !filterqp)
	    sbt = 0;	/* filter failed */
	}
    }
  if (!qr->count && s->repo == pool->installed)
    {
      /* oh no! Look up reference file */
      Dataiterator di;
      const char *refbasename = solvable_lookup_str(s, PRODUCT_REFERENCEFILE);
      dataiterator_init(&di, pool, s->repo, 0, SOLVABLE_FILELIST, refbasename, SEARCH_STRING);
      while (dataiterator_step(&di))
	queue_push(qr, di.solvid);
      dataiterator_free(&di);
      if (qp)
	{
	  dataiterator_init(&di, pool, s->repo, 0, PRODUCT_REFERENCEFILE, refbasename, SEARCH_STRING);
	  while (dataiterator_step(&di))
	    queue_push(qp, di.solvid);
	  dataiterator_free(&di);
	}
    }
  else if (qp)
    {
      /* find qp */
      FOR_PROVIDES(p, pp, s->name)
	{
	  Solvable *ps = pool->solvables + p;
	  if (s->name != ps->name || ps->repo != s->repo || ps->arch != s->arch || s->evr != ps->evr)
	    continue;
	  if (sbt && solvable_lookup_num(ps, SOLVABLE_BUILDTIME, 0) != sbt)
	    continue;
	  queue_push(qp, p);
	}
    }
  if (reqidp)
    *reqidp = namerelid;
  if (prvidp)
    *prvidp = solvable_selfprovidedep(s);
}

void
find_pattern_link(Pool *pool, Solvable *s, Id *reqidp, Queue *qr, Id *prvidp, Queue *qp)
{
  Id p, pp, *pr, apevr = 0, aprel = 0;

  /* check if autopattern */
  if (!s->provides)
    return;
  for (pr = s->repo->idarraydata + s->provides; (p = *pr++) != 0; )
    if (ISRELDEP(p))
      {
	Reldep *rd = GETRELDEP(pool, p);
	if (rd->flags == REL_EQ && !strcmp(pool_id2str(pool, rd->name), "autopattern()"))
	  {
	    aprel = p;
	    apevr = rd->evr;
	    break;
	  }
      }
  if (!apevr)
    return;
  FOR_PROVIDES(p, pp, apevr)
    {
      Solvable *s2 = pool->solvables + p;
      if (s2->repo == s->repo && s2->name == apevr && s2->evr == s->evr && s2->vendor == s->vendor)
        queue_push(qr, p);
    }
  if (qp)
    {
      FOR_PROVIDES(p, pp, aprel)
	{
	  Solvable *s2 = pool->solvables + p;
	  if (s2->repo == s->repo && s2->evr == s->evr && s2->vendor == s->vendor)
	    queue_push(qp, p);
	}
    }
  if (reqidp)
    *reqidp = apevr;
  if (prvidp)
    *prvidp = aprel;
}

/* the following two functions are used in solvable_lookup_str_base to do
 * translated lookups on the product/pattern packages
 */
Id
find_autopattern_name(Pool *pool, Solvable *s)
{
  Id prv, *prvp;
  if (!s->provides)
    return 0;
  for (prvp = s->repo->idarraydata + s->provides; (prv = *prvp++) != 0; )
    if (ISRELDEP(prv))
      {
        Reldep *rd = GETRELDEP(pool, prv);
        if (rd->flags == REL_EQ && !strcmp(pool_id2str(pool, rd->name), "autopattern()"))
          return strncmp(pool_id2str(pool, rd->evr), "pattern:", 8) != 0 ? rd->evr : 0;
      }
  return 0;
}

Id
find_autoproduct_name(Pool *pool, Solvable *s)
{
  Id prv, *prvp;
  if (!s->provides)
    return 0;
  for (prvp = s->repo->idarraydata + s->provides; (prv = *prvp++) != 0; )
    if (ISRELDEP(prv))
      {
        Reldep *rd = GETRELDEP(pool, prv);
        if (rd->flags == REL_EQ && !strcmp(pool_id2str(pool, rd->name), "autoproduct()"))
          return strncmp(pool_id2str(pool, rd->evr), "product:", 8) != 0 ? rd->evr : 0;
      }
  return 0;
}

void
find_package_link(Pool *pool, Solvable *s, Id *reqidp, Queue *qr, Id *prvidp, Queue *qp)
{
  const char *name = pool_id2str(pool, s->name);
  if (name[0] == 'a' && !strncmp("application:", name, 12))
    find_application_link(pool, s, reqidp, qr, prvidp, qp);
  else if (name[0] == 'p' && !strncmp("pattern:", name, 7))
    find_pattern_link(pool, s, reqidp, qr, prvidp, qp);
  else if (name[0] == 'p' && !strncmp("product:", name, 8))
    find_product_link(pool, s, reqidp, qr, prvidp, qp);
}

static int
name_min_max(Pool *pool, Solvable *s, Id *namep, Id *minp, Id *maxp)
{
  Queue q;
  Id qbuf[4];
  Id name, min, max;
  int i;

  queue_init_buffer(&q, qbuf, sizeof(qbuf)/sizeof(*qbuf));
  find_package_link(pool, s, 0, &q, 0, 0);
  if (!q.count)
    {
      queue_free(&q);
      return 0;
    }
  s = pool->solvables + q.elements[0];
  name = s->name;
  min = max = s->evr;
  for (i = 1; i < q.count; i++)
    {
      s = pool->solvables + q.elements[i];
      if (s->name != name)
	{
          queue_free(&q);
	  return 0;
	}
      if (s->evr == min || s->evr == max)
	continue;
      if (pool_evrcmp(pool, min, s->evr, EVRCMP_COMPARE) >= 0)
	min = s->evr;
      else if (min == max || pool_evrcmp(pool, max, s->evr, EVRCMP_COMPARE) <= 0)
	max = s->evr;
    }
  queue_free(&q);
  *namep = name;
  *minp = min;
  *maxp = max;
  return 1;
}

int
pool_link_evrcmp(Pool *pool, Solvable *s1, Solvable *s2)
{
  Id name1, evrmin1, evrmax1;
  Id name2, evrmin2, evrmax2;

  if (s1->name != s2->name)
    return 0;	/* can't compare */
  if (!name_min_max(pool, s1, &name1, &evrmin1, &evrmax1))
    return 0;
  if (!name_min_max(pool, s2, &name2, &evrmin2, &evrmax2))
    return 0;
  /* compare linked names */
  if (name1 != name2)
    return 0;
  if (evrmin1 == evrmin2 && evrmax1 == evrmax2)
    return 0;
  /* now compare evr intervals */
  if (evrmin1 == evrmax1 && evrmin2 == evrmax2)
    return pool_evrcmp(pool, evrmin1, evrmax2, EVRCMP_COMPARE);
  if (evrmin1 != evrmax2 && pool_evrcmp(pool, evrmin1, evrmax2, EVRCMP_COMPARE) > 0)
    return 1;
  if (evrmax1 != evrmin2 && pool_evrcmp(pool, evrmax1, evrmin2, EVRCMP_COMPARE) < 0)
    return -1;
  return 0;
}

void
extend_updatemap_to_buddies(Solver *solv)
{
  Pool *pool = solv->pool;
  Repo *installed = solv->installed;
  Solvable *s;
  int p, ip;

  if (!installed)
    return;
  if (!solv->updatemap.size || !solv->instbuddy)
    return;
  FOR_REPO_SOLVABLES(installed, p, s)
    {
      if (!MAPTST(&solv->updatemap, p - installed->start))
	continue;
      if ((ip = solv->instbuddy[p - installed->start]) <= 1)
	continue;
      if (!has_package_link(pool, s))	/* only look at pseudo -> real relations */
	continue;
      if (ip < installed->start || ip >= installed->end || pool->solvables[ip].repo != installed)
	continue;			/* just in case... */
      MAPSET(&solv->updatemap, ip - installed->start);
    }
}

#endif
