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

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "repo.h"
#include "pool.h"
#include "poolid_private.h"
#include "util.h"
#include "attr_store_p.h"

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
      idarray = sat_malloc2(1 + IDARRAY_BLOCK, sizeof(Id));
      idarray[0] = 0;
      idarraysize = 1;
      repo->lastoff = 0;
    }

  if (!olddeps)				/* no deps yet */
    {   
      olddeps = idarraysize;
      if ((idarraysize & IDARRAY_BLOCK) == 0)
        idarray = sat_realloc2(idarray, idarraysize + 1 + IDARRAY_BLOCK, sizeof(Id));
    }   
  else if (olddeps == repo->lastoff)	/* extend at end */
    idarraysize--;
  else					/* can't extend, copy old */
    {
      i = olddeps;
      olddeps = idarraysize;
      for (; idarray[i]; i++)
        {
          if ((idarraysize & IDARRAY_BLOCK) == 0)
            idarray = sat_realloc2(idarray, idarraysize + 1 + IDARRAY_BLOCK, sizeof(Id));
          idarray[idarraysize++] = idarray[i];
        }
      if ((idarraysize & IDARRAY_BLOCK) == 0)
        idarray = sat_realloc2(idarray, idarraysize + 1 + IDARRAY_BLOCK, sizeof(Id));
    }
  
  idarray[idarraysize++] = id;		/* insert Id into array */

  if ((idarraysize & IDARRAY_BLOCK) == 0)   /* realloc if at block boundary */
    idarray = sat_realloc2(idarray, idarraysize + 1 + IDARRAY_BLOCK, sizeof(Id));

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
      repo->idarraydata = sat_malloc2((1 + num + IDARRAY_BLOCK) & ~IDARRAY_BLOCK, sizeof(Id));
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

      /* realloc if crossing block boundary */
      if (((repo->idarraysize - 1) | IDARRAY_BLOCK) != ((repo->idarraysize + count - 1) | IDARRAY_BLOCK))
	repo->idarraydata = sat_realloc2(repo->idarraydata, (repo->idarraysize + count + IDARRAY_BLOCK) & ~IDARRAY_BLOCK, sizeof(Id));

      /* move old deps to end */
      olddeps = repo->lastoff = repo->idarraysize;
      memcpy(repo->idarraydata + olddeps, idstart, count - num);
      repo->idarraysize = olddeps + count - num;

      return olddeps;
    }

  if (olddeps)			       /* appending */
    repo->idarraysize--;

  /* realloc if crossing block boundary */
  if (((repo->idarraysize - 1) | IDARRAY_BLOCK) != ((repo->idarraysize + num - 1) | IDARRAY_BLOCK))
    repo->idarraydata = sat_realloc2(repo->idarraydata, (repo->idarraysize + num + IDARRAY_BLOCK) & ~IDARRAY_BLOCK, sizeof(Id));

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
  Id id, idp, idl, idns;
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
		  strncpy(dep - 9, "language:", 9);
		  *p++ = 0;
		  idl = str2id(pool, dep - 9, 1);
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
		  strncpy(dep - 9, "language:", 9);
		  idl = str2id(pool, dep - 9, 1);
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
	      idns = str2id(pool, "namespace:installed", 1);
	      id = str2id(pool, p, 1);
	      id = rel2id(pool, idns, id, REL_NAMESPACE, 1);
	      id = rel2id(pool, idp, id, REL_AND, 1);
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
	  idns = str2id(pool, "namespace:modalias", 1);
	  if (p && p != buf + 9 && strchr(p + 1, ':'))
	    {
	      *p++ = 0;
	      idp = str2id(pool, buf + 9, 1);
	      p[strlen(p) - 1] = 0;
	      id = str2id(pool, p, 1);
	      id = rel2id(pool, idns, id, REL_NAMESPACE, 1);
	      id = rel2id(pool, idp, id, REL_AND, 1);
	    }
	  else
	    {
	      p = buf + 9;
	      p[strlen(p) - 1] = 0;
	      id = str2id(pool, p, 1);
	      id = rel2id(pool, idns, id, REL_NAMESPACE, 1);
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

static unsigned char *
data_read_id(unsigned char *dp, Id *idp)
{
  Id x = 0;
  unsigned char c;
  for (;;)
    {
      c = *dp++;
      if (!(c & 0x80))
	{
	  *idp = (x << 7) ^ c;
          return dp;
	}
      x = (x << 7) ^ c ^ 128;
    }
}

static unsigned char *
data_skip(unsigned char *dp, int type)
{
  switch (type)
    {
    case TYPE_VOID:
      return dp;
    case TYPE_ID:
      while ((*dp & 0x80) != 0)
	dp++;
      return dp;
    case TYPE_IDARRAY:
    case TYPE_REL_IDARRAY:
    case TYPE_IDVALUEARRAY:
    case TYPE_IDVALUEVALUEARRAY:
      while ((*dp & 0xc0) != 0)
	dp++;
      return dp;
    default:
      fprintf(stderr, "unknown type in data_skip\n");
      exit(1);
    }
}

static unsigned char *
forward_to_key(Repodata *data, Id key, Id schema, unsigned char *dp)
{
  Id k, *keyp;

  keyp = data->schemadata + schema;
  while ((k = *keyp++) != 0)
    {
      if (k == key)
	return dp;
      if (data->keys[k].storage == KEY_STORAGE_VERTICAL_OFFSET)
	{
	  /* skip that offset */
	  dp = data_skip(dp, TYPE_ID);
	  continue;
	}
      if (data->keys[k].storage != KEY_STORAGE_INCORE)
	continue;
      dp = data_skip(dp, data->keys[k].type);
    }
  return 0;
}

static unsigned char *
load_page_range(Repodata *data, unsigned int pstart, unsigned int pend)
{
  /* add smart paging here */
  return 0;
}

static unsigned char *
get_data(Repodata *data, Repokey *key, unsigned char **dpp)
{
  Id off;
  unsigned char *dp = *dpp;
  int i, max;
  unsigned int pstart, pend, poff, plen;

  if (!dp)
    return 0;
  if (key->storage == KEY_STORAGE_INCORE)
    {
      *dpp = data_skip(dp, key->type);
      return dp;
    }
  if (key->storage != KEY_STORAGE_VERTICAL_OFFSET)
    return 0;
  if (!data->fp)
    return 0;
  dp = data_read_id(dp, &off);
  *dpp = dp;
  if (key->type == TYPE_VOID)
    return 0;
  max = key->size - off;
  if (max <= 0)
    return 0;
  /* we now have the offset, go into vertical */
  for (i = key - data->keys - 1; i > 0; i--)
    if (data->keys[i].storage == KEY_STORAGE_VERTICAL_OFFSET)
      off += data->keys[i].size;
  pstart = off / BLOB_PAGESIZE;
  pend = pstart;
  poff = off % BLOB_PAGESIZE;
  plen = BLOB_PAGESIZE - poff;
  for (;;)
    {
      if (plen > max)
	plen = max;
      dp = load_page_range(data, pstart, pend) + poff;
      if (!dp)
	return 0;
      switch (key->type)
	{
	case TYPE_STR:
	  if (memchr(dp, 0, plen))
	    return dp;
	  break;
	case TYPE_ID:
	  for (i = 0; i < plen; i++)
	    if ((dp[i] & 0x80) == 0)
	      return dp;
	  break;
	case TYPE_IDARRAY:
	case TYPE_REL_IDARRAY:
	case TYPE_IDVALUEARRAY:
	case TYPE_IDVALUEVALUEARRAY:
	  for (i = 0; i < plen; i++)
	    if ((dp[i] & 0xc0) == 0)
	      return dp;
	  break;
	}
      if (plen == max)
	return 0;
      pend++;
      plen += BLOB_PAGESIZE;
    }
}


const char *
repodata_lookup_str(Repodata *data, Id entry, Id key)
{
  Id schema;
  Id id, k, *kp, *keyp;
  unsigned char *dp;

  if (data->entryschemau8)
    schema = data->entryschemau8[entry];
  else
    schema = data->entryschema[entry];
  keyp = data->schemadata + schema;
  /* make sure the schema of this solvable contains the key */
  for (kp = keyp; (k = *kp++) != 0; )
    if (k == key)
      break;
  if (k == 0)
    return 0;
  dp = forward_to_key(data, key, schema, data->incoredata + data->incoreoffset[entry]);
  dp = get_data(data, data->keys + key, &dp);
  if (!dp)
    return 0;
  if (data->keys[key].type == TYPE_STR)
    return (const char *)dp;
  /* id type, must either use global or local string strore*/
  dp = data_read_id(dp, &id);
#if 0
  /* not yet working */
  return data->ss.stringspace + data->ss.strings[id];
#else
  return id2str(data->repo->pool, id);
#endif
}

#define SEARCH_NEXT_KEY		1
#define SEARCH_NEXT_SOLVABLE	2
#define SEACH_STOP		3

struct matchdata
{
  Pool *pool;
  const char *matchstr;
  int flags;
#if 0
  regex_t regex;
#endif
  int stop;
  int (*callback)(void *data, Solvable *s, Id key, const char *str);
  void *callback_data;
};

static void
domatch(Id p, Id key, struct matchdata *md, const char *str)
{
  /* fill match code here */
  md->stop = md->callback(md->callback_data, md->pool->solvables + p, key, str);
}

static void
repodata_search(Repodata *data, Id entry, Id key, struct matchdata *md)
{
  Id schema;
  Id id, k, *kp, *keyp;
  unsigned char *dp, *ddp;
  int onekey = 0;

  if (data->entryschemau8)
    schema = data->entryschemau8[entry];
  else
    schema = data->entryschema[entry];
  keyp = data->schemadata + schema;
  dp = data->incoredata + data->incoreoffset[entry];
  if (key)
    {
      /* search in a specific key */
      for (kp = keyp; (k = *kp++) != 0; )
	if (k == key)
	  break;
      if (k == 0)
	return;
      dp = forward_to_key(data, key, schema, dp);
      if (!dp)
	return;
      keyp = kp - 1;
      onekey = 1;
    }
  md->stop = 0;
  while ((key = *keyp++) != 0)
    {
      ddp = get_data(data, data->keys + key, &dp);
      while (ddp)
	{
	  switch (data->keys[key].type)
	    {
	    case TYPE_STR:
	      domatch(data->start + entry, data->keys[key].name, md, (const char *)ddp);
	      ddp = 0;
	      break;
	    case TYPE_ID:
	      data_read_id(ddp, &id);
	      /* domatch */
	      ddp = 0;
	      break;
	    case TYPE_IDARRAY:
	      ddp = data_read_id(ddp, &id);
	      if ((id & 0x40) == 0)
		ddp = 0;
	      id = (id & 0x3f) | ((id >> 1) & ~0x3f);
	      /* domatch */
	      while (md->stop == SEARCH_NEXT_KEY && ddp)
		ddp = data_read_id(ddp, &id);
	      break;
	    default:
	      ddp = 0;
	    }
	}
      if (onekey || md->stop > SEARCH_NEXT_KEY)
	return;
    }
}

static void
domatch_idarray(Id p, Id key, struct matchdata *md, Id *ida)
{
  for (; *ida && !md->stop; ida++)
    domatch(p, key, md, id2str(md->pool, *ida));
}

static void
repo_search_md(Repo *repo, Id p, Id key, struct matchdata *md)
{
  Pool *pool = repo->pool;
  Repodata *data;
  Solvable *s;
  int i;

  md->stop = 0;
  if (!p)
    {
      for (p = repo->start, s = repo->pool->solvables + p; p < repo->end; p++, s++)
	{
	  if (s->repo == repo)
            repo_search_md(repo, p, key, md);
	  if (md->stop > SEARCH_NEXT_SOLVABLE)
	    break;
	}
      return;
    }
  s = pool->solvables + p;
  switch(key)
    {
      case 0:
      case SOLVABLE_NAME:
	if (s->name)
	  domatch(p, SOLVABLE_NAME, md, id2str(pool, s->name));
	if (key || md->stop > SEARCH_NEXT_KEY)
	  return;
      case SOLVABLE_ARCH:
	if (s->arch)
	  domatch(p, SOLVABLE_ARCH, md, id2str(pool, s->arch));
	if (key || md->stop > SEARCH_NEXT_KEY)
	  return;
      case SOLVABLE_EVR:
	if (s->evr)
	  domatch(p, SOLVABLE_EVR, md, id2str(pool, s->evr));
	if (key || md->stop > SEARCH_NEXT_KEY)
	  return;
      case SOLVABLE_VENDOR:
	if (s->vendor)
	  domatch(p, SOLVABLE_VENDOR, md, id2str(pool, s->vendor));
	if (key || md->stop > SEARCH_NEXT_KEY)
	  return;
      case SOLVABLE_PROVIDES:
        if (s->provides)
	  domatch_idarray(p, SOLVABLE_PROVIDES, md, repo->idarraydata + s->provides);
	if (key || md->stop > SEARCH_NEXT_KEY)
	  return;
      case SOLVABLE_OBSOLETES:
        if (s->obsoletes)
	  domatch_idarray(p, SOLVABLE_OBSOLETES, md, repo->idarraydata + s->obsoletes);
	if (key || md->stop > SEARCH_NEXT_KEY)
	  return;
      case SOLVABLE_CONFLICTS:
        if (s->obsoletes)
	  domatch_idarray(p, SOLVABLE_CONFLICTS, md, repo->idarraydata + s->conflicts);
	if (key || md->stop > SEARCH_NEXT_KEY)
	  return;
      case SOLVABLE_REQUIRES:
        if (s->requires)
	  domatch_idarray(p, SOLVABLE_REQUIRES, md, repo->idarraydata + s->requires);
	if (key || md->stop > SEARCH_NEXT_KEY)
	  return;
      case SOLVABLE_RECOMMENDS:
        if (s->recommends)
	  domatch_idarray(p, SOLVABLE_RECOMMENDS, md, repo->idarraydata + s->recommends);
	if (key || md->stop > SEARCH_NEXT_KEY)
	  return;
      case SOLVABLE_SUPPLEMENTS:
        if (s->supplements)
	  domatch_idarray(p, SOLVABLE_SUPPLEMENTS, md, repo->idarraydata + s->supplements);
	if (key || md->stop > SEARCH_NEXT_KEY)
	  return;
      case SOLVABLE_SUGGESTS:
        if (s->suggests)
	  domatch_idarray(p, SOLVABLE_SUGGESTS, md, repo->idarraydata + s->suggests);
	if (key || md->stop > SEARCH_NEXT_KEY)
	  return;
      case SOLVABLE_ENHANCES:
        if (s->enhances)
	  domatch_idarray(p, SOLVABLE_ENHANCES, md, repo->idarraydata + s->enhances);
	if (key || md->stop > SEARCH_NEXT_KEY)
	  return;
      case SOLVABLE_FRESHENS:
        if (s->freshens)
	  domatch_idarray(p, SOLVABLE_FRESHENS, md, repo->idarraydata + s->freshens);
	if (key || md->stop > SEARCH_NEXT_KEY)
	  return;
      default:
	break;
    }

  for (i = 0, data = repo->repodata; i < repo->nrepodata; i++, data++)
    {
      if (p < data->start || p >= data->end)
	continue;
      repodata_search(data, p - data->start, key, md);
      if (md->stop > SEARCH_NEXT_KEY)
	break;
    }
}

void
repo_search(Repo *repo, Id p, Id key, const char *match, int flags, int (*callback)(void *data, Solvable *s, Id key, const char *str), void *callback_data)
{
  struct matchdata md;

  memset(&md, 0, sizeof(md));
  md.pool = repo->pool;
  md.matchstr = match;
  md.flags = flags;
  md.callback = callback;
  md.callback_data = callback_data;
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

// EOF
