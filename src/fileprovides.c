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

  Map *providedids;
  int provstart;
  int provend;

  Map *todo;
  int todo_start;
  int todo_end;
};

/* split filelist dep into basename and dirname */
static void
create_dirs_names_array(struct addfileprovides_cbdata *cbd, Pool *pool)
{
  int i;
  cbd->dirs = solv_malloc2(cbd->nfiles, sizeof(char *));
  cbd->names = solv_malloc2(cbd->nfiles, sizeof(char *));
  for (i = 0; i < cbd->nfiles; i++)
    {
      char *s = solv_strdup(pool_id2str(pool, cbd->ids[i]));
      cbd->dirs[i] = s;
      s = strrchr(s, '/');
      *s = 0;
      cbd->names[i] = s + 1;
    }
}

static void
free_dirs_names_array(struct addfileprovides_cbdata *cbd)
{
  int i;
  if (cbd->dirs)
    {
      for (i = 0; i < cbd->nfiles; i++)
	solv_free(cbd->dirs[i]);
      cbd->dirs = solv_free(cbd->dirs);
      cbd->names = solv_free(cbd->names);
    }
}

static void
prune_todo_range(Repo *repo, struct addfileprovides_cbdata *cbd)
{
  int start = cbd->todo_start, end = cbd->todo_end;
  while (start < end && !MAPTST(cbd->todo, start - repo->start))
    start++;
  while (end > start && !MAPTST(cbd->todo, end - 1 - repo->start))
    end--;
  cbd->todo_start = start;
  cbd->todo_end = end;
}

static int
repodata_intersects_todo(Repodata *data, struct addfileprovides_cbdata *cbd)
{
  Repo *repo;
  int p, start = data->start, end = data->end;
  if (start >= cbd->todo_end || end <= cbd->todo_start)
    return 0;
  repo = data->repo;
  if (start < cbd->todo_start)
    start = cbd->todo_start;
  if (end > cbd->todo_end)
    end = cbd->todo_end;
  for (p = start; p < end; p++)
    if (MAPTST(cbd->todo, p - repo->start))
      return 1;
  return 0;
}

/* forward declaration */
static void repodata_addfileprovides_search(Repodata *data, struct addfileprovides_cbdata *cbd);

/* search a subset of the todo range */
static void
repodata_addfileprovides_search_limited(Repodata *data, struct addfileprovides_cbdata *cbd, int start, int end)
{

  int old_todo_start = cbd->todo_start;
  int old_todo_end = cbd->todo_end;
  if (start < cbd->todo_start)
    start = cbd->todo_start;
  if (end > cbd->todo_end)
    end = cbd->todo_end;
  if (start >= end)
    return;
  cbd->todo_start = start;
  cbd->todo_end = end;
  repodata_addfileprovides_search(data, cbd);
  cbd->todo_start = old_todo_start;
  cbd->todo_end = old_todo_end;
  prune_todo_range(data->repo, cbd);
}

static void
repodata_addfileprovides_search(Repodata *data, struct addfileprovides_cbdata *cbd)
{
  Repo *repo = data->repo;
  int i, p, start, end;
  Map useddirs;
  Map *providedids = 0;

  /* make it available */
  if (data->state == REPODATA_STUB)
    repodata_load(data);
  if (data->state != REPODATA_AVAILABLE)
    return;
  if (!data->incoredata || !data->dirpool.ndirs)
    return;

  start = cbd->todo_start > data->start ? cbd->todo_start : data->start;
  end = cbd->todo_end > data->end ? data->end : cbd->todo_end;

  if (start >= end)
    return;

  /* deal with provideids overlap */
  if (cbd->providedids)
    {
      if (start >= cbd->provstart && end <= cbd->provend)
	providedids = cbd->providedids;	/* complete overlap */
      else if (start < cbd->provend && end > cbd->provstart)
	{
	  /* partial overlap, need to split search */
	  if (start < cbd->provstart)
	    {
	      repodata_addfileprovides_search_limited(data, cbd, start, cbd->provstart);
	      start = cbd->provstart;
	    }
	  if (end > cbd->provend)
	    {
	      repodata_addfileprovides_search_limited(data, cbd, cbd->provend, end);
	      end = cbd->provend;
	    }
	  if (start < end)
	    repodata_addfileprovides_search_limited(data, cbd, start, end);
	  return;
	}
    }

  /* set up dirs and names array if not already done */
  if (!cbd->dirs)
    create_dirs_names_array(cbd, repo->pool);

  /* set up useddirs map and the cbd->dids array */
  map_init(&useddirs, data->dirpool.ndirs);
  for (i = 0; i < cbd->nfiles; i++)
    {
      Id did;
      if (providedids && MAPTST(providedids, cbd->ids[i]))
	{
	  cbd->dids[i] = 0;	/* already included, do not add again */
	  continue;
	}
      cbd->dids[i] = did = repodata_str2dir(data, cbd->dirs[i], 0);
      if (did)
	MAPSET(&useddirs, did);
    }
  repodata_free_dircache(data);		/* repodata_str2dir created it */

  for (p = start; p < end; p++)
    {
      const unsigned char *dp;
      Solvable *s;
      if (!MAPTST(cbd->todo, p - repo->start))
	continue;
      dp = repodata_lookup_packed_dirstrarray(data, p, SOLVABLE_FILELIST);
      if (!dp)
	continue;
      /* now iterate through the packed array */
      s = repo->pool->solvables + p;
      MAPCLR(cbd->todo, p - repo->start);	/* this entry is done */
      for (;;)
	{
	  Id did = 0;
	  int c;
	  while ((c = *dp++) & 0x80)
	    did = (did << 7) ^ c ^ 0x80;
	  did = (did << 6) | (c & 0x3f);
	  if ((unsigned int)did < (unsigned int)data->dirpool.ndirs && MAPTST(&useddirs, did))
	    {
	      /* there is at least one entry with that did */
	      for (i = 0; i < cbd->nfiles; i++)
		if (cbd->dids[i] == did && !strcmp(cbd->names[i], (const char *)dp))
		  s->provides = repo_addid_dep(s->repo, s->provides, cbd->ids[i], SOLVABLE_FILEMARKER);
	    }
	  if (!(c & 0x40))
	    break;
	  dp += strlen((const char *)dp) + 1;
	}
    }
  map_free(&useddirs);
  prune_todo_range(repo, cbd);
}

static void
repo_addfileprovides_search_filtered(Repo *repo, struct addfileprovides_cbdata *cbd, int filteredid, Map *postpone)
{
  Repodata *data = repo->repodata + filteredid;
  Map *providedids = cbd->providedids;
  int rdid;
  int start, end, p, i;
  Map old_todo;
  int old_todo_start, old_todo_end;

  start = cbd->todo_start > data->start ? cbd->todo_start : data->start;
  end = cbd->todo_end > data->end ? data->end : cbd->todo_end;

  if (providedids)
    {
      /* check if all solvables are in the provide range */
      if (start < cbd->provstart || end > cbd->provend)
	{
	  /* unclear, check each solvable */
	  for (p = start; p < end; p++)
	    {
	      if (p >= cbd->provstart && p < cbd->provend)
		continue;
	      if (data->incoreoffset[p - data->start] && MAPTST(cbd->todo, p - repo->start))
		{
		  providedids = 0;	/* nope, cannot prune with providedids */
		  break;
		}
	    }
	}
    }

  /* check if the filtered files are enough */
  for (i = 0; i < cbd->nfiles; i++)
    {
      if (providedids && MAPTST(providedids, cbd->ids[i]))	/* this one is already provided */
	continue;
      if (!repodata_filelistfilter_matches(data, pool_id2str(repo->pool, cbd->ids[i])))
        break;
    }
  if (i < cbd->nfiles)
    {
      /* nope, need to search the extensions as well. postpone. */
      for (p = start; p < end; p++)
	{
	  if (data->incoreoffset[p - data->start] && MAPTST(cbd->todo, p - repo->start))
	    {
	      if (!postpone->size)
		map_grow(postpone, repo->nsolvables);
	      MAPSET(postpone, p - repo->start);
	      MAPCLR(cbd->todo, p - repo->start);
	    }
	}
      prune_todo_range(repo, cbd);
      return;
    }

  /* now check if there is no data marked withour EXTENSION */
  /* limit todo to the solvables in this repodata */
  old_todo_start = cbd->todo_start;
  old_todo_end = cbd->todo_end;
  old_todo = *cbd->todo;
  map_init(cbd->todo, repo->nsolvables);
  for (p = start; p < end; p++)
    if (data->incoreoffset[p - data->start] && MAPTST(&old_todo, p - repo->start))
      {
        MAPCLR(&old_todo, p - repo->start);
        MAPSET(cbd->todo, p - repo->start);
      }
  prune_todo_range(repo, cbd);

  /* do the check */
  for (rdid = repo->nrepodata - 1, data = repo->repodata + rdid; rdid > filteredid ; rdid--, data--)
    {
      if (data->filelisttype == REPODATA_FILELIST_EXTENSION)
	continue;
      if (data->start >= cbd->todo_end || data->end <= cbd->todo_start)
	continue;
      if (!repodata_has_keyname(data, SOLVABLE_FILELIST))
	continue;
      if (!repodata_intersects_todo(data, cbd))
	continue;
      /* oh no, this filelist data is not tagged with REPODATA_FILELIST_EXTENSION! */
      /* postpone entries that have filelist data */
      start = cbd->todo_start > data->start ? cbd->todo_start : data->start;
      end = cbd->todo_end > data->end ? data->end : cbd->todo_end;
      for (p = start; p < end; p++)
	if (MAPTST(cbd->todo, p - repo->start))
	  if (repodata_lookup_type(data, p, SOLVABLE_FILELIST))
	    {
	      if (!postpone->size)
		map_grow(postpone, repo->nsolvables);
	      MAPSET(postpone, p - repo->start);
	      MAPCLR(cbd->todo, p - repo->start);
	    }
      prune_todo_range(repo, cbd);
      if (cbd->todo_start >= cbd->todo_end)
	break;
    }

  /* do the search over the filtered file list with the remaining entries*/
  if (cbd->todo_start < cbd->todo_end)
    repodata_addfileprovides_search(repo->repodata + filteredid, cbd);

  /* restore todo map */
  map_free(cbd->todo);
  *cbd->todo = old_todo;
  cbd->todo_start = old_todo_start;
  cbd->todo_end = old_todo_end;
  prune_todo_range(repo, cbd);
}

static void
repo_addfileprovides_search(Repo *repo, struct addfileprovides_cbdata *cbd, struct searchfiles *sf)
{
  Repodata *data;
  int rdid, p, i;
  int provstart, provend;
  Map todo;
  Map providedids;

  if (repo->end <= repo->start || !repo->nsolvables || !sf->nfiles)
    return;

  /* update search data if changed */
  if (cbd->nfiles != sf->nfiles || cbd->ids != sf->ids)
    {
      free_dirs_names_array(cbd);
      cbd->nfiles = sf->nfiles;
      cbd->ids = sf->ids;
      cbd->dids = solv_realloc2(cbd->dids, sf->nfiles, sizeof(Id));
    }

  /* create todo map and range */
  map_init(&todo, repo->end - repo->start);
  for (p = repo->start; p < repo->end; p++)
    if (repo->pool->solvables[p].repo == repo)
      MAPSET(&todo, p - repo->start);
  cbd->todo = &todo;
  cbd->todo_start = repo->start;
  cbd->todo_end = repo->end;
  prune_todo_range(repo, cbd);

  provstart = provend = 0;
  map_init(&providedids, 0);
  data = repo_lookup_repodata(repo, SOLVID_META, REPOSITORY_ADDEDFILEPROVIDES);
  if (data)
    {
      Queue fileprovidesq;
      queue_init(&fileprovidesq);
      if (repodata_lookup_idarray(data, SOLVID_META, REPOSITORY_ADDEDFILEPROVIDES, &fileprovidesq))
	{
	  map_grow(&providedids, repo->pool->ss.nstrings);
	  cbd->providedids = &providedids;
	  provstart = data->start;
	  provend = data->end;
	  for (i = 0; i < fileprovidesq.count; i++)
	    MAPSET(&providedids, fileprovidesq.elements[i]);
	  for (i = 0; i < cbd->nfiles; i++)
	    if (!MAPTST(&providedids, cbd->ids[i]))
	      break;
	  if (i == cbd->nfiles)
	    {
	      /* all included, clear entries from todo list */
	      if (provstart <= cbd->todo_start && provend >= cbd->todo_end)
		cbd->todo_end = cbd->todo_start;	/* clear complete range */
	      else
		{
		  for (p = provstart; p < provend; p++)
		    MAPCLR(&todo, p - repo->start);
		  prune_todo_range(repo, cbd);
		}
	    }
	}
      queue_free(&fileprovidesq);
    }

  if (cbd->todo_start >= cbd->todo_end)
    {
      map_free(&todo);
      cbd->todo = 0;
      map_free(&providedids);
      cbd->providedids = 0;
      return;
    }

  /* this is similar to repo_lookup_filelist_repodata in repo.c */

  for (rdid = 1, data = repo->repodata + rdid; rdid < repo->nrepodata; rdid++, data++)
    if (data->filelisttype == REPODATA_FILELIST_FILTERED)
      break;
  for (; rdid < repo->nrepodata; rdid++, data++)
    if (data->filelisttype == REPODATA_FILELIST_EXTENSION)
      break;

  if (rdid < repo->nrepodata)
    {
      /* have at least one repodata with REPODATA_FILELIST_FILTERED followed by REPODATA_FILELIST_EXTENSION */
      Map postpone;
      map_init(&postpone, 0);
      for (rdid = repo->nrepodata - 1, data = repo->repodata + rdid; rdid > 0; rdid--, data--)
	{
	  if (data->filelisttype != REPODATA_FILELIST_FILTERED)
	    continue;
	  if (!repodata_intersects_todo(data, cbd))
	    continue;
	  if (data->state != REPODATA_AVAILABLE)
	    {
	      if (data->state != REPODATA_STUB)
		continue;
	      repodata_load(data);
	      if (data->state != REPODATA_AVAILABLE || data->filelisttype != REPODATA_FILELIST_FILTERED)
		continue;
	    }
	  repo_addfileprovides_search_filtered(repo, cbd, rdid, &postpone);
	}
      if (postpone.size)
	{
	  /* add postponed entries back to todo */
	  map_or(&todo, &postpone);
	  cbd->todo_start = repo->start;
	  cbd->todo_end = repo->end;
	  prune_todo_range(repo, cbd);
	}
      map_free(&postpone);
    }

  /* search remaining entries in the standard way */
  if (cbd->todo_start < cbd->todo_end)
    {
      for (rdid = repo->nrepodata - 1, data = repo->repodata + rdid; rdid > 0; rdid--, data--)
	{
	  if (data->start >= cbd->todo_end || data->end <= cbd->todo_start)
	    continue;
	  if (!repodata_has_keyname(data, SOLVABLE_FILELIST))
	    continue;
	  if (!repodata_intersects_todo(data, cbd))
	    continue;
	  repodata_addfileprovides_search(data, cbd);
	  if (cbd->todo_start >= cbd->todo_end)
	    break;
	}
    }

  map_free(&todo);
  cbd->todo = 0;
  map_free(&providedids);
  cbd->providedids = 0;
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
  memset(&cbd, 0, sizeof(cbd));
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
  if (sf.nfiles)
    {
#if 0
      for (i = 0; i < sf.nfiles; i++)
	POOL_DEBUG(SOLV_DEBUG_STATS, "looking up %s in filelist\n", pool_id2str(pool, sf.ids[i]));
#endif
      FOR_REPOS(i, repo)
        repo_addfileprovides_search(repo, &cbd, &sf);
      if (idq)
	queue_insertn(idq, idq->count, sf.nfiles, sf.ids);
      if (idqinst)
	queue_insertn(idqinst, idqinst->count, sf.nfiles, sf.ids);
      solv_free(sf.ids);
    }
  if (isf.nfiles)
    {
#if 0
      for (i = 0; i < isf.nfiles; i++)
	POOL_DEBUG(SOLV_DEBUG_STATS, "looking up %s in installed filelist\n", pool_id2str(pool, isf.ids[i]));
#endif
      if (installed)
        repo_addfileprovides_search(installed, &cbd, &isf);
      if (installed && idqinst)
        for (i = 0; i < isf.nfiles; i++)
	  queue_pushunique(idqinst, isf.ids[i]);
      solv_free(isf.ids);
    }
  free_dirs_names_array(&cbd);
  solv_free(cbd.dids);
  pool_freewhatprovides(pool);	/* as we have added provides */
  POOL_DEBUG(SOLV_DEBUG_STATS, "addfileprovides took %d ms\n", solv_timems(now));
}

void
pool_addfileprovides(Pool *pool)
{
  pool_addfileprovides_queue(pool, 0, 0);
}

