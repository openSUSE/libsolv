/*
 * Copyright (c) 2007, Novell Inc.
 *
 * This program is licensed under the BSD license, read LICENSE.BSD
 * for further information
 */

/*
 * pool.c
 * 
 * The pool contains information about solvables
 * stored optimized for memory consumption and fast retrieval.
 */

#include <stdio.h>
#include <stdlib.h>
#include <stdarg.h>
#include <unistd.h>
#include <string.h>

#include "pool.h"
#include "repo.h"
#include "poolid.h"
#include "poolid_private.h"
#include "poolarch.h"
#include "util.h"
#include "bitmap.h"
#include "evr.h"

#define SOLVABLE_BLOCK	255

/*
 * list of string constants, so we can do pointer/Id instead of string comparison
 * index into array matches ID_xxx constants in pool.h
 */
  
static const char *initpool_data[] = {
  "<NULL>",                   // ID_NULL
  "",                         // ID_EMPTY
  "solvable:name",
  "solvable:arch",
  "solvable:evr",
  "solvable:vendor",
  "solvable:provides",
  "solvable:obsoletes",
  "solvable:conflicts",
  "solvable:requires",
  "solvable:recommends",
  "solvable:suggests",
  "solvable:supplements",
  "solvable:enhances",
  "solvable:freshens",
  "rpm:dbid",			       /* direct key into rpmdb */
  "solvable:prereqmarker",
  "solvable:filemarker",
  "namespace:installed",
  "namespace:modalias",
  "namespace:splitprovides",
  "namespace:language",
  "system:system",
  "src",
  "nosrc",
  "noarch",
  "repodata:external",
  "repodata:keys",
  "repodata:location",
  0
};

/* create pool */
Pool *
pool_create(void)
{
  Pool *pool;
  Solvable *s;

  pool = (Pool *)sat_calloc(1, sizeof(*pool));

  stringpool_init (&pool->ss, initpool_data);

  /* alloc space for ReDep 0 */
  pool->rels = sat_extend_resize(0, 1, sizeof(Reldep), REL_BLOCK);
  pool->nrels = 1;
  memset(pool->rels, 0, sizeof(Reldep));

  /* alloc space for Solvable 0 and system solvable */
  pool->solvables = sat_extend_resize(0, 2, sizeof(Solvable), SOLVABLE_BLOCK);
  pool->nsolvables = 2;
  memset(pool->solvables, 0, 2 * sizeof(Solvable));
  s = pool->solvables + SYSTEMSOLVABLE;
  s->name = SYSTEM_SYSTEM;
  s->arch = ARCH_NOARCH;
  s->evr = ID_EMPTY;

  queue_init(&pool->vendormap);

  pool->debugmask = SAT_DEBUG_RESULT;	/* FIXME */
  return pool;
}


/* free all the resources of our pool */
void
pool_free(Pool *pool)
{
  int i;

  pool_freewhatprovides(pool);
  pool_freeidhashes(pool);
  repo_freeallrepos(pool, 1);
  sat_free(pool->id2arch);
  sat_free(pool->solvables);
  sat_free(pool->ss.stringspace);
  sat_free(pool->ss.strings);
  sat_free(pool->rels);
  queue_free(&pool->vendormap);
  for (i = 0; i < DEP2STRBUF; i++)
    sat_free(pool->dep2strbuf[i]);
  sat_free(pool);
}

Id
pool_add_solvable(Pool *pool)
{
  pool->solvables = sat_extend(pool->solvables, pool->nsolvables, 1, sizeof(Solvable), SOLVABLE_BLOCK);
  memset(pool->solvables + pool->nsolvables, 0, sizeof(Solvable));
  return pool->nsolvables++;
}

Id
pool_add_solvable_block(Pool *pool, int count)
{
  Id nsolvables = pool->nsolvables;
  if (!count)
    return nsolvables;
  pool->solvables = sat_extend(pool->solvables, pool->nsolvables, count, sizeof(Solvable), SOLVABLE_BLOCK);
  memset(pool->solvables + nsolvables, 0, sizeof(Solvable) * count);
  pool->nsolvables += count;
  return nsolvables;
}

void
pool_free_solvable_block(Pool *pool, Id start, int count, int reuseids)
{
  if (!count)
    return;
  if (reuseids && start + count == pool->nsolvables)
    {
      /* might want to shrink solvable array */
      pool->nsolvables = start;
      return;
    }
  memset(pool->solvables + start, 0, sizeof(Solvable) * count);
}


const char *
solvable2str(Pool *pool, Solvable *s)
{
  int l, nn = pool->dep2strn;
  const char *n, *e, *a;
  n = id2str(pool, s->name);
  e = id2str(pool, s->evr);
  a = id2str(pool, s->arch);
  l = strlen(n) + strlen(e) + strlen(a) + 3;
  if (l > pool->dep2strlen[nn])
    {
      pool->dep2strbuf[nn] = sat_realloc(pool->dep2strbuf[nn], l + 32);
      pool->dep2strlen[nn] = l + 32;
    }
  sprintf(pool->dep2strbuf[nn], "%s-%s.%s", n, e, a);
  pool->dep2strn = (nn + 1) % DEP2STRBUF;
  return pool->dep2strbuf[nn];
}

static Pool *pool_shrink_whatprovides_sortcmp_data;

static int
pool_shrink_whatprovides_sortcmp(const void *ap, const void *bp)
{
  int r;
  Pool *pool = pool_shrink_whatprovides_sortcmp_data;
  Id oa, ob, *da, *db;
  oa = pool->whatprovides[*(Id *)ap];
  ob = pool->whatprovides[*(Id *)bp];
  if (oa == ob)
    return *(Id *)ap - *(Id *)bp;
  if (!oa)
    return -1;
  if (!ob)
    return 1;
  da = pool->whatprovidesdata + oa;
  db = pool->whatprovidesdata + ob;
  while (*db)
    if ((r = (*da++ - *db++)) != 0)
      return r;
  if (*da)
    return *da;
  return *(Id *)ap - *(Id *)bp;
}

/*
 * pool_shrink_whatprovides  - unify whatprovides data
 *
 * whatprovides_rel must be empty for this to work!
 *
 */
static void
pool_shrink_whatprovides(Pool *pool)
{
  Id i, id;
  Id *sorted;
  Id lastid, *last, *dp, *lp;
  Offset o;
  int r;

  if (pool->ss.nstrings < 3)
    return;
  sorted = sat_malloc2(pool->ss.nstrings, sizeof(Id));
  for (id = 0; id < pool->ss.nstrings; id++)
    sorted[id] = id;
  pool_shrink_whatprovides_sortcmp_data = pool;
  qsort(sorted + 1, pool->ss.nstrings - 1, sizeof(Id), pool_shrink_whatprovides_sortcmp);
  last = 0;
  lastid = 0;
  for (i = 1; i < pool->ss.nstrings; i++)
    {
      id = sorted[i];
      o = pool->whatprovides[id];
      if (o == 0 || o == 1)
	continue;
      dp = pool->whatprovidesdata + o;
      if (last)
	{
	  lp = last;
	  while (*dp)	
	    if (*dp++ != *lp++)
	      {
		last = 0;
	        break;
	      }
	  if (last && *lp)
	    last = 0;
	  if (last)
	    {
	      pool->whatprovides[id] = -lastid;
	      continue;
	    }
	}
      last = pool->whatprovidesdata + o;
      lastid = id;
    }
  sat_free(sorted);
  dp = pool->whatprovidesdata + 2;
  for (id = 1; id < pool->ss.nstrings; id++)
    {
      o = pool->whatprovides[id];
      if (o == 0 || o == 1)
	continue;
      if ((Id)o < 0)
	{
	  i = -(Id)o;
	  if (i >= id)
	    abort();
	  pool->whatprovides[id] = pool->whatprovides[i];
	  continue;
	}
      lp = pool->whatprovidesdata + o;
      if (lp < dp)
	abort();
      pool->whatprovides[id] = dp - pool->whatprovidesdata;
      while ((*dp++ = *lp++) != 0)
	;
    }
  o = dp - pool->whatprovidesdata;
  POOL_DEBUG(SAT_DEBUG_STATS, "shrunk whatprovidesdata from %d to %d\n", pool->whatprovidesdataoff, o);
  if (pool->whatprovidesdataoff == o)
    return;
  r = pool->whatprovidesdataoff - o;
  pool->whatprovidesdataoff = o;
  pool->whatprovidesdata = sat_realloc(pool->whatprovidesdata, (o + pool->whatprovidesdataleft) * sizeof(Id));
  if (r > pool->whatprovidesdataleft)
    r = pool->whatprovidesdataleft;
  memset(pool->whatprovidesdata + o, 0, r * sizeof(Id));
}


/*
 * pool_createwhatprovides()
 * 
 * create hashes over pool of solvables to ease provide lookups
 * 
 */
void
pool_createwhatprovides(Pool *pool)
{
  int i, num, np, extra;
  Offset off;
  Solvable *s;
  Id id;
  Offset *idp, n;
  Offset *whatprovides;
  Id *whatprovidesdata, *d;

  POOL_DEBUG(SAT_DEBUG_STATS, "number of solvables: %d\n", pool->nsolvables);
  POOL_DEBUG(SAT_DEBUG_STATS, "number of ids: %d + %d\n", pool->ss.nstrings, pool->nrels);

  pool_freeidhashes(pool);	/* XXX: should not be here! */
  pool_freewhatprovides(pool);
  num = pool->ss.nstrings;
  pool->whatprovides = whatprovides = sat_extend_resize(0, num, sizeof(Offset), WHATPROVIDES_BLOCK);
  memset(whatprovides, 0, num * sizeof(Offset));
  pool->whatprovides_rel = sat_extend_resize(0, pool->nrels, sizeof(Offset), WHATPROVIDES_BLOCK);
  memset(pool->whatprovides_rel, 0, pool->nrels * sizeof(Offset));

  /* count providers for each name */
  for (i = 1; i < pool->nsolvables; i++)
    {
      Id *pp;
      s = pool->solvables + i;
      if (!s->provides)
	continue;
      if (!pool_installable(pool, s))
	continue;
      pp = s->repo->idarraydata + s->provides;
      while ((id = *pp++) != ID_NULL)
	{
	  while (ISRELDEP(id))
	    {
	      Reldep *rd = GETRELDEP(pool, id);
	      id = rd->name;
	    }
	  whatprovides[id]++;	       /* inc count of providers */
	}
    }

  off = 2;	/* first entry is undef, second is empty list */
  idp = whatprovides;
  np = 0;			       /* number of names provided */
  for (i = 0; i < num; i++, idp++)
    {
      n = *idp;
      if (!n)			       /* no providers */
	continue;
      *idp = off;		       /* move from counts to offsets into whatprovidesdata */
      off += n + 1;		       /* make space for all providers + terminating ID_NULL */
      np++;			       /* inc # of provider 'slots' */
    }

  POOL_DEBUG(SAT_DEBUG_STATS, "provide ids: %d\n", np);

  /* reserve some space for relation data */
  extra = 2 * pool->nrels;
  if (extra < 256)
    extra = 256;

  POOL_DEBUG(SAT_DEBUG_STATS, "provide space needed: %d + %d\n", off, extra);

  /* alloc space for all providers + extra */
  whatprovidesdata = sat_calloc(off + extra, sizeof(Id));

  /* now fill data for all provides */
  for (i = 1; i < pool->nsolvables; i++)
    {
      Id *pp;
      s = pool->solvables + i;
      if (!s->provides)
	continue;
      if (!pool_installable(pool, s))
	continue;

      /* for all provides of this solvable */
      pp = s->repo->idarraydata + s->provides;
      while ((id = *pp++) != 0)
	{
	  while (ISRELDEP(id))
	    {
	      Reldep *rd = GETRELDEP(pool, id);
	      id = rd->name;
	    }
	  d = whatprovidesdata + whatprovides[id];   /* offset into whatprovidesdata */
	  if (*d)
	    {
	      d++;
	      while (*d)	       /* find free slot */
		d++;
	      if (d[-1] == i)
		continue;
	    }
	  *d = i;		       /* put solvable Id into data */
	}
    }
  pool->whatprovidesdata = whatprovidesdata;
  pool->whatprovidesdataoff = off;
  pool->whatprovidesdataleft = extra;
  pool_shrink_whatprovides(pool);
}

/*
 * free all of our whatprovides data
 * be careful, everything internalized with pool_queuetowhatprovides is gone, too
 */
void
pool_freewhatprovides(Pool *pool)
{
  pool->whatprovides = sat_free(pool->whatprovides);
  pool->whatprovides_rel = sat_free(pool->whatprovides_rel);
  pool->whatprovidesdata = sat_free(pool->whatprovidesdata);
  pool->whatprovidesdataoff = 0;
  pool->whatprovidesdataleft = 0;
}


/******************************************************************************/

/*
 * pool_queuetowhatprovides  - add queue contents to whatprovidesdata
 * 
 * on-demand filling of provider information
 * move queue data into whatprovidesdata
 * q: queue of Ids
 * returns: Offset into whatprovides
 *
 */
Id
pool_queuetowhatprovides(Pool *pool, Queue *q)
{
  Offset off;
  int count = q->count;

  if (count == 0)		       /* queue empty -> ID_EMPTY */
    return ID_EMPTY;

  /* extend whatprovidesdata if needed, +1 for ID_NULL-termination */
  if (pool->whatprovidesdataleft < count + 1)
    {
      POOL_DEBUG(SAT_DEBUG_STATS, "growing provides hash data...\n");
      pool->whatprovidesdata = sat_realloc(pool->whatprovidesdata, (pool->whatprovidesdataoff + count + 4096) * sizeof(Id));
      pool->whatprovidesdataleft = count + 4096;
    }

  /* copy queue to next free slot */
  off = pool->whatprovidesdataoff;
  memcpy(pool->whatprovidesdata + pool->whatprovidesdataoff, q->elements, count * sizeof(Id));

  /* adapt count and ID_NULL-terminate */
  pool->whatprovidesdataoff += count;
  pool->whatprovidesdata[pool->whatprovidesdataoff++] = ID_NULL;
  pool->whatprovidesdataleft -= count + 1;

  return (Id)off;
}


/*************************************************************************/

/*
 * addrelproviders
 * 
 * add packages fulfilling the relation to whatprovides array
 * no exact providers, do range match
 * 
 */

Id *
pool_addrelproviders(Pool *pool, Id d)
{
  Reldep *rd = GETRELDEP(pool, d);
  Reldep *prd;
  Queue plist;
  Id buf[16];
  Id name = rd->name;
  Id evr = rd->evr;
  int flags = rd->flags;
  Id pid, *pidp;
  Id p, *pp, *pp2, *pp3;

  d = GETRELID(d);
  queue_init_buffer(&plist, buf, sizeof(buf)/sizeof(*buf));
  switch (flags)
    {
    case REL_AND:
    case REL_WITH:
      pp = pool_whatprovides(pool, name);
      pp2 = pool_whatprovides(pool, evr);
      while ((p = *pp++) != 0)
	{
	  for (pp3 = pp2; *pp3;)
	    if (*pp3++ == p)
	      {
	        queue_push(&plist, p);
		break;
	      }
	}
      break;
    case REL_OR:
      pp = pool_whatprovides(pool, name);
      while ((p = *pp++) != 0)
	queue_push(&plist, p);
      pp = pool_whatprovides(pool, evr);
      while ((p = *pp++) != 0)
	queue_pushunique(&plist, p);
      break;
    case REL_NAMESPACE:
      if (pool->nscallback)
	{
	  p = pool->nscallback(pool, pool->nscallbackdata, name, evr);
	  if (p > 1)
	    {
	      queue_free(&plist);
	      pool->whatprovides_rel[d] = p;
	      return pool->whatprovidesdata + p;
	    }
	  if (p == 1)
	    queue_push(&plist, SYSTEMSOLVABLE);
	}
      break;
    default:
      break;
    }

  /* convert to whatprovides id */
#if 0
  POOL_DEBUG(DEBUG_1, "addrelproviders: what provides %s?\n", id2str(pool, name));
#endif
  if (flags && flags < 8)
    {
      FOR_PROVIDES(p, pp, name)
	{
#if 0
	  POOL_DEBUG(DEBUG_1, "addrelproviders: checking package %s\n", id2str(pool, pool->p[p].name));
#endif
	  /* solvable p provides name in some rels */
	  pidp = pool->solvables[p].repo->idarraydata + pool->solvables[p].provides;
	  while ((pid = *pidp++) != 0)
	    {
	      int pflags;
	      Id pevr;

	      if (pid == name)
		{
#ifdef DEBIAN_SEMANTICS
		  continue;		/* unversioned provides can
				 	 * never match versioned deps */
#else
		  break;		/* yes, provides all versions */
#endif
		}
	      if (!ISRELDEP(pid))
		continue;		/* wrong provides name */
	      prd = GETRELDEP(pool, pid);
	      if (prd->name != name)
		continue;		/* wrong provides name */
	      /* right package, both deps are rels */
	      pflags = prd->flags;
	      if (!pflags)
		continue;
	      if (flags == 7 || pflags == 7)
		break; /* included */
	      if ((pflags & flags & 5) != 0)
		break; /* same direction, match */
	      pevr = prd->evr;
	      if (pevr == evr)
		{
		  if ((pflags & flags & 2) != 0)
		    break; /* both have =, match */
		}
	      else
		{
		  int f = flags == 5 ? 5 : flags == 2 ? pflags : (flags ^ 5) & (pflags | 5);
		  if ((f & (1 << (1 + evrcmp(pool, pevr, evr, EVRCMP_MATCH_RELEASE)))) != 0)
		    break;
		}
	    }
	  if (!pid)
	    continue;	/* no rel match */
	  queue_push(&plist, p);
	}
      /* make our system solvable provide all unknown rpmlib() stuff */
      if (plist.count == 0 && !strncmp(id2str(pool, name), "rpmlib(", 7))
	queue_push(&plist, SYSTEMSOLVABLE);
    }
  /* add providers to whatprovides */
#if 0
  POOL_DEBUG(DEBUG_1, "addrelproviders: adding %d packages to %d\n", plist.count, d);
#endif
  pool->whatprovides_rel[d] = pool_queuetowhatprovides(pool, &plist);
  queue_free(&plist);

  return pool->whatprovidesdata + pool->whatprovides_rel[d];
}

/*************************************************************************/

void
pool_debug(Pool *pool, int type, const char *format, ...)
{
  va_list args;
  char buf[1024];

  if ((type & (SAT_FATAL|SAT_ERROR)) == 0)
    {
      if ((pool->debugmask & type) == 0)
	return;
    }
  va_start(args, format);
  if (!pool->debugcallback)
    {
      if ((type & (SAT_FATAL|SAT_ERROR)) == 0)
        vprintf(format, args);
      else
        vfprintf(stderr, format, args);
      return;
    }
  vsnprintf(buf, sizeof(buf), format, args);
  pool->debugcallback(pool, pool->debugcallbackdata, type, buf);
}

void
pool_setdebuglevel(Pool *pool, int level)
{
  int mask = SAT_DEBUG_RESULT;
  if (level > 0)
    mask |= SAT_DEBUG_STATS|SAT_DEBUG_ANALYZE|SAT_DEBUG_UNSOLVABLE;
  if (level > 1)
    mask |= SAT_DEBUG_JOB|SAT_DEBUG_SOLUTIONS|SAT_DEBUG_POLICY;
  if (level > 2)
    mask |= SAT_DEBUG_PROPAGATE;
  if (level > 3)
    mask |= SAT_DEBUG_RULE_CREATION;
  if (level > 4)
    mask |= SAT_DEBUG_SCHUBI;
  pool->debugmask = mask;
}

/*************************************************************************/

struct searchfiles {
  Id *ids;
  char **dirs;
  char **names;
  int nfiles;
  Map seen;
};

#define SEARCHFILES_BLOCK 127

static void
pool_addfileprovides_dep(Pool *pool, Id *ida, struct searchfiles *sf, struct searchfiles *isf)
{
  Id dep, sid;
  const char *s, *sr;

  while ((dep = *ida++) != 0)
    {
      while (ISRELDEP(dep))
	{
	  Reldep *rd;
	  sid = pool->ss.nstrings + GETRELID(dep);
	  if (MAPTST(&sf->seen, sid))
	    {
	      dep = 0;
	      break;
	    }
	  MAPSET(&sf->seen, sid);
	  rd = GETRELDEP(pool, dep);
	  if (rd->flags < 8)
	    dep = rd->name;
	  else if (rd->flags == REL_NAMESPACE)
	    {
	      if (isf && (rd->name == NAMESPACE_INSTALLED || rd->name == NAMESPACE_SPLITPROVIDES))
		{
		  sf = isf;
		  isf = 0;
		  if (MAPTST(&sf->seen, sid))
		    {
		      dep = 0;
		      break;
		    }
		  MAPSET(&sf->seen, sid);
		}
	      dep = rd->evr;
	    }
	  else
	    {
	      Id ids[2];
	      ids[0] = rd->name;
	      ids[1] = 0;
	      pool_addfileprovides_dep(pool, ids, sf, isf);
	      dep = rd->evr;
	    }
	}
      if (!dep)
	continue;
      if (MAPTST(&sf->seen, dep))
	continue;
      MAPSET(&sf->seen, dep);
      s = id2str(pool, dep);
      if (*s != '/')
	continue;
      sf->ids = sat_extend(sf->ids, sf->nfiles, 1, sizeof(const char *), SEARCHFILES_BLOCK);
      sf->dirs = sat_extend(sf->dirs, sf->nfiles, 1, sizeof(const char *), SEARCHFILES_BLOCK);
      sf->names = sat_extend(sf->names, sf->nfiles, 1, sizeof(const char *), SEARCHFILES_BLOCK);
      sf->ids[sf->nfiles] = dep;
      sr = strrchr(s, '/');
      sf->names[sf->nfiles] = strdup(sr + 1);
      sf->dirs[sf->nfiles] = sat_malloc(sr - s + 1);
      if (sr != s)
        strncpy(sf->dirs[sf->nfiles], s, sr - s);
      sf->dirs[sf->nfiles][sr - s] = 0;
      sf->nfiles++;
    }
}

struct addfileprovides_cbdata {
  int nfiles;
  Id *ids;
  char **dirs;
  char **names;

  Repodata *olddata;
  Id *dids;
  Map useddirs;
};

static int
addfileprovides_cb(void *cbdata, Solvable *s, Repodata *data, Repokey *key, KeyValue *value)
{
  struct addfileprovides_cbdata *cbd = cbdata;
  int i;

  if (data != cbd->olddata)
    {
      map_free(&cbd->useddirs);
      map_init(&cbd->useddirs, data->dirpool.ndirs);
      for (i = 0; i < cbd->nfiles; i++)
	{
	  Id did = repodata_str2dir(data, cbd->dirs[i], 0);
          cbd->dids[i] = did;
	  if (did)
	    MAPSET(&cbd->useddirs, did);
	}
      cbd->olddata = data;
    }
  if (!MAPTST(&cbd->useddirs, value->id))
    return 0;
  for (i = 0; i < cbd->nfiles; i++)
    {
      if (cbd->dids[i] != value->id)
	continue;
      if (!strcmp(cbd->names[i], value->str))
	break;
    }
  if (i == cbd->nfiles)
    return 0;
  s->provides = repo_addid_dep(s->repo, s->provides, cbd->ids[i], SOLVABLE_FILEMARKER);
  return 0;
}

void
pool_addfileprovides(Pool *pool, Repo *installed)
{
  Solvable *s;
  Repo *repo;
  struct searchfiles sf, isf;
  struct addfileprovides_cbdata cbd;
  int i;
  Id id_filelist;

  id_filelist = str2id(pool, "filelist", 1);

  memset(&sf, 0, sizeof(sf));
  map_init(&sf.seen, pool->ss.nstrings + pool->nrels);
  memset(&isf, 0, sizeof(isf));
  map_init(&isf.seen, pool->ss.nstrings + pool->nrels);

  for (i = 1, s = pool->solvables + i; i < pool->nsolvables; i++, s++)
    {
      repo = s->repo;
      if (!repo)
	continue;
      if (s->obsoletes)
        pool_addfileprovides_dep(pool, repo->idarraydata + s->obsoletes, &sf, &isf);
      if (s->conflicts)
        pool_addfileprovides_dep(pool, repo->idarraydata + s->conflicts, &sf, &isf);
      if (s->requires)
        pool_addfileprovides_dep(pool, repo->idarraydata + s->requires, &sf, &isf);
      if (s->recommends)
        pool_addfileprovides_dep(pool, repo->idarraydata + s->recommends, &sf, &isf);
      if (s->suggests)
        pool_addfileprovides_dep(pool, repo->idarraydata + s->suggests, &sf, &isf);
      if (s->supplements)
        pool_addfileprovides_dep(pool, repo->idarraydata + s->supplements, &sf, &isf);
      if (s->enhances)
        pool_addfileprovides_dep(pool, repo->idarraydata + s->enhances, &sf, &isf);
      if (s->freshens)
        pool_addfileprovides_dep(pool, repo->idarraydata + s->freshens, &sf, &isf);
    }
  map_free(&sf.seen);
  map_free(&isf.seen);
  POOL_DEBUG(SAT_DEBUG_STATS, "found %d file dependencies\n", sf.nfiles);
  POOL_DEBUG(SAT_DEBUG_STATS, "found %d installed file dependencies\n", isf.nfiles);
  cbd.dids = 0;
  map_init(&cbd.useddirs, 1);
  if (sf.nfiles)
    {
#if 0
      for (i = 0; i < sf.nfiles; i++)
	POOL_DEBUG(SAT_DEBUG_STATS, "looking up %s in filelist\n", id2str(pool, sf.ids[i]));
#endif
      cbd.nfiles = sf.nfiles;
      cbd.ids = sf.ids;
      cbd.dirs = sf.dirs;
      cbd.names = sf.names;
      cbd.olddata = 0;
      cbd.dids = sat_realloc2(cbd.dids, sf.nfiles, sizeof(Id));
      pool_search(pool, 0, id_filelist, 0, 0, addfileprovides_cb, &cbd);
      sat_free(sf.ids);
      for (i = 0; i < sf.nfiles; i++)
	{
	  sat_free(sf.dirs[i]);
	  sat_free(sf.names[i]);
	}
      sat_free(sf.dirs);
      sat_free(sf.names);
    }
  if (isf.nfiles && installed)
    {
#if 0
      for (i = 0; i < isf.nfiles; i++)
	POOL_DEBUG(SAT_DEBUG_STATS, "looking up %s in installed filelist\n", id2str(pool, isf.ids[i]));
#endif
      cbd.nfiles = isf.nfiles;
      cbd.ids = isf.ids;
      cbd.dirs = isf.dirs;
      cbd.names = isf.names;
      cbd.olddata = 0;
      cbd.dids = sat_realloc2(cbd.dids, isf.nfiles, sizeof(Id));
      repo_search(installed, 0, id_filelist, 0, 0, addfileprovides_cb, &cbd);
      sat_free(isf.ids);
      for (i = 0; i < isf.nfiles; i++)
	{
	  sat_free(isf.dirs[i]);
	  sat_free(isf.names[i]);
	}
      sat_free(isf.dirs);
      sat_free(isf.names);
    }
  map_free(&cbd.useddirs);
  sat_free(cbd.dids);
  pool_freewhatprovides(pool);	/* as we have added provides */
}

#if 0

struct mountpoint {
  const char *path;
  int kbytes;
  int files;
};

struct mptree {
  Id sibling;
  Id child;
  const char *comp;
  int compl;
  Id mountpoint;
};

struct cbdata {
  struct mountpoint *mps;
  Id *dirmap;
  int nmap;
};

static int
pool_fill_DU_add_cb(void *data, Solvable *s, Id key, const char *str)
{
  struct cbdata *cbdata = data;
  Id mp, dirnum, kbytes, files;

  dp = data_read_id(dp, &dirnum);
  dp = data_read_id(dp, &kbytes);
  data_read_id(dp, &files);
  if (dirnum < 0 || dirnum > cbdata->nmap)
    return 0;
  mp = cbdata->dirmap[dirnum];
  if (mp >= 0)
    {
      cbdata->mps[mp].kbytes += kbytes;
      cbdata->mps[mp].files += files;
    }
  return 0;
}

static int
pool_fill_DU_sub_cb(void *data, Solvable *s, Id key, const char *str)
{
  struct cbdata *cbdata = data;
  Id mp, dirnum, kbytes, files;

  dp = data_read_id(dp, &dirnum);
  dp = data_read_id(dp, &kbytes);
  data_read_id(dp, &files);
  if (dirnum < 0 || dirnum > cbdata->nmap)
    return 0;
  mp = cbdata->dirmap[dirnum];
  if (mp >= 0)
    {
      cbdata->mps[mp].kbytes -= kbytes;
      cbdata->mps[mp].files -= files;
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

void
pool_fill_DU(Pool *pool, struct mountpoint *mps, int nmps)
{
  char *path, *p;
  Id *dirmap;
  struct mptree *mptree;
  int nmptree;
  int pos;
  int mp;

  struct matchdata md;
  struct cbdata cbdata;

  memset(&md, 0, sizeof(md));
  md.pool = 0;
  md.matchstr = 0;
  md.flags = 0;
  md.callback = 0;
  md.callback_data = &cbdata

  cbdata.mps = mps;
  cbdata.dirmap = 0;
  cbdata.nmap = 0;

  mptree = sat_malloc2(16, sizeof(mptree));

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
      pos = 0;
      path = mps[mp].path;
      while(*path == '/')
	path++;
      while (*path)
	{
	  if ((p = strchr('/', path)) == 0)
	    {
	      comp = path;
	      compl = strlen(comp);
	      path += compl;
	    }
	  else
	    {
	      comp = path;
	      compl = p - path;
	      path = p + 1;
	      while(*path == '/')
		path++;
	    }
          for (i = mptree[pos].child; i; i = mptree[i].sibling)
	    if (mptree[i].compl == compl && !strncmp(mptree[i].comp, comp, compl))
	      break;
	  if (!i)
	    {
	      /* create new node */
	      if ((nmptree & 15) == 0)
		mptree = sat_realloc2(mptree, nmptree + 16, sizeof(mptree));
	      i = nmptree++;
	      mptree[i].sibling = mptree[pos].child;
	      mptree[i].child = 0;
	      mptree[i].comp = comp;
	      mptree[i].compl = compl;
	      mptree[i].mountpoint = -1;
	      mptree[pos].child = i;
	    }
	  pos = i;
	}
      mptree[pos].mountpoint = mp;
    }
  propagate_mountpoints(mptree, 0, mptree[0].mountpoint);

  for_all_repos
    {
      for_all_repodatas_containing_DU
	{
	  /* create map from dir to mptree */
	  dirmap = xcalloc2(pool->ndirs, sizeof(Id));
	  mp = 0;
	  for (dn = 2, dirs = pool->dirs + dn; dn < pool->ndirs; dn++)
	    {
	      id = *dirs++;
	      if (id <= 0)
		{
		  mp = dirmap[-id];
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
	      comp = id2str(pool, id);
	      compl = strlen(comp);
	      for (i = mptree[mp].child; i; i = mptree[i].sibling)
		if (mptree[i].compl == compl && !strncmp(mptree[i].comp, comp, compl))
		  break;
	      dirmap[dn] = i ? i : -mp;
	    }
	  /* change dirmap to point to mountpoint instead of mptree */
	  for (dn = 0; dn < pool->ndirs; dn++)
	    {
	      mp = dirmap[i];
	      dirmap[i] = mptree[mp > 0 ? mp : -mp].mountpoint;
	    }

	  cbdata.nmap = pool->ndirs;
	  cbdata.dirmap = dirmap;

	  md.callback = pool_fill_DU_add_cb;
	  for_solvables_to_be_installed()
	    {
	      if (p < data->start || p >= data->end)
		continue;
	      repodata_search(data, p - data->start, SOLVABLE_DUDATA, &md);
	    }
	  md.callback = pool_fill_DU_sub_cb;
	  for_solvables_to_be_erased()
	    {
	      if (p < data->start || p >= data->end)
		continue;
	      repodata_search(data, p - data->start, SOLVABLE_DUDATA, &md);
	    }

	  cbdata.dirmap = 0;
	  cbdata.nmap = 0;
	  sat_free(dirmap);
	}
    }
}

#endif

void pool_search(Pool *pool, Id p, Id key, const char *match, int flags, int (*callback)(void *cbdata, Solvable *s, struct _Repodata *data, struct _Repokey *key, struct _KeyValue *kv), void *cbdata)
{
  if (p)
    {
      if (pool->solvables[p].repo)
        repo_search(pool->solvables[p].repo, p, key, match, flags, callback, cbdata);
      return;
    }
  /* FIXME: obey callback return value! */
  for (p = 1; p < pool->nsolvables; p++)
    if (pool->solvables[p].repo)
      repo_search(pool->solvables[p].repo, p, key, match, flags, callback, cbdata);
}

// EOF
