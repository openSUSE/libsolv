#include <stdio.h>
#include <stdlib.h>
#include <unistd.h>

#include "pool.h"
#include "repo.h"

#include "repoinfo.h"
#include "repoinfo_cache.h"

#include "fileprovides.h"

static void
rewrite_repos(Pool *pool, Queue *addedfileprovides, Queue *addedfileprovides_inst)
{
  Repo *repo;
  Repodata *data;
  Map providedids;
  Queue fileprovidesq;
  int i, j, n;
  struct repoinfo *cinfo;

  map_init(&providedids, pool->ss.nstrings);
  queue_init(&fileprovidesq);
  for (i = 0; i < addedfileprovides->count; i++)
    MAPSET(&providedids, addedfileprovides->elements[i]);
  FOR_REPOS(i, repo)
    {
      /* make sure all repodatas but the first are extensions */
      if (repo->nrepodata < 2)
	continue;
      cinfo = repo->appdata;
      if (!cinfo)
	continue;	/* cmdline */
      if (cinfo->incomplete)
	continue;
      data = repo_id2repodata(repo, 1);
      if (data->loadcallback)
        continue;
      for (j = 2; j < repo->nrepodata; j++)
	{
	  Repodata *edata = repo_id2repodata(repo, j);
	  if (!edata->loadcallback)
	    break;
	}
      if (j < repo->nrepodata)
	continue;	/* found a non-extension repodata, can't rewrite  */
      if (repodata_lookup_idarray(data, SOLVID_META, REPOSITORY_ADDEDFILEPROVIDES, &fileprovidesq))
	{
	  if (repo == pool->installed && addedfileprovides_inst)
	    {
	      for (j = 0; j < addedfileprovides->count; j++)
		MAPCLR(&providedids, addedfileprovides->elements[j]);
	      for (j = 0; j < addedfileprovides_inst->count; j++)
		MAPSET(&providedids, addedfileprovides_inst->elements[j]);
	    }
	  n = 0;
	  for (j = 0; j < fileprovidesq.count; j++)
	    if (MAPTST(&providedids, fileprovidesq.elements[j]))
	      n++;
	  if (repo == pool->installed && addedfileprovides_inst)
	    {
	      for (j = 0; j < addedfileprovides_inst->count; j++)
		MAPCLR(&providedids, addedfileprovides_inst->elements[j]);
	      for (j = 0; j < addedfileprovides->count; j++)
		MAPSET(&providedids, addedfileprovides->elements[j]);
	      if (n == addedfileprovides_inst->count)
		continue;	/* nothing new added */
	    }
	  else if (n == addedfileprovides->count)
	    continue;	/* nothing new added */
	}
      repodata_set_idarray(data, SOLVID_META, REPOSITORY_ADDEDFILEPROVIDES, repo == pool->installed && addedfileprovides_inst ? addedfileprovides_inst : addedfileprovides);
      repodata_internalize(data);
      writecachedrepo(cinfo, 0, data);
    }
  queue_free(&fileprovidesq);
  map_free(&providedids);
}

void
addfileprovides(Pool *pool)
{
  Queue addedfileprovides;
  Queue addedfileprovides_inst;

  queue_init(&addedfileprovides);
  queue_init(&addedfileprovides_inst);
  pool_addfileprovides_queue(pool, &addedfileprovides, &addedfileprovides_inst);
  if (addedfileprovides.count || addedfileprovides_inst.count)
    rewrite_repos(pool, &addedfileprovides, &addedfileprovides_inst);
  queue_free(&addedfileprovides);
  queue_free(&addedfileprovides_inst);
}
