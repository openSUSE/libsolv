/*
 * Copyright (c) 2007-2009, Novell Inc.
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

#define KNOWNID_INITIALIZE
#include "knownid.h"
#undef KNOWNID_INITIALIZE

/* create pool */
Pool *
pool_create(void)
{
  Pool *pool;
  Solvable *s;

  pool = (Pool *)sat_calloc(1, sizeof(*pool));

  stringpool_init (&pool->ss, initpool_data);

  /* alloc space for RelDep 0 */
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
  stringpool_free(&pool->ss);
  sat_free(pool->rels);
  queue_free(&pool->vendormap);
  for (i = 0; i < POOL_TMPSPACEBUF; i++)
    sat_free(pool->tmpspacebuf[i]);
  for (i = 0; i < pool->nlanguages; i++)
    free((char *)pool->languages[i]);
  sat_free(pool->languages);
  sat_free(pool->languagecache);
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


void
pool_set_installed(Pool *pool, Repo *installed)
{
  if (pool->installed == installed)
    return;
  pool->installed = installed;
  pool_freewhatprovides(pool);
}

static int
pool_shrink_whatprovides_sortcmp(const void *ap, const void *bp, void *dp)
{
  int r;
  Pool *pool = dp;
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
  sat_sort(sorted + 1, pool->ss.nstrings - 1, sizeof(Id), pool_shrink_whatprovides_sortcmp, pool);
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
  Repo *installed = pool->installed;
  unsigned int now;

  now = sat_timems(0);
  POOL_DEBUG(SAT_DEBUG_STATS, "number of solvables: %d\n", pool->nsolvables);
  POOL_DEBUG(SAT_DEBUG_STATS, "number of ids: %d + %d\n", pool->ss.nstrings, pool->nrels);

  pool_freeidhashes(pool);	/* XXX: should not be here! */
  pool_freewhatprovides(pool);
  num = pool->ss.nstrings;
  pool->whatprovides = whatprovides = sat_calloc_block(num, sizeof(Offset), WHATPROVIDES_BLOCK);
  pool->whatprovides_rel = sat_calloc_block(pool->nrels, sizeof(Offset), WHATPROVIDES_BLOCK);

  /* count providers for each name */
  for (i = 1; i < pool->nsolvables; i++)
    {
      Id *pp;
      s = pool->solvables + i;
      if (!s->provides)
	continue;
      /* we always need the installed solvable in the whatprovides data,
         otherwise obsoletes/conflicts on them won't work */
      if (s->repo != installed && !pool_installable(pool, s))
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
      if (s->repo != installed && !pool_installable(pool, s))
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
	      if (d[-1] == i)          /* solvable already tacked at end ? */
		continue;              /* Y: skip, on to next provides */
	    }
	  *d = i;		       /* put solvable Id into data */
	}
    }
  pool->whatprovidesdata = whatprovidesdata;
  pool->whatprovidesdataoff = off;
  pool->whatprovidesdataleft = extra;
  pool_shrink_whatprovides(pool);
  POOL_DEBUG(SAT_DEBUG_STATS, "whatprovides memory used: %d K id array, %d K data\n", (pool->ss.nstrings + pool->nrels + WHATPROVIDES_BLOCK) / (int)(1024/sizeof(Id)), (pool->whatprovidesdataoff + pool->whatprovidesdataleft) / (int)(1024/sizeof(Id)));
  POOL_DEBUG(SAT_DEBUG_STATS, "createwhatprovides took %d ms\n", sat_timems(now));
}

/*
 * free all of our whatprovides data
 * be careful, everything internalized with pool_queuetowhatprovides is
 * gone, too
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

  if (count == 0)		       /* queue empty -> 1 */
    return 1;

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

/* check if a package's nevr matches a dependency */

int
pool_match_nevr_rel(Pool *pool, Solvable *s, Id d)
{
  Reldep *rd = GETRELDEP(pool, d);
  Id name = rd->name;
  Id evr = rd->evr;
  int flags = rd->flags;

  if (flags > 7)
    {
      switch (flags)
	{
	case REL_ARCH:
	  if (s->arch != evr)
	    return 0;
	  return pool_match_nevr(pool, s, name);
	case REL_OR:
	  if (pool_match_nevr(pool, s, name))
	    return 1;
	  return pool_match_nevr(pool, s, evr);
	case REL_AND:
	case REL_WITH:
	  if (!pool_match_nevr(pool, s, name))
	    return 0;
	  return pool_match_nevr(pool, s, evr);
	default:
	  return 0;
	}
    }
  if (!pool_match_nevr(pool, s, name))
    return 0;
  if (evr == s->evr)
    return flags & 2 ? 1 : 0;
  if (!flags)
    return 0;
  if (flags == 7)
    return 1;
  if (flags != 2 && flags != 5)
    flags ^= 5;
#ifdef DEBIAN_SEMANTICS
  if ((flags & (1 << (1 + evrcmp(pool, s->evr, evr, EVRCMP_COMPARE)))) != 0)
    return 1;
#else
  if ((flags & (1 << (1 + evrcmp(pool, s->evr, evr, EVRCMP_MATCH_RELEASE)))) != 0)
    return 1;
#endif
  return 0;
}

/* match two dependencies */

int
pool_match_dep(Pool *pool, Id d1, Id d2)
{
  Reldep *rd1, *rd2;
  int pflags, flags;

  if (d1 == d2)
    return 1;
  if (!ISRELDEP(d1))
    {
      if (!ISRELDEP(d2))
	return 0;
      rd2 = GETRELDEP(pool, d2);
      return pool_match_dep(pool, d1, rd2->name);
    }
  rd1 = GETRELDEP(pool, d1);
  if (!ISRELDEP(d2))
    {
      return pool_match_dep(pool, rd1->name, d2);
    }
  rd2 = GETRELDEP(pool, d2);
  if (!pool_match_dep(pool, rd1->name, rd2->name))
    return 0;
  pflags = rd1->flags;
  flags = rd2->flags;
  if (!pflags || !flags || pflags >= 8 || flags >= 8)
    return 0;
  if (flags == 7 || pflags == 7)
    return 1;
  if ((pflags & flags & 5) != 0)
    return 1;
  if (rd1->evr == rd2->evr)
    {
      if ((pflags & flags & 2) != 0)
	return 1;
    }
  else
    {
      int f = flags == 5 ? 5 : flags == 2 ? pflags : (flags ^ 5) & (pflags | 5);
#ifdef DEBIAN_SEMANTICS
      if ((f & (1 << (1 + evrcmp(pool, rd1->evr, rd2->evr, EVRCMP_COMPARE)))) != 0)
	return 1;
#else
      if ((f & (1 << (1 + evrcmp(pool, rd1->evr, rd2->evr, EVRCMP_MATCH_RELEASE)))) != 0)
	return 1;
#endif
    }
  return 0;
}

/*
 * addrelproviders
 * 
 * add packages fulfilling the relation to whatprovides array
 * no exact providers, do range match
 * 
 */

Id
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
  Id p, wp, *pp, *pp2, *pp3;

  d = GETRELID(d);
  queue_init_buffer(&plist, buf, sizeof(buf)/sizeof(*buf));
  switch (flags)
    {
    case REL_AND:
    case REL_WITH:
      pp = pool_whatprovides_ptr(pool, name);
      pp2 = pool_whatprovides_ptr(pool, evr);
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
      pp = pool_whatprovides_ptr(pool, name);
      while ((p = *pp++) != 0)
	queue_push(&plist, p);
      pp = pool_whatprovides_ptr(pool, evr);
      while ((p = *pp++) != 0)
	queue_pushunique(&plist, p);
      break;
    case REL_NAMESPACE:
      if (name == NAMESPACE_OTHERPROVIDERS)
	{
	  wp = pool_whatprovides(pool, evr);
	  pool->whatprovides_rel[d] = wp;
	  return wp;
	}
      if (pool->nscallback)
	{
	  /* ask callback which packages provide the dependency
           * 0:  none
           * 1:  the system (aka SYSTEMSOLVABLE)
           * >1: a set of packages, stored as offset on whatprovidesdata
           */
	  p = pool->nscallback(pool, pool->nscallbackdata, name, evr);
	  if (p > 1)
	    {
	      queue_free(&plist);
	      pool->whatprovides_rel[d] = p;
	      return p;
	    }
	  if (p == 1)
	    queue_push(&plist, SYSTEMSOLVABLE);
	}
      break;
    case REL_ARCH:
      /* small hack: make it possible to match <pkg>.src
       * we have to iterate over the solvables as src packages do not
       * provide anything, thus they are not indexed in our
       * whatprovides hash */
      if (evr == ARCH_SRC)
	{
	  Solvable *s;
	  for (p = 1, s = pool->solvables + p; p < pool->nsolvables; p++, s++)
	    {
	      if (s->arch != ARCH_SRC && s->arch != ARCH_NOSRC)
		continue;
	      if (pool_match_nevr(pool, s, name))
		queue_push(&plist, p);
	    }
	  break;
	}
      wp = pool_whatprovides(pool, name);
      pp = pool->whatprovidesdata + wp;
      while ((p = *pp++) != 0)
	{
	  Solvable *s = pool->solvables + p;
	  if (s->arch == evr)
	    queue_push(&plist, p);
	  else
	    wp = 0;
	}
      if (wp)
	{
	  /* all solvables match, no need to create a new list */
	  pool->whatprovides_rel[d] = wp;
	  return wp;
	}
      break;
    case REL_FILECONFLICT:
      pp = pool_whatprovides_ptr(pool, name);
      while ((p = *pp++) != 0)
	{
	  Id origd = MAKERELDEP(d);
	  Solvable *s = pool->solvables + p;
	  if (!s->provides)
	    continue;
	  pidp = s->repo->idarraydata + s->provides;
	  while ((pid = *pidp++) != 0)
	    if (pid == origd)
	      break;
	  if (pid)
	    queue_push(&plist, p);
	}
      break;
    default:
      break;
    }

  /* convert to whatprovides id */
#if 0
  POOL_DEBUG(SAT_DEBUG_STATS, "addrelproviders: what provides %s?\n", dep2str(pool, name));
#endif
  if (flags && flags < 8)
    {
      pp = pool_whatprovides_ptr(pool, name);
      while (ISRELDEP(name))
	{
          rd = GETRELDEP(pool, name);
	  name = rd->name;
	}
      while ((p = *pp++) != 0)
	{
	  Solvable *s = pool->solvables + p;
#if 0
	  POOL_DEBUG(DEBUG_1, "addrelproviders: checking package %s\n", id2str(pool, s->name));
#endif
	  if (!s->provides)
	    {
	      /* no provides - check nevr */
	      if (pool_match_nevr_rel(pool, s, MAKERELDEP(d)))
	        queue_push(&plist, p);
	      continue;
	    }
	  /* solvable p provides name in some rels */
	  pidp = s->repo->idarraydata + s->provides;
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
#ifdef DEBIAN_SEMANTICS
		  if ((f & (1 << (1 + evrcmp(pool, pevr, evr, EVRCMP_COMPARE)))) != 0)
		    break;
#else
		  if ((f & (1 << (1 + evrcmp(pool, pevr, evr, EVRCMP_MATCH_RELEASE)))) != 0)
		    break;
#endif
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
  POOL_DEBUG(SAT_DEBUG_STATS, "addrelproviders: adding %d packages to %d\n", plist.count, d);
#endif
  pool->whatprovides_rel[d] = pool_queuetowhatprovides(pool, &plist);
  queue_free(&plist);

  return pool->whatprovides_rel[d];
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
      if ((type & (SAT_FATAL|SAT_ERROR)) == 0 || !(pool->debugmask & SAT_DEBUG_TO_STDERR))
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
    mask |= SAT_DEBUG_STATS|SAT_DEBUG_ANALYZE|SAT_DEBUG_UNSOLVABLE|SAT_DEBUG_SOLVER|SAT_DEBUG_TRANSACTION;
  if (level > 1)
    mask |= SAT_DEBUG_JOB|SAT_DEBUG_SOLUTIONS|SAT_DEBUG_POLICY;
  if (level > 2)
    mask |= SAT_DEBUG_PROPAGATE;
  if (level > 3)
    mask |= SAT_DEBUG_RULE_CREATION;
  if (level > 4)
    mask |= SAT_DEBUG_SCHUBI;
  mask |= pool->debugmask & SAT_DEBUG_TO_STDERR;	/* keep bit */
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
	      if (rd->name == NAMESPACE_INSTALLED || rd->name == NAMESPACE_SPLITPROVIDES)
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
      s = id2str(pool, dep);
      if (*s != '/')
	continue;
      csf->ids = sat_extend(csf->ids, csf->nfiles, 1, sizeof(Id), SEARCHFILES_BLOCK);
      csf->dirs = sat_extend(csf->dirs, csf->nfiles, 1, sizeof(const char *), SEARCHFILES_BLOCK);
      csf->names = sat_extend(csf->names, csf->nfiles, 1, sizeof(const char *), SEARCHFILES_BLOCK);
      csf->ids[csf->nfiles] = dep;
      sr = strrchr(s, '/');
      csf->names[csf->nfiles] = strdup(sr + 1);
      csf->dirs[csf->nfiles] = sat_malloc(sr - s + 1);
      if (sr != s)
        strncpy(csf->dirs[csf->nfiles], s, sr - s);
      csf->dirs[csf->nfiles][sr - s] = 0;
      csf->nfiles++;
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

static int
addfileprovides_setid_cb(void *cbdata, Solvable *s, Repodata *data, Repokey *key, KeyValue *kv)
{
  Map *provideids = cbdata;
  if (key->type != REPOKEY_TYPE_IDARRAY)
    return 0;
  MAPSET(provideids, kv->id);
  return kv->eof ? SEARCH_NEXT_SOLVABLE : 0;
}


static void
pool_addfileprovides_search(Pool *pool, struct addfileprovides_cbdata *cbd, struct searchfiles *sf, Repo *repoonly)
{
  Id p, start, end;
  Solvable *s;
  Repodata *data = 0, *nextdata;
  Repo *oldrepo = 0;
  int dataincludes = 0;
  int i, j;
  Map providedids;

  cbd->nfiles = sf->nfiles;
  cbd->ids = sf->ids;
  cbd->dirs = sf->dirs;
  cbd->names = sf->names;
  cbd->olddata = 0;
  cbd->dids = sat_realloc2(cbd->dids, sf->nfiles, sizeof(Id));
  if (repoonly)
    {
      start = repoonly->start;
      end = repoonly->end;
    }
  else
    {
      start = 2;	/* skip system solvable */
      end = pool->nsolvables;
    }
  for (p = start, s = pool->solvables + p; p < end; p++, s++)
    {
      if (!s->repo || (repoonly && s->repo != repoonly))
	continue;
      /* check if p is in (oldrepo,data) */
      if (s->repo != oldrepo || (data && p >= data->end))
	{
	  data = 0;
	  oldrepo = 0;
	}
      if (oldrepo == 0)
	{
	  /* nope, find new repo/repodata */
          /* if we don't find a match, set data to the next repodata */
	  nextdata = 0;
	  for (i = 0, data = s->repo->repodata; i < s->repo->nrepodata; i++, data++)
	    {
	      if (p >= data->end)
		continue;
	      if (data->state != REPODATA_AVAILABLE)
		continue;
	      for (j = 1; j < data->nkeys; j++)
		if (data->keys[j].name == REPOSITORY_ADDEDFILEPROVIDES && data->keys[j].type == REPOKEY_TYPE_IDARRAY)
		  break;
	      if (j == data->nkeys)
		continue;
	      /* great, this repodata contains addedfileprovides */
	      if (!nextdata || nextdata->start > data->start)
		nextdata = data;
	      if (p >= data->start)
		break;
	    }
	  if (i == s->repo->nrepodata)
	    data = nextdata;	/* no direct hit, use next repodata */
	  if (data)
	    {
	      map_init(&providedids, pool->ss.nstrings);
	      repodata_search(data, SOLVID_META, REPOSITORY_ADDEDFILEPROVIDES, 0, addfileprovides_setid_cb, &providedids);
	      for (i = 0; i < cbd->nfiles; i++)
		if (!MAPTST(&providedids, cbd->ids[i]))
		  break;
	      map_free(&providedids);
	      dataincludes = i == cbd->nfiles;
	    }
	  oldrepo = s->repo;
	}
      if (data && p >= data->start && dataincludes)
	continue;
      repo_search(s->repo, p, SOLVABLE_FILELIST, 0, 0, addfileprovides_cb, cbd);
    }
}

void
pool_addfileprovides_ids(Pool *pool, Repo *installed, Id **idp)
{
  Solvable *s;
  Repo *repo;
  struct searchfiles sf, isf, *isfp;
  struct addfileprovides_cbdata cbd;
  int i;

  memset(&sf, 0, sizeof(sf));
  map_init(&sf.seen, pool->ss.nstrings + pool->nrels);
  memset(&isf, 0, sizeof(isf));
  map_init(&isf.seen, pool->ss.nstrings + pool->nrels);

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
  POOL_DEBUG(SAT_DEBUG_STATS, "found %d file dependencies\n", sf.nfiles);
  POOL_DEBUG(SAT_DEBUG_STATS, "found %d installed file dependencies\n", isf.nfiles);
  cbd.dids = 0;
  map_init(&cbd.useddirs, 1);
  if (idp)
    *idp = 0;
  if (sf.nfiles)
    {
#if 0
      for (i = 0; i < sf.nfiles; i++)
	POOL_DEBUG(SAT_DEBUG_STATS, "looking up %s in filelist\n", id2str(pool, sf.ids[i]));
#endif
      pool_addfileprovides_search(pool, &cbd, &sf, 0);
      if (idp)
	{
	  sf.ids = sat_extend(sf.ids, sf.nfiles, 1, sizeof(Id), SEARCHFILES_BLOCK);
	  sf.ids[sf.nfiles] = 0;
	  *idp = sf.ids;
	  sf.ids = 0;
	}
      sat_free(sf.ids);
      for (i = 0; i < sf.nfiles; i++)
	{
	  sat_free(sf.dirs[i]);
	  sat_free(sf.names[i]);
	}
      sat_free(sf.dirs);
      sat_free(sf.names);
    }
  if (isf.nfiles)
    {
#if 0
      for (i = 0; i < isf.nfiles; i++)
	POOL_DEBUG(SAT_DEBUG_STATS, "looking up %s in installed filelist\n", id2str(pool, isf.ids[i]));
#endif
      if (installed)
        pool_addfileprovides_search(pool, &cbd, &isf, installed);
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

void
pool_addfileprovides(Pool *pool)
{
  pool_addfileprovides_ids(pool, pool->installed, 0);
}

void
pool_search(Pool *pool, Id p, Id key, const char *match, int flags, int (*callback)(void *cbdata, Solvable *s, struct _Repodata *data, struct _Repokey *key, struct _KeyValue *kv), void *cbdata)
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

void
pool_clear_pos(Pool *pool)
{
  memset(&pool->pos, 0, sizeof(pool->pos));
}


void
pool_set_languages(Pool *pool, const char **languages, int nlanguages)
{
  int i;

  pool->languagecache = sat_free(pool->languagecache);
  pool->languagecacheother = 0;
  if (pool->nlanguages)
    {
      for (i = 0; i < pool->nlanguages; i++)
	free((char *)pool->languages[i]);
      free(pool->languages);
    }
  pool->nlanguages = nlanguages;
  if (!nlanguages)
    return;
  pool->languages = sat_calloc(nlanguages, sizeof(const char **));
  for (i = 0; i < pool->nlanguages; i++)
    pool->languages[i] = strdup(languages[i]);
}

Id
pool_id2langid(Pool *pool, Id id, const char *lang, int create)
{
  const char *n;
  char buf[256], *p;
  int l;

  if (!lang)
    return id;
  n = id2str(pool, id);
  l = strlen(n) + strlen(lang) + 2;
  if (l > sizeof(buf))
    p = sat_malloc(strlen(n) + strlen(lang) + 2);
  else
    p = buf;
  sprintf(p, "%s:%s", n, lang);
  id = str2id(pool, p, create);
  if (p != buf)
    free(p);
  return id;
}

char *
pool_alloctmpspace(Pool *pool, int len)
{
  int n = pool->tmpspacen;
  if (!len)
    return 0;
  if (len > pool->tmpspacelen[n])
    {
      pool->tmpspacebuf[n] = sat_realloc(pool->tmpspacebuf[n], len + 32);
      pool->tmpspacelen[n] = len + 32;
    }
  pool->tmpspacen = (n + 1) % POOL_TMPSPACEBUF;
  return pool->tmpspacebuf[n];
}

char *
pool_tmpjoin(Pool *pool, const char *str1, const char *str2, const char *str3)
{
  int l1, l2, l3;
  char *s, *str;
  l1 = str1 ? strlen(str1) : 0;
  l2 = str2 ? strlen(str2) : 0;
  l3 = str3 ? strlen(str3) : 0;
  s = str = pool_alloctmpspace(pool, l1 + l2 + l3 + 1);
  if (l1)
    {
      strcpy(s, str1);
      s += l1;
    }
  if (l2)
    {
      strcpy(s, str2);
      s += l2;
    }
  if (l3)
    {
      strcpy(s, str3);
      s += l3;
    }
  *s = 0;
  return str;
}


/*******************************************************************/

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
      cbd->dirmap = sat_free(cbd->dirmap);
      cbd->nmap = 0;
      dirmap = sat_calloc(data->dirpool.ndirs, sizeof(Id));
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
	    compstr = id2str(data->repo->pool, comp);
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
  else
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

void
pool_calc_duchanges(Pool *pool, Map *installedmap, DUChanges *mps, int nmps)
{
  char *p;
  const char *path, *compstr;
  struct mptree *mptree;
  int i, nmptree;
  int pos, compl;
  int mp;
  struct ducbdata cbd;
  Solvable *s;
  Id sp;
  Map ignoredu;
  Repo *oldinstalled = pool->installed;

  memset(&ignoredu, 0, sizeof(ignoredu));
  cbd.mps = mps;
  cbd.addsub = 0;
  cbd.dirmap = 0;
  cbd.nmap = 0;
  cbd.olddata = 0;

  mptree = sat_extend_resize(0, 1, sizeof(struct mptree), MPTREE_BLOCK);

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
	      mptree = sat_extend(mptree, nmptree, 1, sizeof(struct mptree), MPTREE_BLOCK);
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
	  /* no du data available, ignore data of all installed solvables we obsolete */
	  if (!ignoredu.map)
	    map_init(&ignoredu, oldinstalled->end - oldinstalled->start);
	  if (s->obsoletes)
	    {
	      Id obs, *obsp = s->repo->idarraydata + s->obsoletes;
	      while ((obs = *obsp++) != 0)
		FOR_PROVIDES(op, opp, obs)
		  if (op >= oldinstalled->start && op < oldinstalled->end)
		    MAPSET(&ignoredu, op - oldinstalled->start);
	    }
	  FOR_PROVIDES(op, opp, s->name)
	    if (pool->solvables[op].name == s->name)
	      if (op >= oldinstalled->start && op < oldinstalled->end)
		MAPSET(&ignoredu, op - oldinstalled->start);
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
  if (ignoredu.map)
    map_free(&ignoredu);
  sat_free(cbd.dirmap);
  sat_free(mptree);
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
      change += solvable_lookup_num(s, SOLVABLE_INSTALLSIZE, 0);
    }
  if (oldinstalled)
    {
      FOR_REPO_SOLVABLES(oldinstalled, sp, s)
	{
	  if (MAPTST(installedmap, sp))
	    continue;
	  change -= solvable_lookup_num(s, SOLVABLE_INSTALLSIZE, 0);
	}
    }
  return change;
}

/* map:
 *  1: installed
 *  2: conflicts with installed
 *  8: interesting (only true if installed)
 * 16: undecided
 */
 
static inline Id dep2name(Pool *pool, Id dep)
{
  while (ISRELDEP(dep))
    {
      Reldep *rd = rd = GETRELDEP(pool, dep);
      dep = rd->name;
    }
  return dep;
}

static int providedbyinstalled_multiversion(Pool *pool, unsigned char *map, Id n, Id dep) 
{
  Id p, pp;
  Solvable *sn = pool->solvables + n; 

  FOR_PROVIDES(p, pp, sn->name)
    {    
      Solvable *s = pool->solvables + p; 
      if (s->name != sn->name || s->arch != sn->arch)
        continue;
      if ((map[p] & 9) == 9)
        return 1;
    }    
  return 0;
}

static inline int providedbyinstalled(Pool *pool, unsigned char *map, Id dep, int ispatch, Map *noobsoletesmap)
{
  Id p, pp;
  int r = 0;
  FOR_PROVIDES(p, pp, dep)
    {
      if (p == SYSTEMSOLVABLE)
        return 1;	/* always boring, as never constraining */
      if (ispatch && !pool_match_nevr(pool, pool->solvables + p, dep))
	continue;
      if (ispatch && noobsoletesmap && noobsoletesmap->size && MAPTST(noobsoletesmap, p) && ISRELDEP(dep))
	if (providedbyinstalled_multiversion(pool, map, p, dep))
	  continue;
      if ((map[p] & 9) == 9)
	return 9;
      r |= map[p] & 17;
    }
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
pool_trivial_installable_noobsoletesmap(Pool *pool, Map *installedmap, Queue *pkgs, Queue *res, Map *noobsoletesmap)
{
  int i, r, m, did;
  Id p, *dp, con, *conp, req, *reqp;
  unsigned char *map;
  Solvable *s;

  map = sat_calloc(pool->nsolvables, 1);
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
	      r = providedbyinstalled(pool, map, req, 0, 0);
	      if (!r)
		{
		  /* decided and miss */
		  map[p] = 2;
		  break;
		}
	      m |= r;	/* 1 | 9 | 16 | 17 */
	    }
	  if (req)
	    continue;
	  if ((m & 9) == 9)
	    m = 9;
	}
      if (s->conflicts)
	{
	  int ispatch = 0;	/* see solver.c patch handling */

	  if (!strncmp("patch:", id2str(pool, s->name), 6))
	    ispatch = 1;
	  conp = s->repo->idarraydata + s->conflicts;
	  while ((con = *conp++) != 0)
	    {
	      if ((providedbyinstalled(pool, map, con, ispatch, noobsoletesmap) & 1) != 0)
		{
		  map[p] = 2;
		  break;
		}
	      if ((m == 1 || m == 17) && ISRELDEP(con))
		{
		  con = dep2name(pool, con);
		  if ((providedbyinstalled(pool, map, con, ispatch, noobsoletesmap) & 1) != 0)
		    m = 9;
		}
	    }
	  if (con)
	    continue;	/* found a conflict */
	}
#if 0
      if (s->repo && s->repo != oldinstalled)
	{
	  Id p2, obs, *obsp, *pp;
	  Solvable *s2;
	  if (s->obsoletes)
	    {
	      obsp = s->repo->idarraydata + s->obsoletes;
	      while ((obs = *obsp++) != 0)
		{
		  if ((providedbyinstalled(pool, map, obs, 0, 0) & 1) != 0)
		    {
		      map[p] = 2;
		      break;
		    }
		}
	      if (obs)
		continue;
	    }
	  FOR_PROVIDES(p2, pp, s->name)
	    {
	      s2 = pool->solvables + p2;
	      if (s2->name == s->name && (map[p2] & 1) != 0)
		{
		  map[p] = 2;
		  break;
		}
	    }
	  if (p2)
	    continue;
	}
#endif
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
  pool_trivial_installable_noobsoletesmap(pool, installedmap, pkgs, res, 0);
}

const char *
pool_lookup_str(Pool *pool, Id entry, Id keyname)
{
  if (entry == SOLVID_POS && pool->pos.repo)
    return repodata_lookup_str(pool->pos.repo->repodata + pool->pos.repodataid, SOLVID_POS, keyname);
  if (entry <= 0)
    return 0;
  return solvable_lookup_str(pool->solvables + entry, keyname);
}

Id
pool_lookup_id(Pool *pool, Id entry, Id keyname)
{
  if (entry == SOLVID_POS && pool->pos.repo)
    return repodata_lookup_id(pool->pos.repo->repodata + pool->pos.repodataid, SOLVID_POS, keyname);
  if (entry <= 0)
    return 0;
  return solvable_lookup_id(pool->solvables + entry, keyname);
}

unsigned int
pool_lookup_num(Pool *pool, Id entry, Id keyname, unsigned int notfound)
{
  if (entry == SOLVID_POS && pool->pos.repo)
    {
      unsigned int value;
      if (repodata_lookup_num(pool->pos.repo->repodata + pool->pos.repodataid, SOLVID_POS, keyname, &value))
	return value;
      return notfound;
    }
  if (entry <= 0)
    return notfound;
  return solvable_lookup_num(pool->solvables + entry, keyname, notfound);
}

int
pool_lookup_void(Pool *pool, Id entry, Id keyname)
{
  if (entry == SOLVID_POS && pool->pos.repo)
    return repodata_lookup_void(pool->pos.repo->repodata + pool->pos.repodataid, SOLVID_POS, keyname);
  if (entry <= 0)
    return 0;
  return solvable_lookup_void(pool->solvables + entry, keyname);
}

const unsigned char *
pool_lookup_bin_checksum(Pool *pool, Id entry, Id keyname, Id *typep)
{
  if (entry == SOLVID_POS && pool->pos.repo)
    return repodata_lookup_bin_checksum(pool->pos.repo->repodata + pool->pos.repodataid, SOLVID_POS, keyname, typep);
  if (entry <= 0)
    return 0;
  return solvable_lookup_bin_checksum(pool->solvables + entry, keyname, typep);
}

const char *
pool_lookup_checksum(Pool *pool, Id entry, Id keyname, Id *typep)
{
  if (entry == SOLVID_POS && pool->pos.repo)
    {
      const unsigned char *chk = repodata_lookup_bin_checksum(pool->pos.repo->repodata + pool->pos.repodataid, SOLVID_POS, keyname, typep);
      return chk ? repodata_chk2str(pool->pos.repo->repodata + pool->pos.repodataid, *typep, chk) : 0;
    }
  if (entry <= 0)
    return 0;
  return solvable_lookup_checksum(pool->solvables + entry, keyname, typep);
}

void
pool_add_fileconflicts_deps(Pool *pool, Queue *conflicts)
{
  int hadhashes = pool->relhashtbl ? 1 : 0;
  Solvable *s;
  Id fn, p, q, md5;
  Id id;
  int i;

  if (!conflicts->count)
    return;
  pool_freewhatprovides(pool);
  for (i = 0; i < conflicts->count; i += 5)
    {
      fn = conflicts->elements[i];
      p = conflicts->elements[i + 1];
      md5 = conflicts->elements[i + 2];
      q = conflicts->elements[i + 3];
      id = rel2id(pool, fn, md5, REL_FILECONFLICT, 1);
      s = pool->solvables + p;
      if (!s->repo)
	continue;
      s->provides = repo_addid_dep(s->repo, s->provides, id, SOLVABLE_FILEMARKER);
      s = pool->solvables + q;
      if (!s->repo)
	continue;
      s->conflicts = repo_addid_dep(s->repo, s->conflicts, id, 0);
    }
  if (!hadhashes)
    pool_freeidhashes(pool);
}

/* EOF */
