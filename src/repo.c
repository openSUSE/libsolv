/*
 * Copyright (c) 2007, Novell Inc.
 *
 * This program is licensed under the BSD license, read LICENSE.BSD
 * for further information
 */

/*
 * repo.c
 *
 * Manage metadata coming from one repository
 * 
 */

#define _GNU_SOURCE
#include <string.h>
#include <fnmatch.h>

#include <stdio.h>
#include <stdlib.h>



#include "repo.h"
#include "pool.h"
#include "poolid_private.h"
#include "util.h"
#if 0
#include "attr_store_p.h"
#endif

#define IDARRAY_BLOCK     4095


/*
 * create empty repo
 * and add to pool
 */

Repo *
repo_create(Pool *pool, const char *name)
{
  Repo *repo;

  pool_freewhatprovides(pool);
  repo = (Repo *)sat_calloc(1, sizeof(*repo));
  pool->repos = (Repo **)sat_realloc2(pool->repos, pool->nrepos + 1, sizeof(Repo *));
  pool->repos[pool->nrepos++] = repo;
  repo->name = name ? strdup(name) : 0;
  repo->pool = pool;
  repo->start = pool->nsolvables;
  repo->end = pool->nsolvables;
  repo->nsolvables = 0;
  return repo;
}

static void
repo_freedata(Repo *repo)
{
  sat_free(repo->idarraydata);
  sat_free(repo->rpmdbid);
  sat_free((char *)repo->name);
  sat_free(repo);
}

/*
 * add Id to repo
 * olddeps = old array to extend
 * 
 */

Offset
repo_addid(Repo *repo, Offset olddeps, Id id)
{
  Id *idarray;
  int idarraysize;
  int i;
  
  idarray = repo->idarraydata;
  idarraysize = repo->idarraysize;

  if (!idarray)			       /* alloc idarray if not done yet */
    {
      idarraysize = 1;
      idarray = sat_extend_resize(0, 1, sizeof(Id), IDARRAY_BLOCK);
      idarray[0] = 0;
      repo->lastoff = 0;
    }

  if (!olddeps)				/* no deps yet */
    {   
      olddeps = idarraysize;
      idarray = sat_extend(idarray, idarraysize, 1, sizeof(Id), IDARRAY_BLOCK);
    }   
  else if (olddeps == repo->lastoff)	/* extend at end */
    idarraysize--;
  else					/* can't extend, copy old */
    {
      i = olddeps;
      olddeps = idarraysize;
      for (; idarray[i]; i++)
        {
	  idarray = sat_extend(idarray, idarraysize, 1, sizeof(Id), IDARRAY_BLOCK);
          idarray[idarraysize++] = idarray[i];
        }
      idarray = sat_extend(idarray, idarraysize, 1, sizeof(Id), IDARRAY_BLOCK);
    }
  
  idarray[idarraysize++] = id;		/* insert Id into array */
  idarray = sat_extend(idarray, idarraysize, 1, sizeof(Id), IDARRAY_BLOCK);
  idarray[idarraysize++] = 0;		/* ensure NULL termination */

  repo->idarraydata = idarray;
  repo->idarraysize = idarraysize;
  repo->lastoff = olddeps;

  return olddeps;
}


/*
 * add dependency (as Id) to repo, also unifies dependencies
 * olddeps = offset into idarraydata
 * marker= 0 for normal dep
 * marker > 0 add dep after marker
 * marker < 0 add dep after -marker
 * 
 */
Offset
repo_addid_dep(Repo *repo, Offset olddeps, Id id, Id marker)
{
  Id oid, *oidp, *markerp;
  int before;

  if (!olddeps)
    {
      if (marker > 0)
	olddeps = repo_addid(repo, olddeps, marker);
      return repo_addid(repo, olddeps, id);
    }

  if (!marker)
    {
      for (oidp = repo->idarraydata + olddeps; (oid = *oidp) != ID_NULL; oidp++)
	{
	  if (oid == id)
	    return olddeps;
	}
      return repo_addid(repo, olddeps, id);
    }

  before = 0;
  markerp = 0;
  if (marker < 0)
    {
      before = 1;
      marker = -marker;
    }
  for (oidp = repo->idarraydata + olddeps; (oid = *oidp) != ID_NULL; oidp++)
    {
      if (oid == marker)
	markerp = oidp;
      else if (oid == id)
	break;
    }

  if (oid)
    {
      if (markerp || before)
        return olddeps;
      /* we found it, but in the wrong half */
      markerp = oidp++;
      for (; (oid = *oidp) != ID_NULL; oidp++)
        if (oid == marker)
          break;
      if (!oid)
        {
	  /* no marker in array yet */
          oidp--;
          if (markerp < oidp)
            memmove(markerp, markerp + 1, (oidp - markerp) * sizeof(Id));
          *oidp = marker;
          return repo_addid(repo, olddeps, id);
        }
      while (oidp[1])
        oidp++;
      memmove(markerp, markerp + 1, (oidp - markerp) * sizeof(Id));
      *oidp = id;
      return olddeps;
    }
  /* id not yet in array */
  if (!before && !markerp)
    olddeps = repo_addid(repo, olddeps, marker);
  else if (before && markerp)
    {
      *markerp++ = id;
      id = *--oidp;
      if (markerp < oidp)
        memmove(markerp + 1, markerp, (oidp - markerp) * sizeof(Id));
      *markerp = marker;
    }
  return repo_addid(repo, olddeps, id);
}


/*
 * reserve Ids
 * make space for 'num' more dependencies
 */

Offset
repo_reserve_ids(Repo *repo, Offset olddeps, int num)
{
  num++;	/* room for trailing ID_NULL */

  if (!repo->idarraysize)	       /* ensure buffer space */
    {
      repo->idarraysize = 1;
      repo->idarraydata = sat_extend_resize(0, 1 + num, sizeof(Id), IDARRAY_BLOCK);
      repo->idarraydata[0] = 0;
      repo->lastoff = 1;
      return 1;
    }

  if (olddeps && olddeps != repo->lastoff)   /* if not appending */
    {
      /* can't insert into idarray, this would invalidate all 'larger' offsets
       * so create new space at end and move existing deps there.
       * Leaving 'hole' at old position.
       */
      
      Id *idstart, *idend;
      int count;

      for (idstart = idend = repo->idarraydata + olddeps; *idend++; )   /* find end */
	;
      count = idend - idstart - 1 + num;	       /* new size */

      repo->idarraydata = sat_extend(repo->idarraydata, repo->idarraysize, count, sizeof(Id), IDARRAY_BLOCK);
      /* move old deps to end */
      olddeps = repo->lastoff = repo->idarraysize;
      memcpy(repo->idarraydata + olddeps, idstart, count - num);
      repo->idarraysize = olddeps + count - num;

      return olddeps;
    }

  if (olddeps)			       /* appending */
    repo->idarraysize--;

  /* make room*/
  repo->idarraydata = sat_extend(repo->idarraydata, repo->idarraysize, num, sizeof(Id), IDARRAY_BLOCK);

  /* appending or new */
  repo->lastoff = olddeps ? olddeps : repo->idarraysize;

  return repo->lastoff;
}


/*
 * remove repo from pool, zero out solvables 
 * 
 */

void
repo_free(Repo *repo, int reuseids)
{
  Pool *pool = repo->pool;
  Solvable *s;
  int i;

  pool_freewhatprovides(pool);

  if (reuseids && repo->end == pool->nsolvables)
    {
      /* it's ok to reuse the ids. As this is the last repo, we can
         just shrink the solvable array */
      for (i = repo->end - 1, s = pool->solvables + i; i >= repo->start; i--, s--)
	if (s->repo != repo)
	  break;
      repo->end = i + 1;
      pool->nsolvables = i + 1;
    }
  /* zero out solvables belonging to this repo */
  for (i = repo->start, s = pool->solvables + i; i < repo->end; i++, s++)
    if (s->repo == repo)
      memset(s, 0, sizeof(*s));
  for (i = 0; i < pool->nrepos; i++)	/* find repo in pool */
    if (pool->repos[i] == repo)
      break;
  if (i == pool->nrepos)	       /* repo not in pool, return */
    return;
  if (i < pool->nrepos - 1)
    memmove(pool->repos + i, pool->repos + i + 1, (pool->nrepos - 1 - i) * sizeof(Repo *));
  pool->nrepos--;
  repo_freedata(repo);
}

void
repo_freeallrepos(Pool *pool, int reuseids)
{
  int i;

  pool_freewhatprovides(pool);
  for (i = 0; i < pool->nrepos; i++)
    repo_freedata(pool->repos[i]);
  pool->repos = sat_free(pool->repos);
  pool->nrepos = 0;
  /* the first two solvables don't belong to a repo */
  pool_free_solvable_block(pool, 2, pool->nsolvables - 2, reuseids);
}

Offset
repo_fix_legacy(Repo *repo, Offset provides, Offset supplements)
{
  Pool *pool = repo->pool;
  Id id, idp, idl;
  char buf[1024], *p, *dep;
  int i;

  if (provides)
    {
      for (i = provides; repo->idarraydata[i]; i++)
	{
	  id = repo->idarraydata[i];
	  if (ISRELDEP(id))
	    continue;
	  dep = (char *)id2str(pool, id);
	  if (!strncmp(dep, "locale(", 7) && strlen(dep) < sizeof(buf) - 2)
	    {
	      idp = 0;
	      strcpy(buf + 2, dep);
	      dep = buf + 2 + 7;
	      if ((p = strchr(dep, ':')) != 0 && p != dep)
		{
		  *p++ = 0;
		  idp = str2id(pool, dep, 1);
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
#if 0
		  strncpy(dep - 9, "language:", 9);
		  idl = str2id(pool, dep - 9, 1);
#else
		  idl = str2id(pool, dep, 1);
		  idl = rel2id(pool, NAMESPACE_LANGUAGE, idl, REL_NAMESPACE, 1);
#endif
		  if (id)
		    id = rel2id(pool, id, idl, REL_OR, 1);
		  else
		    id = idl;
		  dep = p;
		}
	      if (dep[0] && dep[1])
		{
	 	  for (p = dep; *p && *p != ')'; p++)
		    ;
		  *p = 0;
#if 0
		  strncpy(dep - 9, "language:", 9);
		  idl = str2id(pool, dep - 9, 1);
#else
		  idl = str2id(pool, dep, 1);
		  idl = rel2id(pool, NAMESPACE_LANGUAGE, idl, REL_NAMESPACE, 1);
#endif
		  if (id)
		    id = rel2id(pool, id, idl, REL_OR, 1);
		  else
		    id = idl;
		}
	      if (idp)
		id = rel2id(pool, idp, id, REL_AND, 1);
	      if (id)
		supplements = repo_addid_dep(repo, supplements, id, 0);
	    }
	  else if ((p = strchr(dep, ':')) != 0 && p != dep && p[1] == '/' && strlen(dep) < sizeof(buf))
	    {
	      strcpy(buf, dep);
	      p = buf + (p - dep);
	      *p++ = 0;
	      idp = str2id(pool, buf, 1);
	      id = str2id(pool, p, 1);
	      id = rel2id(pool, idp, id, REL_WITH, 1);
	      id = rel2id(pool, NAMESPACE_SPLITPROVIDES, id, REL_NAMESPACE, 1);
	      supplements = repo_addid_dep(repo, supplements, id, 0);
	    }
	}
    }
  if (!supplements)
    return 0;
  for (i = supplements; repo->idarraydata[i]; i++)
    {
      id = repo->idarraydata[i];
      if (ISRELDEP(id))
	continue;
      dep = (char *)id2str(pool, id);
      if (!strncmp(dep, "system:modalias(", 16))
	dep += 7;
      if (!strncmp(dep, "modalias(", 9) && dep[9] && dep[10] && strlen(dep) < sizeof(buf))
	{
	  strcpy(buf, dep);
	  p = strchr(buf + 9, ':');
	  if (p && p != buf + 9 && strchr(p + 1, ':'))
	    {
	      *p++ = 0;
	      idp = str2id(pool, buf + 9, 1);
	      p[strlen(p) - 1] = 0;
	      id = str2id(pool, p, 1);
	      id = rel2id(pool, NAMESPACE_MODALIAS, id, REL_NAMESPACE, 1);
	      id = rel2id(pool, idp, id, REL_AND, 1);
	    }
	  else
	    {
	      p = buf + 9;
	      p[strlen(p) - 1] = 0;
	      id = str2id(pool, p, 1);
	      id = rel2id(pool, NAMESPACE_MODALIAS, id, REL_NAMESPACE, 1);
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
	      *p++ = 0;
	      idp = str2id(pool, dep, 1);
	      if (id)
		id = rel2id(pool, id, idp, REL_AND, 1);
	      else
		id = idp;
	      dep = p;
	    }
	  if (dep[0] && dep[1])
	    {
	      dep[strlen(dep) - 1] = 0;
	      idp = str2id(pool, dep, 1);
	      if (id)
		id = rel2id(pool, id, idp, REL_AND, 1);
	      else
		id = idp;
	    }
	  if (id)
	    repo->idarraydata[i] = id;
	}
    }
  return supplements;
}

struct matchdata
{
  Pool *pool;
  const char *match;
  int flags;
#if 0
  regex_t regex;
#endif
  int stop;
  int (*callback)(void *cbdata, Solvable *s, Repodata *data, Repokey *key, KeyValue *kv);
  void *callback_data;
};

int
repo_matchvalue(void *cbdata, Solvable *s, Repodata *data, Repokey *key, KeyValue *kv)
{
  struct matchdata *md = cbdata;
  int flags = md->flags;

  if ((flags & SEARCH_STRINGMASK) != 0)
    {
      switch (key->type)
	{
	case TYPE_ID:
	case TYPE_IDARRAY:
	  if (data->localpool)
	    kv->str = stringpool_id2str(&data->spool, kv->id);
	  else
	    kv->str = id2str(data->repo->pool, kv->id);
	  break;
	case TYPE_STR:
	  break;
	default:
	  return 0;
	}
      switch ((flags & SEARCH_STRINGMASK))
	{
	  case SEARCH_SUBSTRING:
	    if (flags & SEARCH_NOCASE)
	      {
	        if (!strcasestr(kv->str, md->match))
		  return 0;
	      }
	    else
	      {
	        if (!strstr(kv->str, md->match))
		  return 0;
	      }
	    break;
	  case SEARCH_STRING:
	    if (flags & SEARCH_NOCASE)
	      {
	        if (strcasecmp(md->match, kv->str))
		  return 0;
	      }
	    else
	      {
	        if (strcmp(md->match, kv->str))
		  return 0;
	      }
	    break;
	  case SEARCH_GLOB:
	    if (fnmatch(md->match, kv->str, (flags & SEARCH_NOCASE) ? FNM_CASEFOLD : 0))
	      return 0;
	    break;
#if 0
	  case SEARCH_REGEX:
	    if (regexec(&md->regexp, kv->str, 0, NULL, 0))
	      return 0;
#endif
	  default:
	    return 0;
	}
    }
  md->stop = md->callback(md->callback_data, s, data, key, kv);
  return md->stop;
}


static Repokey solvablekeys[RPM_RPMDBID - SOLVABLE_NAME + 1] = {
  { SOLVABLE_NAME,        TYPE_ID, 0, KEY_STORAGE_SOLVABLE },
  { SOLVABLE_ARCH,        TYPE_ID, 0, KEY_STORAGE_SOLVABLE },
  { SOLVABLE_EVR,         TYPE_ID, 0, KEY_STORAGE_SOLVABLE },
  { SOLVABLE_VENDOR,      TYPE_ID, 0, KEY_STORAGE_SOLVABLE },
  { SOLVABLE_PROVIDES,    TYPE_IDARRAY, 0, KEY_STORAGE_SOLVABLE },
  { SOLVABLE_OBSOLETES,   TYPE_IDARRAY, 0, KEY_STORAGE_SOLVABLE },
  { SOLVABLE_CONFLICTS,   TYPE_IDARRAY, 0, KEY_STORAGE_SOLVABLE },
  { SOLVABLE_REQUIRES,    TYPE_IDARRAY, 0, KEY_STORAGE_SOLVABLE },
  { SOLVABLE_RECOMMENDS,  TYPE_IDARRAY, 0, KEY_STORAGE_SOLVABLE },
  { SOLVABLE_SUGGESTS,    TYPE_IDARRAY, 0, KEY_STORAGE_SOLVABLE },
  { SOLVABLE_SUPPLEMENTS, TYPE_IDARRAY, 0, KEY_STORAGE_SOLVABLE },
  { SOLVABLE_ENHANCES,    TYPE_IDARRAY, 0, KEY_STORAGE_SOLVABLE },
  { SOLVABLE_FRESHENS,    TYPE_IDARRAY, 0, KEY_STORAGE_SOLVABLE },
  { RPM_RPMDBID,          TYPE_U32, 0, KEY_STORAGE_SOLVABLE },
};

static void
domatch_idarray(Solvable *s, Id keyname, struct matchdata *md, Id *ida)
{
  KeyValue kv;
  for (; *ida && !md->stop; ida++)
    {
      kv.id = *ida;
      kv.eof = ida[1] ? 0 : 1;
      repo_matchvalue(md, s, 0, solvablekeys + (keyname - SOLVABLE_NAME), &kv);
    }
}

static void
repo_search_md(Repo *repo, Id p, Id keyname, struct matchdata *md)
{
  KeyValue kv;
  Pool *pool = repo->pool;
  Repodata *data;
  Solvable *s;
  int i, j, flags;

  md->stop = 0;
  if (!p)
    {
      for (p = repo->start, s = repo->pool->solvables + p; p < repo->end; p++, s++)
	{
	  if (s->repo == repo)
            repo_search_md(repo, p, keyname, md);
	  if (md->stop > SEARCH_NEXT_SOLVABLE)
	    break;
	}
      return;
    }
  s = pool->solvables + p;
  flags = md->flags;
  if (!(flags & SEARCH_NO_STORAGE_SOLVABLE))
    {
      switch(keyname)
	{
	  case 0:
	  case SOLVABLE_NAME:
	    if (s->name)
	      {
		kv.id = s->name;
		repo_matchvalue(md, s, 0, solvablekeys + 0, &kv);
	      }
	    if (keyname || md->stop > SEARCH_NEXT_KEY)
	      return;
	  case SOLVABLE_ARCH:
	    if (s->arch)
	      {
		kv.id = s->arch;
		repo_matchvalue(md, s, 0, solvablekeys + 1, &kv);
	      }
	    if (keyname || md->stop > SEARCH_NEXT_KEY)
	      return;
	  case SOLVABLE_EVR:
	    if (s->evr)
	      {
		kv.id = s->evr;
		repo_matchvalue(md, s, 0, solvablekeys + 2, &kv);
	      }
	    if (keyname || md->stop > SEARCH_NEXT_KEY)
	      return;
	  case SOLVABLE_VENDOR:
	    if (s->vendor)
	      {
		kv.id = s->vendor;
		repo_matchvalue(md, s, 0, solvablekeys + 3, &kv);
	      }
	    if (keyname || md->stop > SEARCH_NEXT_KEY)
	      return;
	  case SOLVABLE_PROVIDES:
	    if (s->provides)
	      domatch_idarray(s, SOLVABLE_PROVIDES, md, repo->idarraydata + s->provides);
	    if (keyname || md->stop > SEARCH_NEXT_KEY)
	      return;
	  case SOLVABLE_OBSOLETES:
	    if (s->obsoletes)
	      domatch_idarray(s, SOLVABLE_OBSOLETES, md, repo->idarraydata + s->obsoletes);
	    if (keyname || md->stop > SEARCH_NEXT_KEY)
	      return;
	  case SOLVABLE_CONFLICTS:
	    if (s->conflicts)
	      domatch_idarray(s, SOLVABLE_CONFLICTS, md, repo->idarraydata + s->conflicts);
	    if (keyname || md->stop > SEARCH_NEXT_KEY)
	      return;
	  case SOLVABLE_REQUIRES:
	    if (s->requires)
	      domatch_idarray(s, SOLVABLE_REQUIRES, md, repo->idarraydata + s->requires);
	    if (keyname || md->stop > SEARCH_NEXT_KEY)
	      return;
	  case SOLVABLE_RECOMMENDS:
	    if (s->recommends)
	      domatch_idarray(s, SOLVABLE_RECOMMENDS, md, repo->idarraydata + s->recommends);
	    if (keyname || md->stop > SEARCH_NEXT_KEY)
	      return;
	  case SOLVABLE_SUPPLEMENTS:
	    if (s->supplements)
	      domatch_idarray(s, SOLVABLE_SUPPLEMENTS, md, repo->idarraydata + s->supplements);
	    if (keyname || md->stop > SEARCH_NEXT_KEY)
	      return;
	  case SOLVABLE_SUGGESTS:
	    if (s->suggests)
	      domatch_idarray(s, SOLVABLE_SUGGESTS, md, repo->idarraydata + s->suggests);
	    if (keyname || md->stop > SEARCH_NEXT_KEY)
	      return;
	  case SOLVABLE_ENHANCES:
	    if (s->enhances)
	      domatch_idarray(s, SOLVABLE_ENHANCES, md, repo->idarraydata + s->enhances);
	    if (keyname || md->stop > SEARCH_NEXT_KEY)
	      return;
	  case SOLVABLE_FRESHENS:
	    if (s->freshens)
	      domatch_idarray(s, SOLVABLE_FRESHENS, md, repo->idarraydata + s->freshens);
	    if (keyname || md->stop > SEARCH_NEXT_KEY)
	      return;
	  case RPM_RPMDBID:
	    if (repo->rpmdbid)
	      {
		kv.num = repo->rpmdbid[p - repo->start];
		repo_matchvalue(md, s, 0, solvablekeys + (RPM_RPMDBID - SOLVABLE_NAME), &kv);
	      }
	    if (keyname || md->stop > SEARCH_NEXT_KEY)
	      return;
	    break;
	  default:
	    break;
	}
    }

  for (i = 0, data = repo->repodata; i < repo->nrepodata; i++, data++)
    {
      if (p < data->start || p >= data->end)
	continue;
      if (data->state == REPODATA_STUB)
	{
	  if (keyname)
	    {
	      for (j = 1; j < data->nkeys; j++)
		if (keyname == data->keys[j].name)
		  break;
	      if (j == data->nkeys)
		continue;
	    }
	  /* load it */
	  if (data->loadcallback)
	    data->loadcallback(data);
	  else
            data->state = REPODATA_ERROR;
	}
      if (data->state == REPODATA_ERROR)
	continue;
      repodata_search(data, p - data->start, keyname, repo_matchvalue, md);
      if (md->stop > SEARCH_NEXT_KEY)
	break;
    }
}

void
repo_search(Repo *repo, Id p, Id key, const char *match, int flags, int (*callback)(void *cbdata, Solvable *s, Repodata *data, Repokey *key, KeyValue *kv), void *cbdata)
{
  struct matchdata md;

  memset(&md, 0, sizeof(md));
  md.pool = repo->pool;
  md.match = match;
  md.flags = flags;
  md.callback = callback;
  md.callback_data = cbdata;
  repo_search_md(repo, p, key, &md);
}

const char *
repo_lookup_str(Solvable *s, Id key)
{
  Repo *repo = s->repo;
  Pool *pool = repo->pool;
  Repodata *data;
  int i, j, n;

  switch(key)
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
	  if (data->keys[j].name == key && (data->keys[j].type == TYPE_ID || data->keys[j].type == TYPE_STR))
	    return repodata_lookup_str(data, n - data->start, j);
	}
    }
  return 0;
}

int
repo_lookup_num(Solvable *s, Id key)
{
  Repo *repo = s->repo;
  Pool *pool = repo->pool;
  Repodata *data;
  int i, j, n;

  n = s - pool->solvables;
  for (i = 0, data = repo->repodata; i < repo->nrepodata; i++, data++)
    {
      if (n < data->start || n >= data->end)
	continue;
      for (j = 1; j < data->nkeys; j++)
	{
	  if (data->keys[j].name == key
	      && (data->keys[j].type == TYPE_U32
	          || data->keys[j].type == TYPE_NUM
		  || data->keys[j].type == TYPE_CONSTANT))
	    {
	      unsigned value;
	      if (repodata_lookup_num(data, n - data->start, j, &value))
	        return value;
	    }
	}
    }
  return 0;
}

Repodata *
repo_add_repodata(Repo *repo)
{
  Repodata *data;

  repo->nrepodata++;
  repo->repodata = sat_realloc2(repo->repodata, repo->nrepodata, sizeof(*data));
  data = repo->repodata + repo->nrepodata - 1;
  memset(data, 0, sizeof (*data));
  data->repo = repo;
  data->start = repo->start;
  data->end = repo->end;
  data->localpool = 0;
  data->keys = sat_calloc(1, sizeof(Repokey));
  data->nkeys = 1;
  data->schemata = sat_calloc(1, sizeof(Id));
  data->schemadata = sat_calloc(1, sizeof(Id));
  data->nschemata = 1;
  data->schemadatalen = 1;
  data->incoreoffset = sat_calloc(data->end - data->start, sizeof(Id));
  return data;
}

static Repodata *findrepodata(Repo *repo, Id p, Id keyname)
{
  int i;
  Repodata *data;

  /* FIXME: enter nice code here */
  for (i = 0, data = repo->repodata; i < repo->nrepodata; i++, data++)
    if (p >= data->start && p < data->end)
      return data;
  for (i = 0, data = repo->repodata; i < repo->nrepodata; i++, data++)
    if (p == data->end)
      break;
  if (i < repo->nrepodata)
    {
      repodata_extend(data, p);
      return data;
    }
  return repo_add_repodata(repo);
}

void
repo_set_id(Repo *repo, Id p, Id keyname, Id id)
{
  Repodata *data = findrepodata(repo, p, keyname);
  repodata_set_id(data, p - data->start, keyname, id);
}

void
repo_set_num(Repo *repo, Id p, Id keyname, Id num)
{
  Repodata *data = findrepodata(repo, p, keyname);
  repodata_set_num(data, p - data->start, keyname, num);
}

void
repo_set_str(Repo *repo, Id p, Id keyname, const char *str)
{
  Repodata *data = findrepodata(repo, p, keyname);
  repodata_set_str(data, p - data->start, keyname, str);
}

void
repo_set_poolstr(Repo *repo, Id p, Id keyname, const char *str)
{
  Repodata *data = findrepodata(repo, p, keyname);
  repodata_set_poolstr(data, p - data->start, keyname, str);
}

void
repo_internalize(Repo *repo)
{
  int i;
  Repodata *data;

  for (i = 0, data = repo->repodata; i < repo->nrepodata; i++, data++)
    if (data->attrs)
      repodata_internalize(data);
}


#if 0

static int
key_cmp (const void *pa, const void *pb)
{
  Repokey *a = (Repokey *)pa;
  Repokey *b = (Repokey *)pb;
  return a->name - b->name;
}

void
repo_add_attrstore (Repo *repo, Attrstore *s, const char *location)
{
  unsigned i;
  Repodata *data;
  /* If this is meant to be the embedded attributes, make sure we don't
     have them already.  */
  if (!location)
    {
      for (i = 0; i < repo->nrepodata; i++)
        if (repo->repodata[i].location == 0)
	  break;
      if (i != repo->nrepodata)
        {
	  pool_debug (repo->pool, SAT_FATAL, "embedded attribs added twice\n");
	  exit (1);
	}
    }
  repo->nrepodata++;
  repo->repodata = sat_realloc2(repo->repodata, repo->nrepodata, sizeof(*data));
  data = repo->repodata + repo->nrepodata - 1;
  memset (data, 0, sizeof (*data));
  data->s = s;
  data->nkeys = s->nkeys;
  if (data->nkeys)
    {
      data->keys = sat_malloc2(data->nkeys, sizeof(data->keys[0]));
      for (i = 1; i < data->nkeys; i++)
        {
          data->keys[i].name = s->keys[i].name;
	  data->keys[i].type = s->keys[i].type;
	}
      if (data->nkeys > 2)
        qsort(data->keys + 1, data->nkeys - 1, sizeof(data->keys[0]), key_cmp);
    }
  if (location)
    data->location = strdup(location);
}
#endif

// EOF
