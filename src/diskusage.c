/*
 * Copyright (c) 2007-2016, SUSE LLC
 *
 * This program is licensed under the BSD license, read LICENSE.BSD
 * for further information
 */

/*
 * diskusage.c
 *
 * calculate needed space on partitions
 */

#include <stdio.h>
#include <stdlib.h>
#include <stdarg.h>
#include <unistd.h>
#include <string.h>

#include "pool.h"
#include "poolarch.h"
#include "repo.h"
#include "util.h"
#include "bitmap.h"


struct mptree {
  Id sibling;
  Id child;
  const char *comp;
  int compl;
  Id mountpoint;
};

struct ducbdata {
  DUChanges *mps;
  struct mptree *mptree;
  int addsub;
  int hasdu;

  Id *dirmap;
  int nmap;
  Repodata *olddata;
};


static int
solver_fill_DU_cb(void *cbdata, Solvable *s, Repodata *data, Repokey *key, KeyValue *value)
{
  struct ducbdata *cbd = cbdata;
  Id mp;

  if (data != cbd->olddata)
    {
      Id dn, mp, comp, *dirmap, *dirs;
      int i, compl;
      const char *compstr;
      struct mptree *mptree;

      /* create map from dir to mptree */
      cbd->dirmap = solv_free(cbd->dirmap);
      cbd->nmap = 0;
      dirmap = solv_calloc(data->dirpool.ndirs, sizeof(Id));
      mptree = cbd->mptree;
      mp = 0;
      for (dn = 2, dirs = data->dirpool.dirs + dn; dn < data->dirpool.ndirs; dn++)
	{
	  comp = *dirs++;
	  if (comp <= 0)
	    {
	      mp = dirmap[-comp];
	      continue;
	    }
	  if (mp < 0)
	    {
	      /* unconnected */
	      dirmap[dn] = mp;
	      continue;
	    }
	  if (!mptree[mp].child)
	    {
	      dirmap[dn] = -mp;
	      continue;
	    }
	  if (data->localpool)
	    compstr = stringpool_id2str(&data->spool, comp);
	  else
	    compstr = pool_id2str(data->repo->pool, comp);
	  compl = strlen(compstr);
	  for (i = mptree[mp].child; i; i = mptree[i].sibling)
	    if (mptree[i].compl == compl && !strncmp(mptree[i].comp, compstr, compl))
	      break;
	  dirmap[dn] = i ? i : -mp;
	}
      /* change dirmap to point to mountpoint instead of mptree */
      for (dn = 0; dn < data->dirpool.ndirs; dn++)
	{
	  mp = dirmap[dn];
	  dirmap[dn] = mptree[mp > 0 ? mp : -mp].mountpoint;
	}
      cbd->dirmap = dirmap;
      cbd->nmap = data->dirpool.ndirs;
      cbd->olddata = data;
    }
  cbd->hasdu = 1;
  if (value->id < 0 || value->id >= cbd->nmap)
    return 0;
  mp = cbd->dirmap[value->id];
  if (mp < 0)
    return 0;
  if (cbd->addsub > 0)
    {
      cbd->mps[mp].kbytes += value->num;
      cbd->mps[mp].files += value->num2;
    }
  else if (!(cbd->mps[mp].flags & DUCHANGES_ONLYADD))
    {
      cbd->mps[mp].kbytes -= value->num;
      cbd->mps[mp].files -= value->num2;
    }
  return 0;
}

static void
propagate_mountpoints(struct mptree *mptree, int pos, Id mountpoint)
{
  int i;
  if (mptree[pos].mountpoint == -1)
    mptree[pos].mountpoint = mountpoint;
  else
    mountpoint = mptree[pos].mountpoint;
  for (i = mptree[pos].child; i; i = mptree[i].sibling)
    propagate_mountpoints(mptree, i, mountpoint);
}

#define MPTREE_BLOCK 15

static struct mptree *
create_mptree(DUChanges *mps, int nmps)
{
  int i, nmptree;
  struct mptree *mptree;
  int pos, compl;
  int mp;
  const char *p, *path, *compstr;

  mptree = solv_extend_resize(0, 1, sizeof(struct mptree), MPTREE_BLOCK);

  /* our root node */
  mptree[0].sibling = 0;
  mptree[0].child = 0;
  mptree[0].comp = 0;
  mptree[0].compl = 0;
  mptree[0].mountpoint = -1;
  nmptree = 1;

  /* create component tree */
  for (mp = 0; mp < nmps; mp++)
    {
      mps[mp].kbytes = 0;
      mps[mp].files = 0;
      pos = 0;
      path = mps[mp].path;
      while(*path == '/')
	path++;
      while (*path)
	{
	  if ((p = strchr(path, '/')) == 0)
	    {
	      compstr = path;
	      compl = strlen(compstr);
	      path += compl;
	    }
	  else
	    {
	      compstr = path;
	      compl = p - path;
	      path = p + 1;
	      while(*path == '/')
		path++;
	    }
          for (i = mptree[pos].child; i; i = mptree[i].sibling)
	    if (mptree[i].compl == compl && !strncmp(mptree[i].comp, compstr, compl))
	      break;
	  if (!i)
	    {
	      /* create new node */
	      mptree = solv_extend(mptree, nmptree, 1, sizeof(struct mptree), MPTREE_BLOCK);
	      i = nmptree++;
	      mptree[i].sibling = mptree[pos].child;
	      mptree[i].child = 0;
	      mptree[i].comp = compstr;
	      mptree[i].compl = compl;
	      mptree[i].mountpoint = -1;
	      mptree[pos].child = i;
	    }
	  pos = i;
	}
      mptree[pos].mountpoint = mp;
    }

  propagate_mountpoints(mptree, 0, mptree[0].mountpoint);

#if 0
  for (i = 0; i < nmptree; i++)
    {
      printf("#%d sibling: %d\n", i, mptree[i].sibling);
      printf("#%d child: %d\n", i, mptree[i].child);
      printf("#%d comp: %s\n", i, mptree[i].comp);
      printf("#%d compl: %d\n", i, mptree[i].compl);
      printf("#%d mountpont: %d\n", i, mptree[i].mountpoint);
    }
#endif

  return mptree;
}

void
pool_calc_duchanges(Pool *pool, Map *installedmap, DUChanges *mps, int nmps)
{
  struct mptree *mptree;
  struct ducbdata cbd;
  Solvable *s;
  int i, sp;
  Map ignoredu;
  Repo *oldinstalled = pool->installed;
  int haveonlyadd = 0;

  map_init(&ignoredu, 0);
  mptree = create_mptree(mps, nmps);

  for (i = 0; i < nmps; i++)
    if ((mps[i].flags & DUCHANGES_ONLYADD) != 0)
      haveonlyadd = 1;
  cbd.mps = mps;
  cbd.dirmap = 0;
  cbd.nmap = 0;
  cbd.olddata = 0;
  cbd.mptree = mptree;
  cbd.addsub = 1;
  for (sp = 1, s = pool->solvables + sp; sp < pool->nsolvables; sp++, s++)
    {
      if (!s->repo || (oldinstalled && s->repo == oldinstalled))
	continue;
      if (!MAPTST(installedmap, sp))
	continue;
      cbd.hasdu = 0;
      repo_search(s->repo, sp, SOLVABLE_DISKUSAGE, 0, 0, solver_fill_DU_cb, &cbd);
      if (!cbd.hasdu && oldinstalled)
	{
	  Id op, opp;
	  int didonlyadd = 0;
	  /* no du data available, ignore data of all installed solvables we obsolete */
	  if (!ignoredu.size)
	    map_grow(&ignoredu, oldinstalled->end - oldinstalled->start);
	  FOR_PROVIDES(op, opp, s->name)
	    {
	      Solvable *s2 = pool->solvables + op;
	      if (!pool->implicitobsoleteusesprovides && s->name != s2->name)
		continue;
	      if (pool->implicitobsoleteusescolors && !pool_colormatch(pool, s, s2))
		continue;
	      if (op >= oldinstalled->start && op < oldinstalled->end)
		{
		  MAPSET(&ignoredu, op - oldinstalled->start);
		  if (haveonlyadd && pool->solvables[op].repo == oldinstalled && !didonlyadd)
		    {
		      repo_search(oldinstalled, op, SOLVABLE_DISKUSAGE, 0, 0, solver_fill_DU_cb, &cbd);
		      cbd.addsub = -1;
		      repo_search(oldinstalled, op, SOLVABLE_DISKUSAGE, 0, 0, solver_fill_DU_cb, &cbd);
		      cbd.addsub = 1;
		      didonlyadd = 1;
		    }
		}
	    }
	  if (s->obsoletes)
	    {
	      Id obs, *obsp = s->repo->idarraydata + s->obsoletes;
	      while ((obs = *obsp++) != 0)
		FOR_PROVIDES(op, opp, obs)
		  {
		    Solvable *s2 = pool->solvables + op;
		    if (!pool->obsoleteusesprovides && !pool_match_nevr(pool, s2, obs))
		      continue;
		    if (pool->obsoleteusescolors && !pool_colormatch(pool, s, s2))
		      continue;
		    if (op >= oldinstalled->start && op < oldinstalled->end)
		      {
			MAPSET(&ignoredu, op - oldinstalled->start);
			if (haveonlyadd && pool->solvables[op].repo == oldinstalled && !didonlyadd)
			  {
			    repo_search(oldinstalled, op, SOLVABLE_DISKUSAGE, 0, 0, solver_fill_DU_cb, &cbd);
			    cbd.addsub = -1;
			    repo_search(oldinstalled, op, SOLVABLE_DISKUSAGE, 0, 0, solver_fill_DU_cb, &cbd);
			    cbd.addsub = 1;
			    didonlyadd = 1;
			  }
		      }
		  }
	    }
	}
    }
  cbd.addsub = -1;
  if (oldinstalled)
    {
      /* assumes we allways have du data for installed solvables */
      FOR_REPO_SOLVABLES(oldinstalled, sp, s)
	{
	  if (MAPTST(installedmap, sp))
	    continue;
	  if (ignoredu.map && MAPTST(&ignoredu, sp - oldinstalled->start))
	    continue;
	  repo_search(oldinstalled, sp, SOLVABLE_DISKUSAGE, 0, 0, solver_fill_DU_cb, &cbd);
	}
    }
  map_free(&ignoredu);
  solv_free(cbd.dirmap);
  solv_free(mptree);
}

int
pool_calc_installsizechange(Pool *pool, Map *installedmap)
{
  Id sp;
  Solvable *s;
  int change = 0; 
  Repo *oldinstalled = pool->installed;

  for (sp = 1, s = pool->solvables + sp; sp < pool->nsolvables; sp++, s++) 
    {    
      if (!s->repo || (oldinstalled && s->repo == oldinstalled))
        continue;
      if (!MAPTST(installedmap, sp)) 
        continue;
      change += solvable_lookup_sizek(s, SOLVABLE_INSTALLSIZE, 0);
    }    
  if (oldinstalled)
    {    
      FOR_REPO_SOLVABLES(oldinstalled, sp, s)
        {
          if (MAPTST(installedmap, sp)) 
            continue;
          change -= solvable_lookup_sizek(s, SOLVABLE_INSTALLSIZE, 0);
        }
    }    
  return change;
}

