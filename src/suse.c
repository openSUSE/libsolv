/*
 * Copyright (c) 2016, SUSE LLC.
 *
 * This program is licensed under the BSD license, read LICENSE.BSD
 * for further information
 */

/* weird SUSE stuff. better not use it for your projects. */

#include <stdio.h>
#include <stdlib.h>
#include <unistd.h>
#include <string.h>

#include "solver.h"
#include "solver_private.h"
#include "bitmap.h"
#include "pool.h"
#include "poolvendor.h"
#include "util.h"

Offset
repo_fix_supplements(Repo *repo, Offset provides, Offset supplements, Offset freshens)
{
  Pool *pool = repo->pool;
  Id id, idp, idl;
  char buf[1024], *p, *dep;
  int i, l;

  if (provides)
    {
      for (i = provides; repo->idarraydata[i]; i++)
	{
	  id = repo->idarraydata[i];
	  if (ISRELDEP(id))
	    continue;
	  dep = (char *)pool_id2str(pool, id);
	  if (!strncmp(dep, "locale(", 7) && strlen(dep) < sizeof(buf) - 2)
	    {
	      idp = 0;
	      strcpy(buf + 2, dep);
	      dep = buf + 2 + 7;
	      if ((p = strchr(dep, ':')) != 0 && p != dep)
		{
		  *p++ = 0;
		  idp = pool_str2id(pool, dep, 1);
		  dep = p;
		}
	      id = 0;
	      while ((p = strchr(dep, ';')) != 0)
		{
		  if (p == dep)
		    {
		      dep = p + 1;
		      continue;
		    }
		  *p++ = 0;
		  idl = pool_str2id(pool, dep, 1);
		  idl = pool_rel2id(pool, NAMESPACE_LANGUAGE, idl, REL_NAMESPACE, 1);
		  if (id)
		    id = pool_rel2id(pool, id, idl, REL_OR, 1);
		  else
		    id = idl;
		  dep = p;
		}
	      if (dep[0] && dep[1])
		{
	 	  for (p = dep; *p && *p != ')'; p++)
		    ;
		  *p = 0;
		  idl = pool_str2id(pool, dep, 1);
		  idl = pool_rel2id(pool, NAMESPACE_LANGUAGE, idl, REL_NAMESPACE, 1);
		  if (id)
		    id = pool_rel2id(pool, id, idl, REL_OR, 1);
		  else
		    id = idl;
		}
	      if (idp)
		id = pool_rel2id(pool, idp, id, REL_AND, 1);
	      if (id)
		supplements = repo_addid_dep(repo, supplements, id, 0);
	    }
	  else if ((p = strchr(dep, ':')) != 0 && p != dep && p[1] == '/' && strlen(dep) < sizeof(buf))
	    {
	      strcpy(buf, dep);
	      p = buf + (p - dep);
	      *p++ = 0;
	      idp = pool_str2id(pool, buf, 1);
	      /* strip trailing slashes */
	      l = strlen(p);
	      while (l > 1 && p[l - 1] == '/')
		p[--l] = 0;
	      id = pool_str2id(pool, p, 1);
	      id = pool_rel2id(pool, idp, id, REL_WITH, 1);
	      id = pool_rel2id(pool, NAMESPACE_SPLITPROVIDES, id, REL_NAMESPACE, 1);
	      supplements = repo_addid_dep(repo, supplements, id, 0);
	    }
	}
    }
  if (supplements)
    {
      for (i = supplements; repo->idarraydata[i]; i++)
	{
	  id = repo->idarraydata[i];
	  if (ISRELDEP(id))
	    continue;
	  dep = (char *)pool_id2str(pool, id);
	  if (!strncmp(dep, "system:modalias(", 16))
	    dep += 7;
	  if (!strncmp(dep, "modalias(", 9) && dep[9] && dep[10] && strlen(dep) < sizeof(buf))
	    {
	      strcpy(buf, dep);
	      p = strchr(buf + 9, ':');
	      if (p && p != buf + 9 && strchr(p + 1, ':'))
		{
		  *p++ = 0;
		  idp = pool_str2id(pool, buf + 9, 1);
		  p[strlen(p) - 1] = 0;
		  id = pool_str2id(pool, p, 1);
		  id = pool_rel2id(pool, NAMESPACE_MODALIAS, id, REL_NAMESPACE, 1);
		  id = pool_rel2id(pool, idp, id, REL_AND, 1);
		}
	      else
		{
		  p = buf + 9;
		  p[strlen(p) - 1] = 0;
		  id = pool_str2id(pool, p, 1);
		  id = pool_rel2id(pool, NAMESPACE_MODALIAS, id, REL_NAMESPACE, 1);
		}
	      if (id)
		repo->idarraydata[i] = id;
	    }
	  else if (!strncmp(dep, "packageand(", 11) && strlen(dep) < sizeof(buf))
	    {
	      strcpy(buf, dep);
	      id = 0;
	      dep = buf + 11;
	      while ((p = strchr(dep, ':')) != 0)
		{
		  if (p == dep)
		    {
		      dep = p + 1;
		      continue;
		    }
		  /* argh, allow pattern: prefix. sigh */
		  if (p - dep == 7 && !strncmp(dep, "pattern", 7))
		    {
		      p = strchr(p + 1, ':');
		      if (!p)
			break;
		    }
		  *p++ = 0;
		  idp = pool_str2id(pool, dep, 1);
		  if (id)
		    id = pool_rel2id(pool, id, idp, REL_AND, 1);
		  else
		    id = idp;
		  dep = p;
		}
	      if (dep[0] && dep[1])
		{
		  dep[strlen(dep) - 1] = 0;
		  idp = pool_str2id(pool, dep, 1);
		  if (id)
		    id = pool_rel2id(pool, id, idp, REL_AND, 1);
		  else
		    id = idp;
		}
	      if (id)
		repo->idarraydata[i] = id;
	    }
	  else if (!strncmp(dep, "filesystem(", 11) && strlen(dep) < sizeof(buf))
	    {
	      strcpy(buf, dep + 11);
	      if ((p = strrchr(buf, ')')) != 0)
		*p = 0;
	      id = pool_str2id(pool, buf, 1);
	      id = pool_rel2id(pool, NAMESPACE_FILESYSTEM, id, REL_NAMESPACE, 1);
	      repo->idarraydata[i] = id;
	    }
	}
    }
  if (freshens && repo->idarraydata[freshens])
    {
      Id idsupp = 0, idfresh = 0;
      if (!supplements || !repo->idarraydata[supplements])
	return freshens;
      for (i = supplements; repo->idarraydata[i]; i++)
        {
	  if (!idsupp)
	    idsupp = repo->idarraydata[i];
	  else
	    idsupp = pool_rel2id(pool, idsupp, repo->idarraydata[i], REL_OR, 1);
        }
      for (i = freshens; repo->idarraydata[i]; i++)
        {
	  if (!idfresh)
	    idfresh = repo->idarraydata[i];
	  else
	    idfresh = pool_rel2id(pool, idfresh, repo->idarraydata[i], REL_OR, 1);
        }
      if (!idsupp)
        idsupp = idfresh;
      else
	idsupp = pool_rel2id(pool, idsupp, idfresh, REL_AND, 1);
      supplements = repo_addid_dep(repo, 0, idsupp, 0);
    }
  return supplements;
}

Offset
repo_fix_conflicts(Repo *repo, Offset conflicts)
{
  char buf[1024], *dep;
  Pool *pool = repo->pool;
  Id id;
  int i;

  if (!conflicts)
    return conflicts;
  for (i = conflicts; repo->idarraydata[i]; i++)
    {
      id = repo->idarraydata[i];
      if (ISRELDEP(id))
	continue;
      dep = (char *)pool_id2str(pool, id);
      if (!strncmp(dep, "otherproviders(", 15) && dep[15] && strlen(dep) < sizeof(buf) - 2)
	{
	  strcpy(buf, dep + 15);
	  buf[strlen(buf) - 1] = 0;
	  id = pool_str2id(pool, buf, 1);
	  id = pool_rel2id(pool, NAMESPACE_OTHERPROVIDERS, id, REL_NAMESPACE, 1);
	  repo->idarraydata[i] = id;
	}
    }
  return conflicts;
}

void
repo_rewrite_suse_deps(Solvable *s, Offset freshens)
{
  s->supplements = repo_fix_supplements(s->repo, s->provides, s->supplements, freshens);
  if (s->conflicts)
    s->conflicts = repo_fix_conflicts(s->repo, s->conflicts);
}

/**********************************************************************************/

static inline Id
dep2name(Pool *pool, Id dep)
{
  while (ISRELDEP(dep))
    {
      Reldep *rd = GETRELDEP(pool, dep);
      dep = rd->name;
    }
  return dep;
}

static int
providedbyinstalled_multiversion(Pool *pool, Map *installed, Id n, Id con)
{
  Id p, pp;
  Solvable *sn = pool->solvables + n;

  FOR_PROVIDES(p, pp, sn->name)
    {
      Solvable *s = pool->solvables + p;
      if (s->name != sn->name || s->arch != sn->arch)
        continue;
      if (!MAPTST(installed, p))
        continue;
      if (pool_match_nevr(pool, pool->solvables + p, con))
        continue;
      return 1;         /* found installed package that doesn't conflict */
    }
  return 0;
}

static inline int
providedbyinstalled(Pool *pool, Map *installed, Id dep, int ispatch, Map *multiversionmap)
{
  Id p, pp;
  FOR_PROVIDES(p, pp, dep)
    {
      if (p == SYSTEMSOLVABLE)
	return -1;
      if (ispatch && !pool_match_nevr(pool, pool->solvables + p, dep))
	continue;
      if (ispatch && multiversionmap && multiversionmap->size && MAPTST(multiversionmap, p) && ISRELDEP(dep))
	if (providedbyinstalled_multiversion(pool, installed, p, dep))
	  continue;
      if (MAPTST(installed, p))
	return 1;
    }
  return 0;
}

/* xmap:
 *  1: installed
 *  2: conflicts with installed
 *  8: interesting (only true if installed)
 * 16: undecided
 */

static int
providedbyinstalled_multiversion_xmap(Pool *pool, unsigned char *map, Id n, Id con) 
{
  Id p, pp;
  Solvable *sn = pool->solvables + n; 

  FOR_PROVIDES(p, pp, sn->name)
    {    
      Solvable *s = pool->solvables + p; 
      if (s->name != sn->name || s->arch != sn->arch)
        continue;
      if ((map[p] & 9) != 9)
        continue;
      if (pool_match_nevr(pool, pool->solvables + p, con))
        continue;
      return 1;         /* found installed package that doesn't conflict */
    }    
  return 0;
}


static inline int
providedbyinstalled_xmap(Pool *pool, unsigned char *map, Id dep, int ispatch, Map *multiversionmap)
{
  Id p, pp;
  int r = 0; 
  FOR_PROVIDES(p, pp, dep) 
    {    
      if (p == SYSTEMSOLVABLE)
        return 1;       /* always boring, as never constraining */
      if (ispatch && !pool_match_nevr(pool, pool->solvables + p, dep))
        continue;
      if (ispatch && multiversionmap && multiversionmap->size && MAPTST(multiversionmap, p) && ISRELDEP(dep))
        if (providedbyinstalled_multiversion_xmap(pool, map, p, dep))
          continue;
      if ((map[p] & 9) == 9)
        return 9;
      r |= map[p] & 17;
    }    
  return r;
}

/* FIXME: this mirrors policy_illegal_vendorchange */
static int
pool_illegal_vendorchange(Pool *pool, Solvable *s1, Solvable *s2)
{
  Id v1, v2;
  Id vendormask1, vendormask2;

  if (pool->custom_vendorcheck)
    return pool->custom_vendorcheck(pool, s1, s2);
  /* treat a missing vendor as empty string */
  v1 = s1->vendor ? s1->vendor : ID_EMPTY;
  v2 = s2->vendor ? s2->vendor : ID_EMPTY;
  if (v1 == v2)
    return 0;
  vendormask1 = pool_vendor2mask(pool, v1);
  if (!vendormask1)
    return 1;   /* can't match */
  vendormask2 = pool_vendor2mask(pool, v2);
  if ((vendormask1 & vendormask2) != 0)
    return 0;
  return 1;     /* no class matches */
}

/* check if this patch is relevant according to the vendor. To bad that patches
 * don't have a vendor, so we need to do some careful repo testing. */
int
solvable_is_irrelevant_patch(Solvable *s, Map *installedmap)
{
  Pool *pool = s->repo->pool;
  Id con, *conp;
  int hadpatchpackage = 0;

  if (!s->conflicts)
    return 0;
  conp = s->repo->idarraydata + s->conflicts;
  while ((con = *conp++) != 0)
    {
      Reldep *rd;
      Id p, pp, p2, pp2;
      if (!ISRELDEP(con))
        continue;
      rd = GETRELDEP(pool, con);
      if (rd->flags != REL_LT)
        continue;
      FOR_PROVIDES(p, pp, con)
        {
          Solvable *si;
          if (!MAPTST(installedmap, p))
            continue;
          si = pool->solvables + p;
          if (!pool_match_nevr(pool, si, con))
            continue;
          FOR_PROVIDES(p2, pp2, rd->name)
            {
              Solvable *s2 = pool->solvables + p2;
              if (!pool_match_nevr(pool, s2, rd->name))
                continue;
              if (pool_match_nevr(pool, s2, con))
                continue;       /* does not fulfill patch */
              if (s2->repo == s->repo)
                {
                  hadpatchpackage = 1;
                  /* ok, we have a package from the patch repo that solves the conflict. check vendor */
                  if (si->vendor == s2->vendor)
                    return 0;
                  if (!pool_illegal_vendorchange(pool, si, s2))
                    return 0;
                  /* vendor change was illegal, ignore conflict */
                }
            }
        }
    }
  /* if we didn't find a patchpackage don't claim that the patch is irrelevant */
  if (!hadpatchpackage)
    return 0;
  return 1;
}

/*
 * solvable_trivial_installable_map - answers if a solvable is installable
 * without any other installs/deinstalls.
 * The packages considered to be installed are provided via the
 * installedmap bitmap. A additional "conflictsmap" bitmap providing
 * information about the conflicts of the installed packages can be
 * used for extra speed up. Provide a NULL pointer if you do not
 * have this information.
 * Both maps can be created with pool_create_state_maps() or
 * solver_create_state_maps().
 *
 * returns:
 * 1:  solvable is installable without any other package changes
 * 0:  solvable is not installable
 * -1: solvable is installable, but doesn't constrain any installed packages
 */
int
solvable_trivial_installable_map(Solvable *s, Map *installedmap, Map *conflictsmap, Map *multiversionmap)
{
  Pool *pool = s->repo->pool;
  Solvable *s2;
  Id p, *dp;
  Id *reqp, req;
  Id *conp, con;
  int r, interesting = 0;

  if (conflictsmap && MAPTST(conflictsmap, s - pool->solvables))
    return 0;
  if (s->requires)
    {
      reqp = s->repo->idarraydata + s->requires;
      while ((req = *reqp++) != 0)
	{
	  if (req == SOLVABLE_PREREQMARKER)
	    continue;
          r = providedbyinstalled(pool, installedmap, req, 0, 0);
	  if (!r)
	    return 0;
	  if (r > 0)
	    interesting = 1;
	}
    }
  if (s->conflicts)
    {
      int ispatch = 0;

      if (!strncmp("patch:", pool_id2str(pool, s->name), 6))
	ispatch = 1;
      conp = s->repo->idarraydata + s->conflicts;
      while ((con = *conp++) != 0)
	{
	  if (providedbyinstalled(pool, installedmap, con, ispatch, multiversionmap))
	    {
	      if (ispatch && solvable_is_irrelevant_patch(s, installedmap))
		return -1;
	      return 0;
	    }
	  if (!interesting && ISRELDEP(con))
	    {
              con = dep2name(pool, con);
	      if (providedbyinstalled(pool, installedmap, con, ispatch, multiversionmap))
		interesting = 1;
	    }
	}
      if (ispatch && interesting && solvable_is_irrelevant_patch(s, installedmap))
	interesting = 0;
    }
  if (!conflictsmap)
    {
      int i;

      p = s - pool->solvables;
      for (i = 1; i < pool->nsolvables; i++)
	{
	  if (!MAPTST(installedmap, i))
	    continue;
	  s2 = pool->solvables + i;
	  if (!s2->conflicts)
	    continue;
	  conp = s2->repo->idarraydata + s2->conflicts;
	  while ((con = *conp++) != 0)
	    {
	      dp = pool_whatprovides_ptr(pool, con);
	      for (; *dp; dp++)
		if (*dp == p)
		  return 0;
	    }
	}
     }
  return interesting ? 1 : -1;
}

/*
 * different interface for solvable_trivial_installable_map, where
 * the information about the installed packages is provided
 * by a queue.
 */
int
solvable_trivial_installable_queue(Solvable *s, Queue *installed, Map *multiversionmap)
{
  Pool *pool = s->repo->pool;
  int i;
  Id p;
  Map installedmap;
  int r;

  map_init(&installedmap, pool->nsolvables);
  for (i = 0; i < installed->count; i++)
    {
      p = installed->elements[i];
      if (p > 0)		/* makes it work with decisionq */
	MAPSET(&installedmap, p);
    }
  r = solvable_trivial_installable_map(s, &installedmap, 0, multiversionmap);
  map_free(&installedmap);
  return r;
}

/*
 * different interface for solvable_trivial_installable_map, where
 * the information about the installed packages is provided
 * by a repo containing the installed solvables.
 */
int
solvable_trivial_installable_repo(Solvable *s, Repo *installed, Map *multiversionmap)
{
  Pool *pool = s->repo->pool;
  Id p;
  Solvable *s2;
  Map installedmap;
  int r;

  map_init(&installedmap, pool->nsolvables);
  FOR_REPO_SOLVABLES(installed, p, s2)
    MAPSET(&installedmap, p);
  r = solvable_trivial_installable_map(s, &installedmap, 0, multiversionmap);
  map_free(&installedmap);
  return r;
}

/*
 * pool_trivial_installable - calculate if a set of solvables is
 * trivial installable without any other installs/deinstalls of
 * packages not belonging to the set.
 *
 * the state is returned in the result queue:
 * 1:  solvable is installable without any other package changes
 * 0:  solvable is not installable
 * -1: solvable is installable, but doesn't constrain any installed packages
 */

void
pool_trivial_installable_multiversionmap(Pool *pool, Map *installedmap, Queue *pkgs, Queue *res, Map *multiversionmap)
{
  int i, r, m, did;
  Id p, *dp, con, *conp, req, *reqp;
  unsigned char *map;
  Solvable *s;

  map = solv_calloc(pool->nsolvables, 1);
  for (p = 1; p < pool->nsolvables; p++)
    {
      if (!MAPTST(installedmap, p))
	continue;
      map[p] |= 9;
      s = pool->solvables + p;
      if (!s->conflicts)
	continue;
      conp = s->repo->idarraydata + s->conflicts;
      while ((con = *conp++) != 0)
	{
	  dp = pool_whatprovides_ptr(pool, con);
	  for (; *dp; dp++)
	    map[p] |= 2;	/* XXX: self conflict ? */
	}
    }
  for (i = 0; i < pkgs->count; i++)
    map[pkgs->elements[i]] = 16;

  for (i = 0, did = 0; did < pkgs->count; i++, did++)
    {
      if (i == pkgs->count)
	i = 0;
      p = pkgs->elements[i];
      if ((map[p] & 16) == 0)
	continue;
      if ((map[p] & 2) != 0)
	{
	  map[p] = 2;
	  continue;
	}
      s = pool->solvables + p;
      m = 1;
      if (s->requires)
	{
	  reqp = s->repo->idarraydata + s->requires;
	  while ((req = *reqp++) != 0)
	    {
	      if (req == SOLVABLE_PREREQMARKER)
		continue;
	      r = providedbyinstalled_xmap(pool, map, req, 0, 0);
	      if (!r)
		{
		  /* decided and miss */
		  map[p] = 2;
		  did = 0;
		  break;
		}
	      if (r == 16)
		break;	/* undecided */
	      m |= r;	/* 1 | 9 | 17 */
	    }
	  if (req)
	    continue;
	  if ((m & 9) == 9)
	    m = 9;
	}
      if (s->conflicts)
	{
	  int ispatch = 0;	/* see solver.c patch handling */

	  if (!strncmp("patch:", pool_id2str(pool, s->name), 6))
	    ispatch = 1;
	  conp = s->repo->idarraydata + s->conflicts;
	  while ((con = *conp++) != 0)
	    {
	      if ((providedbyinstalled_xmap(pool, map, con, ispatch, multiversionmap) & 1) != 0)
		{
		  map[p] = 2;
		  did = 0;
		  break;
		}
	      if ((m == 1 || m == 17) && ISRELDEP(con))
		{
		  con = dep2name(pool, con);
		  if ((providedbyinstalled_xmap(pool, map, con, ispatch, multiversionmap) & 1) != 0)
		    m = 9;
		}
	    }
	  if (con)
	    continue;	/* found a conflict */
	}
      if (m != map[p])
	{
	  map[p] = m;
	  did = 0;
	}
    }
  queue_free(res);
  queue_init_clone(res, pkgs);
  for (i = 0; i < pkgs->count; i++)
    {
      m = map[pkgs->elements[i]];
      if ((m & 9) == 9)
	r = 1;
      else if (m & 1)
	r = -1;
      else
	r = 0;
      res->elements[i] = r;
    }
  free(map);
}

void
pool_trivial_installable(Pool *pool, Map *installedmap, Queue *pkgs, Queue *res)
{
  pool_trivial_installable_multiversionmap(pool, installedmap, pkgs, res, 0);
}

void
solver_trivial_installable(Solver *solv, Queue *pkgs, Queue *res)
{
  Pool *pool = solv->pool;
  Map installedmap;
  int i;
  pool_create_state_maps(pool,  &solv->decisionq, &installedmap, 0);
  pool_trivial_installable_multiversionmap(pool, &installedmap, pkgs, res, solv->multiversion.size ? &solv->multiversion : 0);
  for (i = 0; i < res->count; i++) 
    if (res->elements[i] != -1)
      {    
        Solvable *s = pool->solvables + pkgs->elements[i];
        if (!strncmp("patch:", pool_id2str(pool, s->name), 6) && solvable_is_irrelevant_patch(s, &installedmap))
          res->elements[i] = -1;
      }    
  map_free(&installedmap);
}

void
solver_printtrivial(Solver *solv)
{
  Pool *pool = solv->pool;
  Queue in, out;
  Id p;
  const char *n; 
  Solvable *s; 
  int i;

  queue_init(&in);
  for (p = 1, s = pool->solvables + p; p < solv->pool->nsolvables; p++, s++)
    {   
      n = pool_id2str(pool, s->name);
      if (strncmp(n, "patch:", 6) != 0 && strncmp(n, "pattern:", 8) != 0)
        continue;
      queue_push(&in, p); 
    }   
  if (!in.count)
    {   
      queue_free(&in);
      return;
    }   
  queue_init(&out);
  solver_trivial_installable(solv, &in, &out);
  POOL_DEBUG(SOLV_DEBUG_RESULT, "trivial installable status:\n");
  for (i = 0; i < in.count; i++)
    POOL_DEBUG(SOLV_DEBUG_RESULT, "  %s: %d\n", pool_solvid2str(pool, in.elements[i]), out.elements[i]);
  POOL_DEBUG(SOLV_DEBUG_RESULT, "\n");
  queue_free(&in);
  queue_free(&out);
}


