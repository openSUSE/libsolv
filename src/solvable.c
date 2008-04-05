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
  Repo *repo = s->repo;
  Pool *pool;
  Repodata *data;
  int i, j, n;

  if (!repo)
    return 0;
  pool = repo->pool;
  switch(keyname)
    {
    case SOLVABLE_NAME:
      return s->name;
    case SOLVABLE_ARCH:
      return s->arch;
    case SOLVABLE_EVR:
      return s->evr;
    case SOLVABLE_VENDOR:
      return s->vendor;
    }
  n = s - pool->solvables;
  for (i = 0, data = repo->repodata; i < repo->nrepodata; i++, data++)
    {
      if (n < data->start || n >= data->end)
        continue;
      for (j = 1; j < data->nkeys; j++)
        {
          if (data->keys[j].name == keyname && (data->keys[j].type == REPOKEY_TYPE_ID || data->keys[j].type == REPOKEY_TYPE_CONSTANTID))
	    {
              Id id = repodata_lookup_id(data, n - data->start, j);
	      if (id)
		{
		  if (data->localpool)
		    id = repodata_globalize_id(data, id);
		  return id;
		}
	    }
        }
    }
  return 0;
}

const char *
solvable_lookup_str(Solvable *s, Id keyname)
{
  Repo *repo = s->repo;
  Pool *pool;
  Repodata *data;
  int i, j, n;
  const char *str;

  if (!repo)
    return 0;
  pool = repo->pool;
  switch(keyname)
    {
    case SOLVABLE_NAME:
      return id2str(pool, s->name);
    case SOLVABLE_ARCH:
      return id2str(pool, s->arch);
    case SOLVABLE_EVR:
      return id2str(pool, s->evr);
    case SOLVABLE_VENDOR:
      return id2str(pool, s->vendor);
    }
  n = s - pool->solvables;
  for (i = 0, data = repo->repodata; i < repo->nrepodata; i++, data++)
    {
      if (n < data->start || n >= data->end)
        continue;
      for (j = 1; j < data->nkeys; j++)
        {
          if (data->keys[j].name == keyname && (data->keys[j].type == REPOKEY_TYPE_ID || data->keys[j].type == REPOKEY_TYPE_CONSTANTID || data->keys[j].type == REPOKEY_TYPE_STR))
	    {
              str = repodata_lookup_str(data, n - data->start, j);
	      if (str)
		return str;
	    }
        }
    }
  return 0;
}

const char *
solvable_lookup_str_poollang(Solvable *s, Id keyname)
{
  Pool *pool;
  int i, cols;
  const char *str;
  Id *row;

  if (!s->repo)
    return repo_lookup_str(s, keyname);
  pool = s->repo->pool;
  if (!pool->nlanguages)
    return repo_lookup_str(s, keyname);
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
	  pool->languagecacheother++;
	  row = pool->languagecache + cols * (ID_NUM_INTERNAL + pool->languagecacheother++);
	}
    }
  else
    row = pool->languagecache + keyname * cols;
  row++;	/* skip keyname */
  for (i = 0; i < pool->nlanguages; i++, row++)
    {
      if (!*row)
	{
	  char *p;
	  const char *kn;

	  kn = id2str(pool, keyname);
          p = sat_malloc(strlen(kn) + strlen(pool->languages[i]) + 2);
	  sprintf(p, "%s:%s", kn, pool->languages[i]);
	  *row = str2id(pool, p, 1);
          sat_free(p);
	}
      str = repo_lookup_str(s, *row);
      if (str)
	return str;
    }
  return repo_lookup_str(s, keyname);
}

const char *
solvable_lookup_str_lang(Solvable *s, Id keyname, const char *lang)
{
  Pool *pool;
  Id id;
  char *p;
  const char *kn, *str;

  if (!s->repo)
    return repo_lookup_str(s, keyname);
  pool = s->repo->pool;
  kn = id2str(pool, keyname);
  p = sat_malloc(strlen(kn) + strlen(lang) + 2);
  sprintf(p, "%s:%s", kn, lang);
  id = str2id(pool, p, 0);
  free(p);
  if (id)
    {
      str = repo_lookup_str(s, id);
      if (str)
	return str;
    }
  return repo_lookup_str(s, keyname);
}

unsigned int
solvable_lookup_num(Solvable *s, Id keyname, unsigned int notfound)
{
  Repo *repo = s->repo;
  Pool *pool;
  Repodata *data;
  int i, j, n;

  if (!repo)
    return 0;
  pool = repo->pool;
  if (keyname == RPM_RPMDBID)
    {
      if (repo->rpmdbid)
        return repo->rpmdbid[(s - pool->solvables) - repo->start];
      return notfound;
    }
  n = s - pool->solvables;
  for (i = 0, data = repo->repodata; i < repo->nrepodata; i++, data++)
    {
      if (n < data->start || n >= data->end)
        continue;
      for (j = 1; j < data->nkeys; j++)
        {
          if (data->keys[j].name == keyname
              && (data->keys[j].type == REPOKEY_TYPE_U32
                  || data->keys[j].type == REPOKEY_TYPE_NUM
                  || data->keys[j].type == REPOKEY_TYPE_CONSTANT))
            {
              unsigned int value;
              if (repodata_lookup_num(data, n - data->start, j, &value))
                return value;
            }
        }
    }
  return notfound;
}

int
solvable_lookup_bool(Solvable *s, Id keyname)
{
  return solvable_lookup_num(s, keyname, 0) ? 1 : 0;
}

int
solvable_lookup_void(Solvable *s, Id keyname)
{
  Repo *repo = s->repo;
  Pool *pool;
  Repodata *data;
  int i, j, n;

  if (!repo)
    return 0;
  pool = repo->pool;
  n = s - pool->solvables;
  for (i = 0, data = repo->repodata; i < repo->nrepodata; i++, data++)
    {
      if (n < data->start || n >= data->end)
        continue;
      for (j = 1; j < data->nkeys; j++)
        {
          if (data->keys[j].name == keyname
              && (data->keys[j].type == REPOKEY_TYPE_VOID))
            {
              if (repodata_lookup_void(data, n - data->start, j))
                return 1;
            }
        }
    }
  return 0;
}

const unsigned char *
solvable_lookup_bin_checksum(Solvable *s, Id keyname, Id *typep)
{
  Repo *repo = s->repo;
  Pool *pool;
  Repodata *data;
  int i, j, n;

  *typep = 0;
  if (!repo)
    return 0;
  pool = repo->pool;
  n = s - pool->solvables;
  for (i = 0, data = repo->repodata; i < repo->nrepodata; i++, data++)
    {
      if (n < data->start || n >= data->end)
        continue;
      for (j = 1; j < data->nkeys; j++)
        {
          if (data->keys[j].name == keyname
              && (data->keys[j].type == REPOKEY_TYPE_MD5
		  || data->keys[j].type == REPOKEY_TYPE_SHA1
		  || data->keys[j].type == REPOKEY_TYPE_SHA256))
	    {
	      const unsigned char *chk = repodata_lookup_bin_checksum(data, n - data->start, j, typep);
	      if (chk)
		return chk;
	    }
	}
    }
  return 0;
}

const char *
solvable_lookup_checksum(Solvable *s, Id keyname, Id *typep)
{
  const unsigned char *chk = solvable_lookup_bin_checksum(s, keyname, typep);
  /* we need the repodata just as a reference for a pool */
  return chk ? repodata_chk2str(s->repo->repodata, *typep, chk) : 0;
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
      evr = id2str(pool, s->evr);
      arch = id2str(pool, s->arch);
      /* name-evr.arch.rpm */
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


static inline Id dep2name(Pool *pool, Id dep)
{
  while (ISRELDEP(dep))
    {
      Reldep *rd = rd = GETRELDEP(pool, dep);
      dep = rd->name;
    }
  return dep;
}

static inline int providedbyinstalled(Pool *pool, Repo *installed, Id dep)
{
  Id p, *pp;
  Solvable *s;
  FOR_PROVIDES(p, pp, dep)
    {
      if (p == SYSTEMSOLVABLE)
	return -1;
      s = pool->solvables + p;
      if (s->repo && s->repo == installed)
	return 1;
    }
  return 0;
}

/*
 * returns:
 * 1:  solvable is installable without any other package changes
 * 0:  solvable is not installable
 * -1: solvable is installable, but doesn't constrain any installed packages
 */
int
solvable_trivial_installable(Solvable *s, Repo *installed)
{
  Pool *pool = s->repo->pool;
  Solvable *s2;
  Id p, *pp, *dp;
  Id *reqp, req;
  Id *conp, con;
  Id *obsp, obs;
  int r, interesting = 0;

  if (s->requires)
    {
      reqp = s->repo->idarraydata + s->requires;
      while ((req = *reqp++) != 0)
	{
	  if (req == SOLVABLE_PREREQMARKER)
	    continue;
          r = providedbyinstalled(pool, installed, req);
	  if (!r)
	    return 0;
	  if (r > 0)
	    interesting = 1;
	}
    }
  if (!installed)
    return 1;
  if (s->conflicts)
    {
      conp = s->repo->idarraydata + s->conflicts;
      while ((con = *conp++) != 0)
	{
	  if (providedbyinstalled(pool, installed, con))
	    return 0;
	  if (!interesting && ISRELDEP(con))
	    {
              con = dep2name(pool, con);
	      if (providedbyinstalled(pool, installed, con))
		interesting = 1;
	    }
	}
    }
  if (s->obsoletes && s->repo != installed)
    {
      obsp = s->repo->idarraydata + s->obsoletes;
      while ((obs = *obsp++) != 0)
	{
	  if (providedbyinstalled(pool, installed, obs))
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
  FOR_REPO_SOLVABLES(installed, p, s2)
    {
      if (!s2->conflicts)
	continue;
      conp = s->repo->idarraydata + s->conflicts;
      while ((con = *conp++) != 0)
	{
	  dp = pool_whatprovides(pool, con);
	  for (; *dp; dp++)
	    if (*dp == s - pool->solvables)
	      return 0;
	}
    }
  return interesting ? 1 : -1;
}
