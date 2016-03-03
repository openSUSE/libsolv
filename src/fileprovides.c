/*
 * Copyright (c) 2007-2016, SUSE LLC
 *
 * This program is licensed under the BSD license, read LICENSE.BSD
 * for further information
 */

/*
 * fileprovides.c
 *
 * Add missing file dependencies to the package provides 
 */

#include <stdio.h>
#include <stdlib.h>
#include <stdarg.h>
#include <unistd.h>
#include <string.h>

#include "pool.h"
#include "repo.h"
#include "util.h"
#include "bitmap.h"

struct searchfiles {
  Id *ids;
  int nfiles;
  Map seen;
};

#define SEARCHFILES_BLOCK 127

static void
pool_addfileprovides_dep(Pool *pool, Id *ida, struct searchfiles *sf, struct searchfiles *isf)
{
  Id dep, sid;
  const char *s;
  struct searchfiles *csf;

  while ((dep = *ida++) != 0)
    {
      csf = sf;
      while (ISRELDEP(dep))
	{
	  Reldep *rd;
	  sid = pool->ss.nstrings + GETRELID(dep);
	  if (MAPTST(&csf->seen, sid))
	    {
	      dep = 0;
	      break;
	    }
	  MAPSET(&csf->seen, sid);
	  rd = GETRELDEP(pool, dep);
	  if (rd->flags < 8)
	    dep = rd->name;
	  else if (rd->flags == REL_NAMESPACE)
	    {
	      if (rd->name == NAMESPACE_SPLITPROVIDES)
		{
		  csf = isf;
		  if (!csf || MAPTST(&csf->seen, sid))
		    {
		      dep = 0;
		      break;
		    }
		  MAPSET(&csf->seen, sid);
		}
	      dep = rd->evr;
	    }
	  else if (rd->flags == REL_FILECONFLICT)
	    {
	      dep = 0;
	      break;
	    }
	  else
	    {
	      Id ids[2];
	      ids[0] = rd->name;
	      ids[1] = 0;
	      pool_addfileprovides_dep(pool, ids, csf, isf);
	      dep = rd->evr;
	    }
	}
      if (!dep)
	continue;
      if (MAPTST(&csf->seen, dep))
	continue;
      MAPSET(&csf->seen, dep);
      s = pool_id2str(pool, dep);
      if (*s != '/')
	continue;
      if (csf != isf && pool->addedfileprovides == 1 && !repodata_filelistfilter_matches(0, s))
	continue;	/* skip non-standard locations csf == isf: installed case */
      csf->ids = solv_extend(csf->ids, csf->nfiles, 1, sizeof(Id), SEARCHFILES_BLOCK);
      csf->ids[csf->nfiles++] = dep;
    }
}

struct addfileprovides_cbdata {
  int nfiles;
  Id *ids;
  char **dirs;
  char **names;

  Id *dids;

  Map providedids;

  Map useddirs;
};

static int
addfileprovides_cb(void *cbdata, Solvable *s, Repodata *data, Repokey *key, KeyValue *value)
{
  struct addfileprovides_cbdata *cbd = cbdata;
  int i;

  if (!cbd->useddirs.size)
    {
      map_init(&cbd->useddirs, data->dirpool.ndirs + 1);
      if (!cbd->dirs)
	{
	  cbd->dirs = solv_malloc2(cbd->nfiles, sizeof(char *));
	  cbd->names = solv_malloc2(cbd->nfiles, sizeof(char *));
	  for (i = 0; i < cbd->nfiles; i++)
	    {
	      char *s = solv_strdup(pool_id2str(data->repo->pool, cbd->ids[i]));
	      cbd->dirs[i] = s;
	      s = strrchr(s, '/');
	      *s = 0;
	      cbd->names[i] = s + 1;
	    }
	}
      for (i = 0; i < cbd->nfiles; i++)
	{
	  Id did;
	  if (MAPTST(&cbd->providedids, cbd->ids[i]))
	    {
	      cbd->dids[i] = 0;
	      continue;
	    }
	  did = repodata_str2dir(data, cbd->dirs[i], 0);
	  cbd->dids[i] = did;
	  if (did)
	    MAPSET(&cbd->useddirs, did);
	}
      repodata_free_dircache(data);
    }
  if (value->id >= data->dirpool.ndirs || !MAPTST(&cbd->useddirs, value->id))
    return 0;
  for (i = 0; i < cbd->nfiles; i++)
    if (cbd->dids[i] == value->id && !strcmp(cbd->names[i], value->str))
      s->provides = repo_addid_dep(s->repo, s->provides, cbd->ids[i], SOLVABLE_FILEMARKER);
  return 0;
}

static void
pool_addfileprovides_search(Pool *pool, struct addfileprovides_cbdata *cbd, struct searchfiles *sf, Repo *repoonly)
{
  Id p;
  Repodata *data;
  Repo *repo;
  Queue fileprovidesq;
  int i, j, repoid, repodataid;
  int provstart, provend;
  Map donemap;
  int ndone, incomplete;

  if (!pool->urepos)
    return;

  cbd->nfiles = sf->nfiles;
  cbd->ids = sf->ids;
  cbd->dirs = 0;
  cbd->names = 0;
  cbd->dids = solv_realloc2(cbd->dids, sf->nfiles, sizeof(Id));
  map_init(&cbd->providedids, pool->ss.nstrings);

  repoid = 1;
  repo = repoonly ? repoonly : pool->repos[repoid];
  map_init(&donemap, pool->nsolvables);
  queue_init(&fileprovidesq);
  provstart = provend = 0;
  for (;;)
    {
      if (!repo || repo->disabled)
	{
	  if (repoonly || ++repoid == pool->nrepos)
	    break;
	  repo = pool->repos[repoid];
	  continue;
	}
      ndone = 0;
      FOR_REPODATAS(repo, repodataid, data)
	{
	  if (ndone >= repo->nsolvables)
	    break;

	  if (repodata_lookup_idarray(data, SOLVID_META, REPOSITORY_ADDEDFILEPROVIDES, &fileprovidesq))
	    {
	      map_empty(&cbd->providedids);
	      for (i = 0; i < fileprovidesq.count; i++)
		MAPSET(&cbd->providedids, fileprovidesq.elements[i]);
	      provstart = data->start;
	      provend = data->end;
	      for (i = 0; i < cbd->nfiles; i++)
		if (!MAPTST(&cbd->providedids, cbd->ids[i]))
		  break;
	      if (i == cbd->nfiles)
		{
		  /* great! no need to search files */
		  for (p = data->start; p < data->end; p++)
		    if (pool->solvables[p].repo == repo)
		      {
			if (MAPTST(&donemap, p))
			  continue;
			MAPSET(&donemap, p);
			ndone++;
		      }
		  continue;
		}
	    }

	  if (!repodata_has_keyname(data, SOLVABLE_FILELIST))
	    continue;

	  if (data->start < provstart || data->end > provend)
	    {
	      map_empty(&cbd->providedids);
	      provstart = provend = 0;
	    }

	  /* check if the data is incomplete */
	  incomplete = 0;
	  if (data->state == REPODATA_AVAILABLE)
	    {
	      for (j = 1; j < data->nkeys; j++)
		if (data->keys[j].name != REPOSITORY_SOLVABLES && data->keys[j].name != SOLVABLE_FILELIST)
		  break;
	      if (j < data->nkeys)
		{
#if 0
		  for (i = 0; i < cbd->nfiles; i++)
		    if (!MAPTST(&cbd->providedids, cbd->ids[i]) && !repodata_filelistfilter_matches(data, pool_id2str(pool, cbd->ids[i])))
		      printf("need complete filelist because of %s\n", pool_id2str(pool, cbd->ids[i]));
#endif
		  for (i = 0; i < cbd->nfiles; i++)
		    if (!MAPTST(&cbd->providedids, cbd->ids[i]) && !repodata_filelistfilter_matches(data, pool_id2str(pool, cbd->ids[i])))
		      break;
		  if (i < cbd->nfiles)
		    incomplete = 1;
		}
	    }

	  /* do the search */
	  map_init(&cbd->useddirs, 0);
	  for (p = data->start; p < data->end; p++)
	    if (pool->solvables[p].repo == repo)
	      {
		if (MAPTST(&donemap, p))
		  continue;
	        repodata_search(data, p, SOLVABLE_FILELIST, 0, addfileprovides_cb, cbd);
		if (!incomplete)
		  {
		    MAPSET(&donemap, p);
		    ndone++;
		  }
	      }
	  map_free(&cbd->useddirs);
	}

      if (repoonly || ++repoid == pool->nrepos)
	break;
      repo = pool->repos[repoid];
    }
  map_free(&donemap);
  queue_free(&fileprovidesq);
  map_free(&cbd->providedids);
  if (cbd->dirs)
    {
      for (i = 0; i < cbd->nfiles; i++)
	solv_free(cbd->dirs[i]);
      cbd->dirs = solv_free(cbd->dirs);
      cbd->names = solv_free(cbd->names);
    }
}

void
pool_addfileprovides_queue(Pool *pool, Queue *idq, Queue *idqinst)
{
  Solvable *s;
  Repo *installed, *repo;
  struct searchfiles sf, isf, *isfp;
  struct addfileprovides_cbdata cbd;
  int i;
  unsigned int now;

  installed = pool->installed;
  now = solv_timems(0);
  memset(&sf, 0, sizeof(sf));
  map_init(&sf.seen, pool->ss.nstrings + pool->nrels);
  memset(&isf, 0, sizeof(isf));
  map_init(&isf.seen, pool->ss.nstrings + pool->nrels);
  pool->addedfileprovides = pool->addfileprovidesfiltered ? 1 : 2;

  if (idq)
    queue_empty(idq);
  if (idqinst)
    queue_empty(idqinst);
  isfp = installed ? &isf : 0;
  for (i = 1, s = pool->solvables + i; i < pool->nsolvables; i++, s++)
    {
      repo = s->repo;
      if (!repo)
	continue;
      if (s->obsoletes)
        pool_addfileprovides_dep(pool, repo->idarraydata + s->obsoletes, &sf, isfp);
      if (s->conflicts)
        pool_addfileprovides_dep(pool, repo->idarraydata + s->conflicts, &sf, isfp);
      if (s->requires)
        pool_addfileprovides_dep(pool, repo->idarraydata + s->requires, &sf, isfp);
      if (s->recommends)
        pool_addfileprovides_dep(pool, repo->idarraydata + s->recommends, &sf, isfp);
      if (s->suggests)
        pool_addfileprovides_dep(pool, repo->idarraydata + s->suggests, &sf, isfp);
      if (s->supplements)
        pool_addfileprovides_dep(pool, repo->idarraydata + s->supplements, &sf, isfp);
      if (s->enhances)
        pool_addfileprovides_dep(pool, repo->idarraydata + s->enhances, &sf, isfp);
    }
  map_free(&sf.seen);
  map_free(&isf.seen);
  POOL_DEBUG(SOLV_DEBUG_STATS, "found %d file dependencies, %d installed file dependencies\n", sf.nfiles, isf.nfiles);
  cbd.dids = 0;
  if (sf.nfiles)
    {
#if 0
      for (i = 0; i < sf.nfiles; i++)
	POOL_DEBUG(SOLV_DEBUG_STATS, "looking up %s in filelist\n", pool_id2str(pool, sf.ids[i]));
#endif
      pool_addfileprovides_search(pool, &cbd, &sf, 0);
      if (idq)
        for (i = 0; i < sf.nfiles; i++)
	  queue_push(idq, sf.ids[i]);
      if (idqinst)
        for (i = 0; i < sf.nfiles; i++)
	  queue_push(idqinst, sf.ids[i]);
      solv_free(sf.ids);
    }
  if (isf.nfiles)
    {
#if 0
      for (i = 0; i < isf.nfiles; i++)
	POOL_DEBUG(SOLV_DEBUG_STATS, "looking up %s in installed filelist\n", pool_id2str(pool, isf.ids[i]));
#endif
      if (installed)
        pool_addfileprovides_search(pool, &cbd, &isf, installed);
      if (installed && idqinst)
        for (i = 0; i < isf.nfiles; i++)
	  queue_pushunique(idqinst, isf.ids[i]);
      solv_free(isf.ids);
    }
  solv_free(cbd.dids);
  pool_freewhatprovides(pool);	/* as we have added provides */
  POOL_DEBUG(SOLV_DEBUG_STATS, "addfileprovides took %d ms\n", solv_timems(now));
}

void
pool_addfileprovides(Pool *pool)
{
  pool_addfileprovides_queue(pool, 0, 0);
}

