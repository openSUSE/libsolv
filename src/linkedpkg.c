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
 *   in /etc/products.d. In the future we may switch to using product()
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
#include "linkedpkg.h"

#ifdef ENABLE_LINKED_PKGS

void
find_application_link(Pool *pool, Solvable *s, Id *reqidp, Queue *qr, Id *prvidp, Queue *qp)
{
  Id req = 0;
  Id prv = 0;
  Id p, pp;

  /* find appdata requires */
  if (s->requires)
    {
      Id *reqp = s->repo->idarraydata + s->requires;
      while ((req = *reqp++) != 0)            /* go through all requires */
	{
	  if (ISRELDEP(req))
	    continue;
	  if (!strncmp("appdata(", pool_id2str(pool, req), 8))
	    break;
	}
    }
  if (!req)
    return;
  /* find application-appdata provides */
  if (s->provides)
    {
      Id *prvp = s->repo->idarraydata + s->provides;
      while ((prv = *prvp++) != 0)            /* go through all provides */
	{
	  if (ISRELDEP(prv))
	    continue;
	  if (strncmp("application-appdata(", pool_id2str(pool, prv), 20))
	    continue;
	  if (!strcmp(pool_id2str(pool, prv) + 12, pool_id2str(pool, req)))
	    break;
	}
    }
  if (!prv)
    return;
  /* now link em */
  FOR_PROVIDES(p, pp, req)
    if (pool->solvables[p].repo == s->repo)
      queue_push(qr, p);
  if (reqidp)
    *reqidp = req;
  if (qp)
    {
      FOR_PROVIDES(p, pp, prv)
	if (pool->solvables[p].repo == s->repo)
	  queue_push(qp, pp);
      if (prvidp)
	*prvidp = req;
    }
}

void
find_product_link(Pool *pool, Solvable *s, Id *reqidp, Queue *qr, Id *prvidp, Queue *qp)
{
  Id n = s - pool->solvables;
  Id p, pp, namerelid;
  char *str;

  if (pool->nscallback)
    {
      Id buddy = pool->nscallback(pool, pool->nscallbackdata, NAMESPACE_PRODUCTBUDDY, n);
      if (buddy > 0 && buddy != SYSTEMSOLVABLE && buddy != n && buddy < pool->nsolvables)
	{
	  if (reqidp)
	    *reqidp = solvable_selfprovidedep(pool->solvables + buddy);
	  queue_push(qr, buddy);
	  if (prvidp)
	    *prvidp = solvable_selfprovidedep(s);
	  if (qp)
	    queue_push(qp, n);
	  return;
	}
    }
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
  else
    {
      /* find qp */
      FOR_PROVIDES(p, pp, s->name)
	{
	  Solvable *ps = pool->solvables + p;
	  if (s->name != ps->name || ps->repo != s->repo || ps->arch != s->arch || s->evr != ps->evr)
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

#endif
