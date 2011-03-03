/*
 * Copyright (c) 2008, Novell Inc.
 *
 * This program is licensed under the BSD license, read LICENSE.BSD
 * for further information
 */

/*
 * solvable.c
 *
 * set/retrieve data from solvables
 */

#include <stdio.h>
#include <stdlib.h>
#include <stdarg.h>
#include <unistd.h>
#include <string.h>

#include "pool.h"
#include "repo.h"
#include "util.h"
#include "chksum.h"

const char *
solvable2str(Pool *pool, Solvable *s)
{
  const char *n, *e, *a;
  char *p;
  n = id2str(pool, s->name);
  e = id2str(pool, s->evr);
  a = id2str(pool, s->arch);
  p = pool_alloctmpspace(pool, strlen(n) + strlen(e) + strlen(a) + 3);
  sprintf(p, "%s-%s.%s", n, e, a);
  return p;
}

Id
solvable_lookup_id(Solvable *s, Id keyname)
{
  if (!s->repo)
    return 0;
  return repo_lookup_id(s->repo, s - s->repo->pool->solvables, keyname);
}

int
solvable_lookup_idarray(Solvable *s, Id keyname, Queue *q)
{
  Dataiterator di;
  int found = 0;

  queue_empty(q);
  if (!s->repo)
    return 0;
  dataiterator_init(&di, s->repo->pool, s->repo, s - s->repo->pool->solvables, keyname, 0, SEARCH_ARRAYSENTINEL);
  while (dataiterator_step(&di))
    {
      if (di.key->type != REPOKEY_TYPE_IDARRAY && di.key->type != REPOKEY_TYPE_REL_IDARRAY)
	continue;
      found = 1;
      if (di.kv.eof)
	break;
      queue_push(q, di.kv.id);
    }
  dataiterator_free(&di);
  return found;
}

const char *
solvable_lookup_str(Solvable *s, Id keyname)
{
  if (!s->repo)
    return 0;
  return repo_lookup_str(s->repo, s - s->repo->pool->solvables, keyname);
}

static const char *
solvable_lookup_str_base(Solvable *s, Id keyname, Id basekeyname, int usebase)
{
  Pool *pool;
  const char *str, *basestr;
  Id p, pp;
  Solvable *s2;
  int pass;

  if (!s->repo)
    return 0;
  pool = s->repo->pool;
  str = solvable_lookup_str(s, keyname);
  if (str || keyname == basekeyname)
    return str;
  basestr = solvable_lookup_str(s, basekeyname);
  if (!basestr)
    return 0;
  /* search for a solvable with same name and same base that has the
   * translation */
  if (!pool->whatprovides)
    return usebase ? basestr : 0;
  /* we do this in two passes, first same vendor, then all other vendors */
  for (pass = 0; pass < 2; pass++)
    {
      FOR_PROVIDES(p, pp, s->name)
	{
	  s2 = pool->solvables + p;
	  if (s2->name != s->name)
	    continue;
	  if ((s->vendor == s2->vendor) != (pass == 0))
	    continue;
	  str = solvable_lookup_str(s2, basekeyname);
	  if (!str || strcmp(str, basestr))
	    continue;
	  str = solvable_lookup_str(s2, keyname);
	  if (str)
	    return str;
	}
    }
  return usebase ? basestr : 0;
}

const char *
solvable_lookup_str_poollang(Solvable *s, Id keyname)
{
  Pool *pool;
  int i, cols;
  const char *str;
  Id *row;

  if (!s->repo)
    return 0;
  pool = s->repo->pool;
  if (!pool->nlanguages)
    return solvable_lookup_str(s, keyname);
  cols = pool->nlanguages + 1;
  if (!pool->languagecache)
    {
      pool->languagecache = sat_calloc(cols * ID_NUM_INTERNAL, sizeof(Id));
      pool->languagecacheother = 0;
    }
  if (keyname >= ID_NUM_INTERNAL)
    {
      row = pool->languagecache + ID_NUM_INTERNAL * cols;
      for (i = 0; i < pool->languagecacheother; i++, row += cols)
	if (*row == keyname)
	  break;
      if (i >= pool->languagecacheother)
	{
	  pool->languagecache = sat_realloc2(pool->languagecache, pool->languagecacheother + 1, cols * sizeof(Id));
	  row = pool->languagecache + cols * (ID_NUM_INTERNAL + pool->languagecacheother++);
	  *row = keyname;
	}
    }
  else
    row = pool->languagecache + keyname * cols;
  row++;	/* skip keyname */
  for (i = 0; i < pool->nlanguages; i++, row++)
    {
      if (!*row)
        *row = pool_id2langid(pool, keyname, pool->languages[i], 1);
      str = solvable_lookup_str_base(s, *row, keyname, 0);
      if (str)
	return str;
    }
  return solvable_lookup_str(s, keyname);
}

const char *
solvable_lookup_str_lang(Solvable *s, Id keyname, const char *lang, int usebase)
{
  if (s->repo)
    {
      Id id = pool_id2langid(s->repo->pool, keyname, lang, 0);
      if (id)
        return solvable_lookup_str_base(s, id, keyname, usebase);
      if (!usebase)
	return 0;
    }
  return solvable_lookup_str(s, keyname);
}

unsigned int
solvable_lookup_num(Solvable *s, Id keyname, unsigned int notfound)
{
  if (!s->repo)
    return notfound;
  return repo_lookup_num(s->repo, s - s->repo->pool->solvables, keyname, notfound);
}

int
solvable_lookup_void(Solvable *s, Id keyname)
{
  if (!s->repo)
    return 0;
  return repo_lookup_void(s->repo, s - s->repo->pool->solvables, keyname);
}

int
solvable_lookup_bool(Solvable *s, Id keyname)
{
  if (!s->repo)
    return 0;
  /* historic nonsense: there are two ways of storing a bool, as num == 1 or void. test both. */
  if (repo_lookup_type(s->repo, s - s->repo->pool->solvables, keyname) == REPOKEY_TYPE_VOID)
    return 1;
  return repo_lookup_num(s->repo, s - s->repo->pool->solvables, keyname, 0) == 1;
}

const unsigned char *
solvable_lookup_bin_checksum(Solvable *s, Id keyname, Id *typep)
{
  Repo *repo = s->repo;

  if (!repo)
    {
      *typep = 0;
      return 0;
    }
  return repo_lookup_bin_checksum(repo, s - repo->pool->solvables, keyname, typep);
}

const char *
solvable_lookup_checksum(Solvable *s, Id keyname, Id *typep)
{
  const unsigned char *chk = solvable_lookup_bin_checksum(s, keyname, typep);
  return chk ? pool_bin2hex(s->repo->pool, chk, sat_chksum_len(*typep)) : 0;
}

static inline const char *
evrid2vrstr(Pool *pool, Id evrid)
{
  const char *p, *evr = id2str(pool, evrid);
  if (!evr)
    return evr;
  for (p = evr; *p >= '0' && *p <= '9'; p++)
    ;
  return p != evr && *p == ':' ? p + 1 : evr;
}

char *
solvable_get_location(Solvable *s, unsigned int *medianrp)
{
  Pool *pool;
  int l = 0;
  char *loc;
  const char *mediadir, *mediafile;

  *medianrp = 0;
  if (!s->repo)
    return 0;
  pool = s->repo->pool;
  *medianrp = solvable_lookup_num(s, SOLVABLE_MEDIANR, 1);
  if (solvable_lookup_void(s, SOLVABLE_MEDIADIR))
    mediadir = id2str(pool, s->arch);
  else
    mediadir = solvable_lookup_str(s, SOLVABLE_MEDIADIR);
  if (mediadir)
    l = strlen(mediadir) + 1;
  if (solvable_lookup_void(s, SOLVABLE_MEDIAFILE))
    {
      const char *name, *evr, *arch;
      name = id2str(pool, s->name);
      evr = evrid2vrstr(pool, s->evr);
      arch = id2str(pool, s->arch);
      /* name-vr.arch.rpm */
      loc = pool_alloctmpspace(pool, l + strlen(name) + strlen(evr) + strlen(arch) + 7);
      if (mediadir)
	sprintf(loc, "%s/%s-%s.%s.rpm", mediadir, name, evr, arch);
      else
	sprintf(loc, "%s-%s.%s.rpm", name, evr, arch);
    }
  else
    {
      mediafile = solvable_lookup_str(s, SOLVABLE_MEDIAFILE);
      if (!mediafile)
	return 0;
      loc = pool_alloctmpspace(pool, l + strlen(mediafile) + 1);
      if (mediadir)
	sprintf(loc, "%s/%s", mediadir, mediafile);
      else
	strcpy(loc, mediafile);
    }
  return loc;
}

/*****************************************************************************/

static inline Id dep2name(Pool *pool, Id dep)
{
  while (ISRELDEP(dep))
    {
      Reldep *rd = rd = GETRELDEP(pool, dep);
      dep = rd->name;
    }
  return dep;
}

static inline int providedbyinstalled(Pool *pool, Map *installed, Id dep)
{
  Id p, pp;
  FOR_PROVIDES(p, pp, dep)
    {
      if (p == SYSTEMSOLVABLE)
	return -1;
      if (MAPTST(installed, p))
	return 1;
    }
  return 0;
}

/*
 * solvable_trivial_installable_map - anwers is a solvable is installable
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
solvable_trivial_installable_map(Solvable *s, Map *installedmap, Map *conflictsmap)
{
  Pool *pool = s->repo->pool;
  Solvable *s2;
  Id p, pp, *dp;
  Id *reqp, req;
  Id *conp, con;
  Id *obsp, obs;
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
          r = providedbyinstalled(pool, installedmap, req);
	  if (!r)
	    return 0;
	  if (r > 0)
	    interesting = 1;
	}
    }
  if (s->conflicts)
    {
      conp = s->repo->idarraydata + s->conflicts;
      while ((con = *conp++) != 0)
	{
	  if (providedbyinstalled(pool, installedmap, con))
	    return 0;
	  if (!interesting && ISRELDEP(con))
	    {
              con = dep2name(pool, con);
	      if (providedbyinstalled(pool, installedmap, con))
		interesting = 1;
	    }
	}
    }
  if (s->repo)
    {
      Repo *installed = 0;
      if (s->obsoletes && s->repo != installed)
	{
	  obsp = s->repo->idarraydata + s->obsoletes;
	  while ((obs = *obsp++) != 0)
	    {
	      if (providedbyinstalled(pool, installedmap, obs))
		return 0;
	    }
	}
      if (s->repo != installed)
	{
	  FOR_PROVIDES(p, pp, s->name)
	    {
	      s2 = pool->solvables + p;
	      if (s2->repo == installed && s2->name == s->name)
		return 0;
	    }
	}
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
solvable_trivial_installable_queue(Solvable *s, Queue *installed)
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
  r = solvable_trivial_installable_map(s, &installedmap, 0);
  map_free(&installedmap);
  return r;
}

/*
 * different interface for solvable_trivial_installable_map, where
 * the information about the installed packages is provided
 * by a repo containing the installed solvables.
 */
int
solvable_trivial_installable_repo(Solvable *s, Repo *installed)
{
  Pool *pool = s->repo->pool;
  Id p;
  Solvable *s2;
  Map installedmap;
  int r;

  map_init(&installedmap, pool->nsolvables);
  FOR_REPO_SOLVABLES(installed, p, s2)
    MAPSET(&installedmap, p);
  r = solvable_trivial_installable_map(s, &installedmap, 0);
  map_free(&installedmap);
  return r;
}


/*****************************************************************************/

/*
 * Create maps containing the state of each solvable. Input is a "installed" queue,
 * it contains all solvable ids that are considered to be installed.
 * 
 * The created maps can be used for solvable_trivial_installable_map(),
 * pool_calc_duchanges(), pool_calc_installsizechange().
 *
 */
void
pool_create_state_maps(Pool *pool, Queue *installed, Map *installedmap, Map *conflictsmap)
{
  int i;
  Solvable *s;
  Id p, *dp;
  Id *conp, con;

  map_init(installedmap, pool->nsolvables);
  if (conflictsmap)
    map_init(conflictsmap, pool->nsolvables);
  for (i = 0; i < installed->count; i++)
    {
      p = installed->elements[i];
      if (p <= 0)	/* makes it work with decisionq */
	continue;
      MAPSET(installedmap, p);
      if (!conflictsmap)
	continue;
      s = pool->solvables + p;
      if (!s->conflicts)
	continue;
      conp = s->repo->idarraydata + s->conflicts;
      while ((con = *conp++) != 0)
	{
	  dp = pool_whatprovides_ptr(pool, con);
	  for (; *dp; dp++)
	    MAPSET(conflictsmap, *dp);
	}
    }
}

/* Tests if two solvables have identical content. Currently
 * both solvables need to come from the same pool */

int
solvable_identical(Solvable *s1, Solvable *s2)
{
  unsigned int bt1, bt2;
  Id rq1, rq2;
  Id *reqp;

  if (s1->name != s2->name)
    return 0;
  if (s1->arch != s2->arch)
    return 0;
  if (s1->evr != s2->evr)
    return 0;
  /* map missing vendor to empty string */
  if ((s1->vendor ? s1->vendor : 1) != (s2->vendor ? s2->vendor : 1))
    return 0;

  /* looking good, try some fancier stuff */
  /* might also look up the package checksum here */
  bt1 = solvable_lookup_num(s1, SOLVABLE_BUILDTIME, 0);
  bt2 = solvable_lookup_num(s2, SOLVABLE_BUILDTIME, 0);
  if (bt1 && bt2)
    {
      if (bt1 != bt2)
        return 0;
    }
  else
    {
      /* look at requires in a last attempt to find recompiled packages */
      rq1 = rq2 = 0;
      if (s1->requires)
	for (reqp = s1->repo->idarraydata + s1->requires; *reqp; reqp++)
	  rq1 ^= *reqp;
      if (s2->requires)
	for (reqp = s2->repo->idarraydata + s2->requires; *reqp; reqp++)
	  rq2 ^= *reqp;
      if (rq1 != rq2)
	 return 0;
    }
  return 1;
}

/* return the self provide dependency of a solvable */
Id
solvable_selfprovidedep(Solvable *s)
{
  Pool *pool;
  Reldep *rd;
  Id prov, *provp;

  if (!s->repo)
    return s->name;
  pool = s->repo->pool;
  if (s->provides)
    {
      provp = s->repo->idarraydata + s->provides;
      while ((prov = *provp++) != 0)
	{
	  if (!ISRELDEP(prov))
	    continue;
	  rd = GETRELDEP(pool, prov);
	  if (rd->name == s->name && rd->evr == s->evr && rd->flags == REL_EQ)
	    return prov;
	}
    }
  return rel2id(pool, s->name, s->evr, REL_EQ, 1);
}
