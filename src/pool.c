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
#include "poolvendor.h"
#include "repo.h"
#include "poolid.h"
#include "poolid_private.h"
#include "poolarch.h"
#include "util.h"
#include "bitmap.h"
#include "evr.h"

#define SOLVABLE_BLOCK	255

#undef LIBSOLV_KNOWNID_H
#define KNOWNID_INITIALIZE
#include "knownid.h"
#undef KNOWNID_INITIALIZE

/* create pool */
Pool *
pool_create(void)
{
  Pool *pool;
  Solvable *s;

  pool = (Pool *)solv_calloc(1, sizeof(*pool));

  stringpool_init (&pool->ss, initpool_data);

  /* alloc space for RelDep 0 */
  pool->rels = solv_extend_resize(0, 1, sizeof(Reldep), REL_BLOCK);
  pool->nrels = 1;
  memset(pool->rels, 0, sizeof(Reldep));

  /* alloc space for Solvable 0 and system solvable */
  pool->solvables = solv_extend_resize(0, 2, sizeof(Solvable), SOLVABLE_BLOCK);
  pool->nsolvables = 2;
  memset(pool->solvables, 0, 2 * sizeof(Solvable));

  queue_init(&pool->vendormap);
  queue_init(&pool->pooljobs);
  queue_init(&pool->lazywhatprovidesq);

#if defined(DEBIAN)
  pool->disttype = DISTTYPE_DEB;
  pool->noarchid = ARCH_ALL;
#elif defined(ARCHLINUX)
  pool->disttype = DISTTYPE_ARCH;
  pool->noarchid = ARCH_ANY;
#elif defined(HAIKU)
  pool->disttype = DISTTYPE_HAIKU;
  pool->noarchid = ARCH_ANY;
  pool->obsoleteusesprovides = 1;
#else
  pool->disttype = DISTTYPE_RPM;
  pool->noarchid = ARCH_NOARCH;
#endif

  /* initialize the system solvable */
  s = pool->solvables + SYSTEMSOLVABLE;
  s->name = SYSTEM_SYSTEM;
  s->arch = pool->noarchid;
  s->evr = ID_EMPTY;

  pool->debugmask = SOLV_DEBUG_RESULT;	/* FIXME */
#if defined(FEDORA) || defined(MAGEIA)
  pool->implicitobsoleteusescolors = 1;
#endif
#ifdef RPM5
  pool->noobsoletesmultiversion = 1;
  pool->forbidselfconflicts = 1;
  pool->obsoleteusesprovides = 1;
  pool->implicitobsoleteusesprovides = 1;
  pool->havedistepoch = 1;
#endif
  return pool;
}


/* free all the resources of our pool */
void
pool_free(Pool *pool)
{
  int i;

  pool_freewhatprovides(pool);
  pool_freeidhashes(pool);
  pool_freeallrepos(pool, 1);
  solv_free(pool->id2arch);
  solv_free(pool->id2color);
  solv_free(pool->solvables);
  stringpool_free(&pool->ss);
  solv_free(pool->rels);
  pool_setvendorclasses(pool, 0);
  queue_free(&pool->vendormap);
  queue_free(&pool->pooljobs);
  queue_free(&pool->lazywhatprovidesq);
  for (i = 0; i < POOL_TMPSPACEBUF; i++)
    solv_free(pool->tmpspace.buf[i]);
  for (i = 0; i < pool->nlanguages; i++)
    free((char *)pool->languages[i]);
  solv_free((void *)pool->languages);
  solv_free(pool->languagecache);
  solv_free(pool->errstr);
  solv_free(pool->rootdir);
  solv_free(pool);
}

void
pool_freeallrepos(Pool *pool, int reuseids)
{
  int i;

  pool_freewhatprovides(pool);
  for (i = 1; i < pool->nrepos; i++)
    if (pool->repos[i])
      repo_freedata(pool->repos[i]);
  pool->repos = solv_free(pool->repos);
  pool->nrepos = 0;
  pool->urepos = 0;
  /* the first two solvables don't belong to a repo */
  pool_free_solvable_block(pool, 2, pool->nsolvables - 2, reuseids);
}

int
pool_setdisttype(Pool *pool, int disttype)
{
#ifdef MULTI_SEMANTICS
  int olddisttype = pool->disttype;
  switch(disttype)
    {
    case DISTTYPE_RPM:
      pool->noarchid = ARCH_NOARCH;
      break;
    case DISTTYPE_DEB:
      pool->noarchid = ARCH_ALL;
      break;
    case DISTTYPE_ARCH:
    case DISTTYPE_HAIKU:
      pool->noarchid = ARCH_ANY;
      break;
    default:
      return -1;
    }
  pool->disttype = disttype;
  pool->solvables[SYSTEMSOLVABLE].arch = pool->noarchid;
  return olddisttype;
#else
  return pool->disttype == disttype ? disttype : -1;
#endif
}

int
pool_get_flag(Pool *pool, int flag)
{
  switch (flag)
    {
    case POOL_FLAG_PROMOTEEPOCH:
      return pool->promoteepoch;
    case POOL_FLAG_FORBIDSELFCONFLICTS:
      return pool->forbidselfconflicts;
    case POOL_FLAG_OBSOLETEUSESPROVIDES:
      return pool->obsoleteusesprovides;
    case POOL_FLAG_IMPLICITOBSOLETEUSESPROVIDES:
      return pool->implicitobsoleteusesprovides;
    case POOL_FLAG_OBSOLETEUSESCOLORS:
      return pool->obsoleteusescolors;
    case POOL_FLAG_IMPLICITOBSOLETEUSESCOLORS:
      return pool->implicitobsoleteusescolors;
    case POOL_FLAG_NOINSTALLEDOBSOLETES:
      return pool->noinstalledobsoletes;
    case POOL_FLAG_HAVEDISTEPOCH:
      return pool->havedistepoch;
    case POOL_FLAG_NOOBSOLETESMULTIVERSION:
      return pool->noobsoletesmultiversion;
    case POOL_FLAG_ADDFILEPROVIDESFILTERED:
      return pool->addfileprovidesfiltered;
    case POOL_FLAG_NOWHATPROVIDESAUX:
      return pool->nowhatprovidesaux;
    default:
      break;
    }
  return -1;
}

int
pool_set_flag(Pool *pool, int flag, int value)
{
  int old = pool_get_flag(pool, flag);
  switch (flag)
    {
    case POOL_FLAG_PROMOTEEPOCH:
      pool->promoteepoch = value;
      break;
    case POOL_FLAG_FORBIDSELFCONFLICTS:
      pool->forbidselfconflicts = value;
      break;
    case POOL_FLAG_OBSOLETEUSESPROVIDES:
      pool->obsoleteusesprovides = value;
      break;
    case POOL_FLAG_IMPLICITOBSOLETEUSESPROVIDES:
      pool->implicitobsoleteusesprovides = value;
      break;
    case POOL_FLAG_OBSOLETEUSESCOLORS:
      pool->obsoleteusescolors = value;
      break;
    case POOL_FLAG_IMPLICITOBSOLETEUSESCOLORS:
      pool->implicitobsoleteusescolors = value;
      break;
    case POOL_FLAG_NOINSTALLEDOBSOLETES:
      pool->noinstalledobsoletes = value;
      break;
    case POOL_FLAG_HAVEDISTEPOCH:
      pool->havedistepoch = value;
      break;
    case POOL_FLAG_NOOBSOLETESMULTIVERSION:
      pool->noobsoletesmultiversion = value;
      break;
    case POOL_FLAG_ADDFILEPROVIDESFILTERED:
      pool->addfileprovidesfiltered = value;
      break;
    case POOL_FLAG_NOWHATPROVIDESAUX:
      pool->nowhatprovidesaux = value;
      break;
    default:
      break;
    }
  return old;
}


Id
pool_add_solvable(Pool *pool)
{
  pool->solvables = solv_extend(pool->solvables, pool->nsolvables, 1, sizeof(Solvable), SOLVABLE_BLOCK);
  memset(pool->solvables + pool->nsolvables, 0, sizeof(Solvable));
  return pool->nsolvables++;
}

Id
pool_add_solvable_block(Pool *pool, int count)
{
  Id nsolvables = pool->nsolvables;
  if (!count)
    return nsolvables;
  pool->solvables = solv_extend(pool->solvables, pool->nsolvables, count, sizeof(Solvable), SOLVABLE_BLOCK);
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
  Id i, n, id;
  Id *sorted;
  Id lastid, *last, *dp, *lp;
  Offset o;
  int r;

  if (pool->ss.nstrings < 3)
    return;
  sorted = solv_malloc2(pool->ss.nstrings, sizeof(Id));
  for (i = id = 0; id < pool->ss.nstrings; id++)
    if (pool->whatprovides[id] >= 4)
      sorted[i++] = id;
  n = i;
  solv_sort(sorted, n, sizeof(Id), pool_shrink_whatprovides_sortcmp, pool);
  last = 0;
  lastid = 0;
  for (i = 0; i < n; i++)
    {
      id = sorted[i];
      o = pool->whatprovides[id];
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
  solv_free(sorted);
  dp = pool->whatprovidesdata + 4;
  for (id = 1; id < pool->ss.nstrings; id++)
    {
      o = pool->whatprovides[id];
      if (!o)
	continue;
      if ((Id)o < 0)
	{
	  i = -(Id)o;
	  if (i >= id)
	    abort();
	  pool->whatprovides[id] = pool->whatprovides[i];
	  continue;
	}
      if (o < 4)
	continue;
      lp = pool->whatprovidesdata + o;
      if (lp < dp)
	abort();
      pool->whatprovides[id] = dp - pool->whatprovidesdata;
      while ((*dp++ = *lp++) != 0)
	;
    }
  o = dp - pool->whatprovidesdata;
  POOL_DEBUG(SOLV_DEBUG_STATS, "shrunk whatprovidesdata from %d to %d\n", pool->whatprovidesdataoff, o);
  if (pool->whatprovidesdataoff == o)
    return;
  r = pool->whatprovidesdataoff - o;
  pool->whatprovidesdataoff = o;
  pool->whatprovidesdata = solv_realloc(pool->whatprovidesdata, (o + pool->whatprovidesdataleft) * sizeof(Id));
  if (r > pool->whatprovidesdataleft)
    r = pool->whatprovidesdataleft;
  memset(pool->whatprovidesdata + o, 0, r * sizeof(Id));
}

/* this gets rid of all the zeros in the aux */
static void
pool_shrink_whatprovidesaux(Pool *pool)
{
  int num = pool->whatprovidesauxoff;
  Id id;
  Offset newoff;
  Id *op, *wp = pool->whatprovidesauxdata + 1;
  int i;

  for (i = 0; i < num; i++)
    {
      Offset o = pool->whatprovidesaux[i];
      if (o < 2)
	continue;
      op = pool->whatprovidesauxdata + o;
      pool->whatprovidesaux[i] = wp - pool->whatprovidesauxdata;
      if (op < wp)
	abort();
      while ((id = *op++) != 0)
	*wp++ = id;
    }
  newoff = wp - pool->whatprovidesauxdata;
  pool->whatprovidesauxdata = solv_realloc(pool->whatprovidesauxdata, newoff * sizeof(Id));
  POOL_DEBUG(SOLV_DEBUG_STATS, "shrunk whatprovidesauxdata from %d to %d\n", pool->whatprovidesauxdataoff, newoff);
  pool->whatprovidesauxdataoff = newoff;
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
  Id *whatprovidesdata, *dp, *whatprovidesauxdata;
  Offset *whatprovidesaux;
  Repo *installed = pool->installed;
  unsigned int now;

  now = solv_timems(0);
  POOL_DEBUG(SOLV_DEBUG_STATS, "number of solvables: %d, memory used: %d K\n", pool->nsolvables, pool->nsolvables * (int)sizeof(Solvable) / 1024);
  POOL_DEBUG(SOLV_DEBUG_STATS, "number of ids: %d + %d\n", pool->ss.nstrings, pool->nrels);
  POOL_DEBUG(SOLV_DEBUG_STATS, "string memory used: %d K array + %d K data,  rel memory used: %d K array\n", pool->ss.nstrings / (1024 / (int)sizeof(Id)), pool->ss.sstrings / 1024, pool->nrels * (int)sizeof(Reldep) / 1024);
  if (pool->ss.stringhashmask || pool->relhashmask)
    POOL_DEBUG(SOLV_DEBUG_STATS, "string hash memory: %d K, rel hash memory : %d K\n", (pool->ss.stringhashmask + 1) / (int)(1024/sizeof(Id)), (pool->relhashmask + 1) / (int)(1024/sizeof(Id)));

  pool_freeidhashes(pool);	/* XXX: should not be here! */
  pool_freewhatprovides(pool);
  num = pool->ss.nstrings;
  pool->whatprovides = whatprovides = solv_calloc_block(num, sizeof(Offset), WHATPROVIDES_BLOCK);
  pool->whatprovides_rel = solv_calloc_block(pool->nrels, sizeof(Offset), WHATPROVIDES_BLOCK);

  /* count providers for each name */
  for (i = pool->nsolvables - 1; i > 0; i--)
    {
      Id *pp;
      s = pool->solvables + i;
      if (!s->provides || !s->repo || s->repo->disabled)
	continue;
      /* we always need the installed solvable in the whatprovides data,
         otherwise obsoletes/conflicts on them won't work */
      if (s->repo != installed && !pool_installable(pool, s))
	continue;
      pp = s->repo->idarraydata + s->provides;
      while ((id = *pp++) != 0)
	{
	  while (ISRELDEP(id))
	    {
	      Reldep *rd = GETRELDEP(pool, id);
	      id = rd->name;
	    }
	  whatprovides[id]++;	       /* inc count of providers */
	}
    }

  off = 4;	/* first entry is undef, second is empty list, third is system solvable  */
  np = 0;			       /* number of names provided */
  for (i = 0, idp = whatprovides; i < num; i++, idp++)
    {
      n = *idp;
      if (!n)				/* no providers */
	{
	  *idp = 1;			/* offset for empty list */
	  continue;
	}
      off += n;				/* make space for all providers */
      *idp = off++;			/* now idp points to terminating zero */
      np++;				/* inc # of provider 'slots' for stats */
    }

  POOL_DEBUG(SOLV_DEBUG_STATS, "provide ids: %d\n", np);

  /* reserve some space for relation data */
  extra = 2 * pool->nrels;
  if (extra < 256)
    extra = 256;

  POOL_DEBUG(SOLV_DEBUG_STATS, "provide space needed: %d + %d\n", off, extra);

  /* alloc space for all providers + extra */
  whatprovidesdata = solv_calloc(off + extra, sizeof(Id));
  whatprovidesdata[2] = SYSTEMSOLVABLE;

  /* alloc aux vector */
  whatprovidesauxdata = 0;
  if (!pool->nowhatprovidesaux)
    {
      pool->whatprovidesaux = whatprovidesaux = solv_calloc(num, sizeof(Offset));
      pool->whatprovidesauxoff = num;
      pool->whatprovidesauxdataoff = off;
      pool->whatprovidesauxdata = whatprovidesauxdata = solv_calloc(pool->whatprovidesauxdataoff, sizeof(Id));
    }

  /* now fill data for all provides */
  for (i = pool->nsolvables - 1; i > 0; i--)
    {
      Id *pp;
      s = pool->solvables + i;
      if (!s->provides || !s->repo || s->repo->disabled)
	continue;
      if (s->repo != installed && !pool_installable(pool, s))
	continue;

      /* for all provides of this solvable */
      pp = s->repo->idarraydata + s->provides;
      while ((id = *pp++) != 0)
	{
	  Id auxid = id;
	  while (ISRELDEP(id))
	    {
	      Reldep *rd = GETRELDEP(pool, id);
	      id = rd->name;
	    }
	  dp = whatprovidesdata + whatprovides[id];   /* offset into whatprovidesdata */
	  if (*dp != i)		/* don't add same solvable twice */
	    {
	      dp[-1] = i;
	      whatprovides[id]--;
	    }
	  else
	    auxid = 1;
	  if (whatprovidesauxdata)
	    whatprovidesauxdata[whatprovides[id]] = auxid;
	}
    }
  if (pool->whatprovidesaux)
    memcpy(pool->whatprovidesaux, pool->whatprovides, num * sizeof(Id));
  pool->whatprovidesdata = whatprovidesdata;
  pool->whatprovidesdataoff = off;
  pool->whatprovidesdataleft = extra;
  pool_shrink_whatprovides(pool);
  if (pool->whatprovidesaux)
    pool_shrink_whatprovidesaux(pool);
  POOL_DEBUG(SOLV_DEBUG_STATS, "whatprovides memory used: %d K id array, %d K data\n", (pool->ss.nstrings + pool->nrels + WHATPROVIDES_BLOCK) / (int)(1024/sizeof(Id)), (pool->whatprovidesdataoff + pool->whatprovidesdataleft) / (int)(1024/sizeof(Id)));
  if (pool->whatprovidesaux)
    POOL_DEBUG(SOLV_DEBUG_STATS, "whatprovidesaux memory used: %d K id array, %d K data\n", pool->whatprovidesauxoff / (int)(1024/sizeof(Id)), pool->whatprovidesauxdataoff / (int)(1024/sizeof(Id)));

  queue_empty(&pool->lazywhatprovidesq);
  if ((!pool->addedfileprovides && pool->disttype == DISTTYPE_RPM) || pool->addedfileprovides == 1)
    {
      if (!pool->addedfileprovides)
	POOL_DEBUG(SOLV_DEBUG_STATS, "WARNING: pool_addfileprovides was not called, this may result in slow operation\n");
      /* lazyly add file provides */
      for (i = 1; i < num; i++)
	{
	  const char *str = pool->ss.stringspace + pool->ss.strings[i];
	  if (str[0] != '/')
	    continue;
	  if (pool->addedfileprovides == 1 && repodata_filelistfilter_matches(0, str))
	    continue;
	  /* setup lazy adding, but remember old value */
	  if (pool->whatprovides[i] > 1)
	    queue_push2(&pool->lazywhatprovidesq, i, pool->whatprovides[i]);
	  pool->whatprovides[i] = 0;
	  if (pool->whatprovidesaux)
	    pool->whatprovidesaux[i] = 0;	/* sorry */
	}
      POOL_DEBUG(SOLV_DEBUG_STATS, "lazywhatprovidesq size: %d entries\n", pool->lazywhatprovidesq.count / 2);
    }

  POOL_DEBUG(SOLV_DEBUG_STATS, "createwhatprovides took %d ms\n", solv_timems(now));
}

/*
 * free all of our whatprovides data
 * be careful, everything internalized with pool_queuetowhatprovides is
 * gone, too
 */
void
pool_freewhatprovides(Pool *pool)
{
  pool->whatprovides = solv_free(pool->whatprovides);
  pool->whatprovides_rel = solv_free(pool->whatprovides_rel);
  pool->whatprovidesdata = solv_free(pool->whatprovidesdata);
  pool->whatprovidesdataoff = 0;
  pool->whatprovidesdataleft = 0;
  pool->whatprovidesaux = solv_free(pool->whatprovidesaux);
  pool->whatprovidesauxdata = solv_free(pool->whatprovidesauxdata);
  pool->whatprovidesauxoff = 0;
  pool->whatprovidesauxdataoff = 0;
}


/******************************************************************************/

/*
 * pool_queuetowhatprovides  - add queue contents to whatprovidesdata
 *
 * used for whatprovides, jobs, learnt rules, selections
 * input: q: queue of Ids
 * returns: Offset into whatprovidesdata
 *
 */

Id
pool_ids2whatprovides(Pool *pool, Id *ids, int count)
{
  Offset off;

  if (count == 0)		       /* queue empty -> 1 */
    return 1;
  if (count == 1 && *ids == SYSTEMSOLVABLE)
    return 2;

  /* extend whatprovidesdata if needed, +1 for 0-termination */
  if (pool->whatprovidesdataleft < count + 1)
    {
      POOL_DEBUG(SOLV_DEBUG_STATS, "growing provides hash data...\n");
      pool->whatprovidesdata = solv_realloc(pool->whatprovidesdata, (pool->whatprovidesdataoff + count + 4096) * sizeof(Id));
      pool->whatprovidesdataleft = count + 4096;
    }

  /* copy queue to next free slot */
  off = pool->whatprovidesdataoff;
  memcpy(pool->whatprovidesdata + pool->whatprovidesdataoff, ids, count * sizeof(Id));

  /* adapt count and 0-terminate */
  pool->whatprovidesdataoff += count;
  pool->whatprovidesdata[pool->whatprovidesdataoff++] = 0;
  pool->whatprovidesdataleft -= count + 1;

  return (Id)off;
}

Id
pool_queuetowhatprovides(Pool *pool, Queue *q)
{
  int count = q->count;
  if (count == 0)		       /* queue empty -> 1 */
    return 1;
  if (count == 1 && q->elements[0] == SYSTEMSOLVABLE)
    return 2;
  return pool_ids2whatprovides(pool, q->elements, count);
}


/*************************************************************************/

#if defined(MULTI_SEMANTICS)
# define EVRCMP_DEPCMP (pool->disttype == DISTTYPE_DEB ? EVRCMP_COMPARE : EVRCMP_MATCH_RELEASE)
#elif defined(DEBIAN)
# define EVRCMP_DEPCMP EVRCMP_COMPARE
#else
# define EVRCMP_DEPCMP EVRCMP_MATCH_RELEASE
#endif

/* check if a package's nevr matches a dependency */
/* semi-private, called from public pool_match_nevr */

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
	    {
	      if (evr != ARCH_SRC || s->arch != ARCH_NOSRC)
	        return 0;
	    }
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
	case REL_WITHOUT:
	  if (!pool_match_nevr(pool, s, name))
	    return 0;
	  return !pool_match_nevr(pool, s, evr);
	case REL_MULTIARCH:
	  if (evr != ARCH_ANY)
	    return 0;
	  /* XXX : need to check for Multi-Arch: allowed! */
	  return pool_match_nevr(pool, s, name);
	default:
	  return 0;
	}
    }
  if (!pool_match_nevr(pool, s, name))
    return 0;
  if (evr == s->evr)
    return (flags & REL_EQ) ? 1 : 0;
  if (!flags)
    return 0;
  if (flags == 7)
    return 1;
  switch (pool_evrcmp(pool, s->evr, evr, EVRCMP_DEPCMP))
    {
    case -2:
      return 1;
    case -1:
      return (flags & REL_LT) ? 1 : 0;
    case 0:
      return (flags & REL_EQ) ? 1 : 0;
    case 1:
      return (flags & REL_GT) ? 1 : 0;
    case 2:
      return (flags & REL_EQ) ? 1 : 0;
    default:
      break;
    }
  return 0;
}

#if defined(HAIKU) || defined(MULTI_SEMANTICS)
/* forward declaration */
static int pool_match_flags_evr_rel_compat(Pool *pool, Reldep *range, int flags, int evr);
#endif

/* match (flags, evr) against provider (pflags, pevr) */
static inline int
pool_match_flags_evr(Pool *pool, int pflags, Id pevr, int flags, int evr)
{
  if (!pflags || !flags || pflags >= 8 || flags >= 8)
    return 0;
  if (flags == 7 || pflags == 7)
    return 1;		/* rel provides every version */
  if ((pflags & flags & (REL_LT | REL_GT)) != 0)
    return 1;		/* both rels show in the same direction */
  if (pevr == evr)
    return (flags & pflags & REL_EQ) ? 1 : 0;
#if defined(HAIKU) || defined(MULTI_SEMANTICS)
  if (ISRELDEP(pevr))
    {
      Reldep *rd = GETRELDEP(pool, pevr);
      if (rd->flags == REL_COMPAT)
	return pool_match_flags_evr_rel_compat(pool, rd, flags, evr);
    }
#endif
  switch (pool_evrcmp(pool, pevr, evr, EVRCMP_DEPCMP))
    {
    case -2:
      return (pflags & REL_EQ) ? 1 : 0;
    case -1:
      return (flags & REL_LT) || (pflags & REL_GT) ? 1 : 0;
    case 0:
      return (flags & pflags & REL_EQ) ? 1 : 0;
    case 1:
      return (flags & REL_GT) || (pflags & REL_LT) ? 1 : 0;
    case 2:
      return (flags & REL_EQ) ? 1 : 0;
    default:
      break;
    }
  return 0;
}

#if defined(HAIKU) || defined(MULTI_SEMANTICS)
static int
pool_match_flags_evr_rel_compat(Pool *pool, Reldep *range, int flags, int evr)
{
  /* range->name is the actual version, range->evr the backwards compatibility
     version. If flags are '>=' or '>', we match the compatibility version
     as well, otherwise only the actual version. */
  if (!(flags & REL_GT) || (flags & REL_LT))
    return pool_match_flags_evr(pool, REL_EQ, range->name, flags, evr);
  return pool_match_flags_evr(pool, REL_LT | REL_EQ, range->name, flags, evr) &&
         pool_match_flags_evr(pool, REL_GT | REL_EQ, range->evr, REL_EQ, evr);
}
#endif

/* public (i.e. not inlined) version of pool_match_flags_evr */
int
pool_intersect_evrs(Pool *pool, int pflags, Id pevr, int flags, Id evr)
{
  return pool_match_flags_evr(pool, pflags, pevr, flags, evr);
}

/* match two dependencies (d1 = provider) */

int
pool_match_dep(Pool *pool, Id d1, Id d2)
{
  Reldep *rd1, *rd2;

  if (d1 == d2)
    return 1;

  if (ISRELDEP(d1))
    {
      /* we use potentially matches for complex deps */
      rd1 = GETRELDEP(pool, d1);
      if (rd1->flags == REL_AND || rd1->flags == REL_OR || rd1->flags == REL_WITH || rd1->flags == REL_WITHOUT || rd1->flags == REL_COND || rd1->flags == REL_UNLESS)
	{
	  if (pool_match_dep(pool, rd1->name, d2))
	    return 1;
	  if ((rd1->flags == REL_COND || rd1->flags == REL_UNLESS) && ISRELDEP(rd1->evr))
	    {
	      rd1 = GETRELDEP(pool, rd1->evr);
	      if (rd1->flags != REL_ELSE)
		return 0;
	    }
	  if (rd1->flags != REL_COND && rd1->flags != REL_UNLESS && rd1->flags != REL_WITHOUT && pool_match_dep(pool, rd1->evr, d2))
	    return 1;
	  return 0;
	}
    }
  if (ISRELDEP(d2))
    {
      /* we use potentially matches for complex deps */
      rd2 = GETRELDEP(pool, d2);
      if (rd2->flags == REL_AND || rd2->flags == REL_OR || rd2->flags == REL_WITH || rd2->flags == REL_WITHOUT || rd2->flags == REL_COND || rd2->flags == REL_UNLESS)
	{
	  if (pool_match_dep(pool, d1, rd2->name))
	    return 1;
	  if ((rd2->flags == REL_COND || rd2->flags == REL_UNLESS) && ISRELDEP(rd2->evr))
	    {
	      rd2 = GETRELDEP(pool, rd2->evr);
	      if (rd2->flags != REL_ELSE)
		return 0;
	    }
	  if (rd2->flags != REL_COND && rd2->flags != REL_UNLESS && rd2->flags != REL_WITHOUT && pool_match_dep(pool, d1, rd2->evr))
	    return 1;
	  return 0;
	}
    }
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
  /* first match name */
  if (!pool_match_dep(pool, rd1->name, rd2->name))
    return 0;
  /* name matches, check flags and evr */
  return pool_intersect_evrs(pool, rd1->flags, rd1->evr, rd2->flags, rd2->evr);
}

Id
pool_searchlazywhatprovidesq(Pool *pool, Id d)
{
  int start = 0;
  int end = pool->lazywhatprovidesq.count;
  Id *elements;
  if (!end)
    return 0;
  elements = pool->lazywhatprovidesq.elements;
  while (end - start > 16)
    {
      int mid = (start + end) / 2 & ~1;
      if (elements[mid] == d)
	return elements[mid + 1];
      if (elements[mid] < d)
	start = mid + 2;
      else
	end = mid;
    }
  for (; start < end; start += 2)
    if (elements[start] == d)
      return elements[start + 1];
  return 0;
}

/*
 * addstdproviders
 *
 * lazy populating of the whatprovides array, non relation case
 */
static Id
pool_addstdproviders(Pool *pool, Id d)
{
  const char *str;
  Queue q;
  Id qbuf[16];
  Dataiterator di;
  Id oldoffset;

  if (pool->addedfileprovides == 2)
    {
      pool->whatprovides[d] = 1;
      return 1;
    }
  str =  pool->ss.stringspace + pool->ss.strings[d];
  if (*str != '/')
    {
      pool->whatprovides[d] = 1;
      return 1;
    }
  queue_init_buffer(&q, qbuf, sizeof(qbuf)/sizeof(*qbuf));
  dataiterator_init(&di, pool, 0, 0, SOLVABLE_FILELIST, str, SEARCH_STRING|SEARCH_FILES|SEARCH_COMPLETE_FILELIST);
  for (; dataiterator_step(&di); dataiterator_skip_solvable(&di))
    {
      Solvable *s = pool->solvables + di.solvid;
      /* XXX: maybe should add a provides dependency to the solvables
       * OTOH this is only needed for rel deps that filter the provides,
       * and those should not use filelist entries */
      if (s->repo->disabled)
	continue;
      if (s->repo != pool->installed && !pool_installable(pool, s))
	continue;
      queue_push(&q, di.solvid);
    }
  dataiterator_free(&di);
  oldoffset = pool_searchlazywhatprovidesq(pool, d);
  if (!q.count)
    pool->whatprovides[d] = oldoffset ? oldoffset : 1;
  else
    {
      if (oldoffset)
	{
	  Id *oo = pool->whatprovidesdata + oldoffset;
	  int i;
	  /* unify both queues. easy, as we know both are sorted */
	  for (i = 0; i < q.count; i++)
	    {
	      if (*oo > q.elements[i])
		continue;
	      if (*oo < q.elements[i])
		queue_insert(&q, i, *oo);
	      oo++;
	      if (!*oo)
		break;
	    }
	  while (*oo)
	    queue_push(&q, *oo++);
	  if (q.count == oo - (pool->whatprovidesdata + oldoffset))
	    {
	      /* end result has same size as oldoffset -> no new entries */
	      queue_free(&q);
	      pool->whatprovides[d] = oldoffset;
	      return oldoffset;
	    }
	}
      pool->whatprovides[d] = pool_queuetowhatprovides(pool, &q);
    }
  queue_free(&q);
  return pool->whatprovides[d];
}


static inline int
pool_is_kind(Pool *pool, Id name, Id kind)
{
  const char *n;
  if (!kind)
    return 1;
  n = pool_id2str(pool, name);
  if (kind != 1)
    {
      const char *kn = pool_id2str(pool, kind);
      int knl = strlen(kn);
      return !strncmp(n, kn, knl) && n[knl] == ':' ? 1 : 0;
    }
  else
    {
      if (*n == ':')
	return 1;
      while(*n >= 'a' && *n <= 'z')
	n++;
      return *n == ':' ? 0 : 1;
    }
}

/*
 * rpm eq magic:
 *
 * some dependencies are of the from "foo = md5sum", like the
 * buildid provides. There's not much we can do to speed up
 * getting the providers, because rpm's complex evr comparison
 * rules get in the way. But we can use the first character(s)
 * of the md5sum to do a simple pre-check.
 */
static inline int
rpmeqmagic(Pool *pool, Id evr)
{
  const char *s = pool_id2str(pool, evr);
  if (*s == '0')
    {
      while (*s == '0' && s[1] >= '0' && s[1] <= '9')
	s++;
      /* ignore epoch 0 */
      if (*s == '0' && s[1] == ':')
	{
	  s += 2;
	  while (*s == '0' && s[1] >= '0' && s[1] <= '9')
	    s++;
	}
    }
  if (*s >= '0' && *s <= '9')
    {
      if (s[1] >= '0' && s[1] <= '9')
        return *s + (s[1] << 8);
      return *s;
    }
  if ((*s >= 'a' && *s <= 'z') || (*s >= 'A' && *s <= 'Z'))
    {
      if ((s[1] >= 'a' && s[1] <= 'z') || (s[1] >= 'A' && s[1] <= 'Z'))
        return *s + (s[1] << 8);
      return *s;
    }
  return -1;
}

static inline int
rpmeqmagic_init(Pool *pool, int flags, Id evr)
{
  if (flags != REL_EQ || pool->disttype != DISTTYPE_RPM || pool->promoteepoch || ISRELDEP(evr))
    return -1;
  else
    return rpmeqmagic(pool, evr);
}

static inline int
rpmeqmagic_cantmatch(Pool *pool, int flags, Id evr, int eqmagic)
{
  int peqmagic = rpmeqmagic(pool, evr);
  if (peqmagic > 0 && peqmagic != eqmagic)
    return 1;
  return 0;
}

/*
 * addrelproviders
 *
 * add packages fulfilling the relation to whatprovides array
 *
 * some words about REL_AND and REL_IF: we assume the best case
 * here, so that you get a "potential" result if you ask for a match.
 * E.g. if you ask for "whatrequires A" and package X contains
 * "Requires: A & B", you'll get "X" as an answer.
 */
Id
pool_addrelproviders(Pool *pool, Id d)
{
  Reldep *rd;
  Reldep *prd;
  Queue plist;
  Id buf[16];
  Id name, evr, flags;
  Id pid, *pidp;
  Id p, *pp;

  if (!ISRELDEP(d))
    return pool_addstdproviders(pool, d);
  rd = GETRELDEP(pool, d);
  name = rd->name;
  evr = rd->evr;
  flags = rd->flags;
  d = GETRELID(d);
  queue_init_buffer(&plist, buf, sizeof(buf)/sizeof(*buf));

  if (flags >= 8)
    {
      /* special relation */
      Id wp = 0;
      Id *pp2, *pp3;

      switch (flags)
	{
	case REL_WITH:
	  wp = pool_whatprovides(pool, name);
	  pp2 = pool_whatprovides_ptr(pool, evr);
	  pp = pool->whatprovidesdata + wp;
	  while ((p = *pp++) != 0)
	    {
	      for (pp3 = pp2; *pp3; pp3++)
		if (*pp3 == p)
		  break;
	      if (*pp3)
		queue_push(&plist, p);	/* found it */
	      else
		wp = 0;
	    }
	  break;
	case REL_WITHOUT:
	  wp = pool_whatprovides(pool, name);
	  pp2 = pool_whatprovides_ptr(pool, evr);
	  pp = pool->whatprovidesdata + wp;
	  while ((p = *pp++) != 0)
	    {
	      for (pp3 = pp2; *pp3; pp3++)
		if (*pp3 == p)
		  break;
	      if (!*pp3)
		queue_push(&plist, p);	/* use it */
	      else
		wp = 0;
	    }
	  break;
	case REL_AND:
	case REL_OR:
	case REL_COND:
	case REL_UNLESS:
	  if (flags == REL_COND || flags == REL_UNLESS)
	    {
	      if (ISRELDEP(evr))
		{
		  Reldep *rd2 = GETRELDEP(pool, evr);
		  evr = rd2->flags == REL_ELSE ? rd2->evr : 0;
		}
	      else
	        evr = 0;	/* assume cond is true */
	    }
	  wp = pool_whatprovides(pool, name);
	  if (!pool->whatprovidesdata[wp])
	    wp = evr ? pool_whatprovides(pool, evr) : 1;
	  else if (evr)
	    {
	      /* sorted merge */
	      pp2 = pool_whatprovides_ptr(pool, evr);
	      pp = pool->whatprovidesdata + wp;
	      while (*pp && *pp2)
		{
		  if (*pp < *pp2)
		    queue_push(&plist, *pp++);
		  else
		    {
		      if (*pp == *pp2)
			pp++;
		      queue_push(&plist, *pp2++);
		    }
		}
	      while (*pp)
	        queue_push(&plist, *pp++);
	      while (*pp2)
	        queue_push(&plist, *pp2++);
	      /* if the number of elements did not change, we can reuse wp */
	      if (pp - (pool->whatprovidesdata + wp) != plist.count)
		wp = 0;
	    }
	  break;

	case REL_NAMESPACE:
	  if (name == NAMESPACE_OTHERPROVIDERS)
	    {
	      wp = pool_whatprovides(pool, evr);
	      break;
	    }
	  if (pool->nscallback)
	    {
	      /* ask callback which packages provide the dependency
	       * 0:  none
	       * 1:  the system (aka SYSTEMSOLVABLE)
	       * >1: set of packages, stored as offset on whatprovidesdata
	       */
	      p = pool->nscallback(pool, pool->nscallbackdata, name, evr);
	      if (p > 1)
		wp = p;
	      if (p == 1)
		queue_push(&plist, SYSTEMSOLVABLE);
	    }
	  break;
	case REL_ARCH:
	  /* small hack: make it possible to match <pkg>.src
	   * we have to iterate over the solvables as src packages do not
	   * provide anything, thus they are not indexed in our
	   * whatprovides hash */
	  if (evr == ARCH_SRC || evr == ARCH_NOSRC)
	    {
	      Solvable *s;
	      for (p = 1, s = pool->solvables + p; p < pool->nsolvables; p++, s++)
		{
		  if (!s->repo)
		    continue;
		  if (s->arch != evr && s->arch != ARCH_NOSRC)
		    continue;
		  if (pool_disabled_solvable(pool, s))
		    continue;
		  if (!name || pool_match_nevr(pool, s, name))
		    queue_push(&plist, p);
		}
	      break;
	    }
	  if (!name)
	    {
	      FOR_POOL_SOLVABLES(p)
		{
		  Solvable *s = pool->solvables + p;
		  if (s->repo != pool->installed && !pool_installable(pool, s))
		    continue;
		  if (s->arch == evr)
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
	  break;
	case REL_MULTIARCH:
	  if (evr != ARCH_ANY)
	    break;
	  /* XXX : need to check for Multi-Arch: allowed! */
	  wp = pool_whatprovides(pool, name);
	  break;
	case REL_KIND:
	  /* package kind filtering */
	  if (!name)
	    {
	      FOR_POOL_SOLVABLES(p)
		{
		  Solvable *s = pool->solvables + p;
		  if (s->repo != pool->installed && !pool_installable(pool, s))
		    continue;
		  if (pool_is_kind(pool, s->name, evr))
		    queue_push(&plist, p);
		}
	      break;
	    }
	  wp = pool_whatprovides(pool, name);
	  pp = pool->whatprovidesdata + wp;
	  while ((p = *pp++) != 0)
	    {
	      Solvable *s = pool->solvables + p;
	      if (pool_is_kind(pool, s->name, evr))
		queue_push(&plist, p);
	      else
		wp = 0;
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
      if (wp)
	{
	  /* we can reuse an existing entry */
	  queue_free(&plist);
	  pool->whatprovides_rel[d] = wp;
	  return wp;
	}
    }
  else if (flags)
    {
      Id *ppaux = 0;
      int eqmagic = 0;
      /* simple version comparison relation */
#if 0
      POOL_DEBUG(SOLV_DEBUG_STATS, "addrelproviders: what provides %s?\n", pool_dep2str(pool, name));
#endif
      pp = pool_whatprovides_ptr(pool, name);
      if (!ISRELDEP(name) && name < pool->whatprovidesauxoff)
	ppaux = pool->whatprovidesaux[name] ? pool->whatprovidesauxdata + pool->whatprovidesaux[name] : 0;
      while (ISRELDEP(name))
	{
          rd = GETRELDEP(pool, name);
	  name = rd->name;
	}
      while ((p = *pp++) != 0)
	{
	  Solvable *s = pool->solvables + p;
	  if (ppaux)
	    {
	      pid = *ppaux++;
	      if (pid && pid != 1)
		{
#if 0
	          POOL_DEBUG(SOLV_DEBUG_STATS, "addrelproviders: aux hit %d %s\n", p, pool_dep2str(pool, pid));
#endif
		  if (!ISRELDEP(pid))
		    {
		      if (pid != name)
			continue;		/* wrong provides name */
		      if (pool->disttype == DISTTYPE_DEB)
			continue;		/* unversioned provides can never match versioned deps */
		    }
		  else
		    {
		      prd = GETRELDEP(pool, pid);
		      if (prd->name != name)
			continue;		/* wrong provides name */
		      if (!eqmagic)
			eqmagic = rpmeqmagic_init(pool, flags, evr);
		      if (eqmagic > 0 && prd->flags == REL_EQ && !ISRELDEP(prd->evr) &&
			  rpmeqmagic_cantmatch(pool, prd->flags, prd->evr, eqmagic))
			continue;
		      /* right package, both deps are rels. check flags/evr */
		      if (!pool_match_flags_evr(pool, prd->flags, prd->evr, flags, evr))
			continue;
		    }
		  queue_push(&plist, p);
		  continue;
		}
	    }
	  if (!s->provides)
	    {
	      /* no provides - check nevr */
	      if (pool_match_nevr_rel(pool, s, MAKERELDEP(d)))
	        queue_push(&plist, p);
	      continue;
	    }
	  /* solvable p provides name in some rels */
	  if (!eqmagic)
	    eqmagic = rpmeqmagic_init(pool, flags, evr);
	  pidp = s->repo->idarraydata + s->provides;
	  while ((pid = *pidp++) != 0)
	    {
	      if (!ISRELDEP(pid))
		{
		  if (pid != name)
		    continue;		/* wrong provides name */
		  if (pool->disttype == DISTTYPE_DEB)
		    continue;		/* unversioned provides can never match versioned deps */
		  break;
		}
	      prd = GETRELDEP(pool, pid);
	      if (prd->name != name)
		continue;		/* wrong provides name */
	      /* right package, both deps are rels. check flags/evr */
	      if (eqmagic > 0 && prd->flags == REL_EQ && !ISRELDEP(prd->evr) &&
	          rpmeqmagic_cantmatch(pool, prd->flags, prd->evr, eqmagic))
		continue;
	      if (pool_match_flags_evr(pool, prd->flags, prd->evr, flags, evr))
		break;	/* matches */
	    }
	  if (!pid)
	    continue;	/* none of the providers matched */
	  queue_push(&plist, p);
	}
      /* make our system solvable provide all unknown rpmlib() stuff */
      if (plist.count == 0 && !strncmp(pool_id2str(pool, name), "rpmlib(", 7))
	queue_push(&plist, SYSTEMSOLVABLE);
    }
  /* add providers to whatprovides */
#if 0
  POOL_DEBUG(SOLV_DEBUG_STATS, "addrelproviders: adding %d packages to %d\n", plist.count, d);
#endif
  pool->whatprovides_rel[d] = pool_queuetowhatprovides(pool, &plist);
  queue_free(&plist);

  return pool->whatprovides_rel[d];
}

void
pool_flush_namespaceproviders(Pool *pool, Id ns, Id evr)
{
  int nrels = pool->nrels;
  Id d;
  Reldep *rd;

  if (!pool->whatprovides_rel)
    return;
  for (d = 1, rd = pool->rels + d; d < nrels; d++, rd++)
    {
      if (rd->flags != REL_NAMESPACE || rd->name == NAMESPACE_OTHERPROVIDERS)
	continue;
      if (ns && rd->name != ns)
	continue;
      if (evr && rd->evr != evr)
	continue;
      pool->whatprovides_rel[d] = 0;
    }
}

/* intersect dependencies in keyname with dep, return list of matching packages */
void
pool_whatmatchesdep(Pool *pool, Id keyname, Id dep, Queue *q, int marker)
{
  Id p;
  Queue qq;
  int i;

  queue_empty(q);
  if (keyname == SOLVABLE_NAME)
    {
      Id pp;
      FOR_PROVIDES(p, pp, dep)
        if (pool_match_dep(pool, p, dep))
	  queue_push(q, p);
      return;
    }
  queue_init(&qq);
  FOR_POOL_SOLVABLES(p)
    {
      Solvable *s = pool->solvables + p;
      if (s->repo->disabled)
	continue;
      if (s->repo != pool->installed && !pool_installable(pool, s))
	continue;
      if (qq.count)
	queue_empty(&qq);
      solvable_lookup_deparray(s, keyname, &qq, marker);
      for (i = 0; i < qq.count; i++)
	if (pool_match_dep(pool, qq.elements[i], dep))
	  {
	    queue_push(q, p);
	    break;
	  }
    }
  queue_free(&qq);
}

/* check if keyname contains dep, return list of matching packages */
void
pool_whatcontainsdep(Pool *pool, Id keyname, Id dep, Queue *q, int marker)
{
  Id p;
  Queue qq;
  int i;

  queue_empty(q);
  if (!dep)
    return;
  queue_init(&qq);
  FOR_POOL_SOLVABLES(p)
    {
      Solvable *s = pool->solvables + p;
      if (s->repo->disabled)
        continue;
      if (s->repo != pool->installed && !pool_installable(pool, s))
        continue;
      if (qq.count)
        queue_empty(&qq);
      solvable_lookup_deparray(s, keyname, &qq, marker);
      for (i = 0; i < qq.count; i++)
        if (qq.elements[i] == dep)
          {
            queue_push(q, p);
            break;
          }
    }
  queue_free(&qq);
}

/*************************************************************************/

void
pool_debug(Pool *pool, int type, const char *format, ...)
{
  va_list args;
  char buf[1024];

  if ((type & (SOLV_FATAL|SOLV_ERROR)) == 0)
    {
      if ((pool->debugmask & type) == 0)
	return;
    }
  va_start(args, format);
  if (!pool->debugcallback)
    {
      if ((type & (SOLV_FATAL|SOLV_ERROR)) == 0 && !(pool->debugmask & SOLV_DEBUG_TO_STDERR))
        vprintf(format, args);
      else
        vfprintf(stderr, format, args);
      return;
    }
  vsnprintf(buf, sizeof(buf), format, args);
  va_end(args);
  pool->debugcallback(pool, pool->debugcallbackdata, type, buf);
}

int
pool_error(Pool *pool, int ret, const char *format, ...)
{
  va_list args;
  int l;
  va_start(args, format);
  if (!pool->errstr)
    {
      pool->errstra = 1024;
      pool->errstr = solv_malloc(pool->errstra);
    }
  if (!*format)
    {
      *pool->errstr = 0;
      l = 0;
    }
  else
    l = vsnprintf(pool->errstr, pool->errstra, format, args);
  va_end(args);
  if (l >= 0 && l + 1 > pool->errstra)
    {
      pool->errstra = l + 256;
      pool->errstr = solv_realloc(pool->errstr, pool->errstra);
      va_start(args, format);
      l = vsnprintf(pool->errstr, pool->errstra, format, args);
      va_end(args);
    }
  if (l < 0)
    strcpy(pool->errstr, "unknown error");
  if (pool->debugmask & SOLV_ERROR)
    pool_debug(pool, SOLV_ERROR, "%s\n", pool->errstr);
  return ret;
}

char *
pool_errstr(Pool *pool)
{
  return pool->errstr ? pool->errstr : "no error";
}

void
pool_setdebuglevel(Pool *pool, int level)
{
  int mask = SOLV_DEBUG_RESULT;
  if (level > 0)
    mask |= SOLV_DEBUG_STATS|SOLV_DEBUG_ANALYZE|SOLV_DEBUG_UNSOLVABLE|SOLV_DEBUG_SOLVER|SOLV_DEBUG_TRANSACTION|SOLV_ERROR;
  if (level > 1)
    mask |= SOLV_DEBUG_JOB|SOLV_DEBUG_SOLUTIONS|SOLV_DEBUG_POLICY;
  if (level > 2)
    mask |= SOLV_DEBUG_PROPAGATE;
  if (level > 3)
    mask |= SOLV_DEBUG_RULE_CREATION;
  mask |= pool->debugmask & SOLV_DEBUG_TO_STDERR;	/* keep bit */
  pool->debugmask = mask;
}

void pool_setdebugcallback(Pool *pool, void (*debugcallback)(struct _Pool *, void *data, int type, const char *str), void *debugcallbackdata)
{
  pool->debugcallback = debugcallback;
  pool->debugcallbackdata = debugcallbackdata;
}

void pool_setdebugmask(Pool *pool, int mask)
{
  pool->debugmask = mask;
}

void pool_setloadcallback(Pool *pool, int (*cb)(struct _Pool *, struct _Repodata *, void *), void *loadcbdata)
{
  pool->loadcallback = cb;
  pool->loadcallbackdata = loadcbdata;
}

void pool_setnamespacecallback(Pool *pool, Id (*cb)(struct _Pool *, void *, Id, Id), void *nscbdata)
{
  pool->nscallback = cb;
  pool->nscallbackdata = nscbdata;
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

  pool->languagecache = solv_free(pool->languagecache);
  pool->languagecacheother = 0;
  for (i = 0; i < pool->nlanguages; i++)
    free((char *)pool->languages[i]);
  pool->languages = solv_free((void *)pool->languages);
  pool->nlanguages = nlanguages;
  if (!nlanguages)
    return;
  pool->languages = solv_calloc(nlanguages, sizeof(const char **));
  for (i = 0; i < pool->nlanguages; i++)
    pool->languages[i] = solv_strdup(languages[i]);
}

Id
pool_id2langid(Pool *pool, Id id, const char *lang, int create)
{
  const char *n;
  char buf[256], *p;
  int l;

  if (!lang || !*lang)
    return id;
  n = pool_id2str(pool, id);
  l = strlen(n) + strlen(lang) + 2;
  if (l > sizeof(buf))
    p = solv_malloc(strlen(n) + strlen(lang) + 2);
  else
    p = buf;
  sprintf(p, "%s:%s", n, lang);
  id = pool_str2id(pool, p, create);
  if (p != buf)
    free(p);
  return id;
}

char *
pool_alloctmpspace(Pool *pool, int len)
{
  int n = pool->tmpspace.n;
  if (!len)
    return 0;
  if (len > pool->tmpspace.len[n])
    {
      pool->tmpspace.buf[n] = solv_realloc(pool->tmpspace.buf[n], len + 32);
      pool->tmpspace.len[n] = len + 32;
    }
  pool->tmpspace.n = (n + 1) % POOL_TMPSPACEBUF;
  return pool->tmpspace.buf[n];
}

static char *
pool_alloctmpspace_free(Pool *pool, const char *space, int len)
{
  if (space)
    {
      int n, oldn;
      n = oldn = pool->tmpspace.n;
      for (;;)
	{
	  if (!n--)
	    n = POOL_TMPSPACEBUF - 1;
	  if (n == oldn)
	    break;
	  if (pool->tmpspace.buf[n] != space)
	    continue;
	  if (len > pool->tmpspace.len[n])
	    {
	      pool->tmpspace.buf[n] = solv_realloc(pool->tmpspace.buf[n], len + 32);
	      pool->tmpspace.len[n] = len + 32;
	    }
          return pool->tmpspace.buf[n];
	}
    }
  return 0;
}

void
pool_freetmpspace(Pool *pool, const char *space)
{
  int n = pool->tmpspace.n;
  if (!space)
    return;
  n = (n + (POOL_TMPSPACEBUF - 1)) % POOL_TMPSPACEBUF;
  if (pool->tmpspace.buf[n] == space)
    pool->tmpspace.n = n;
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

char *
pool_tmpappend(Pool *pool, const char *str1, const char *str2, const char *str3)
{
  int l1, l2, l3;
  char *s, *str;

  l1 = str1 ? strlen(str1) : 0;
  l2 = str2 ? strlen(str2) : 0;
  l3 = str3 ? strlen(str3) : 0;
  str = pool_alloctmpspace_free(pool, str1, l1 + l2 + l3 + 1);
  if (str)
    str1 = str;
  else
    str = pool_alloctmpspace(pool, l1 + l2 + l3 + 1);
  s = str;
  if (l1)
    {
      if (s != str1)
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

const char *
pool_bin2hex(Pool *pool, const unsigned char *buf, int len)
{
  char *s;
  if (!len)
    return "";
  s = pool_alloctmpspace(pool, 2 * len + 1);
  solv_bin2hex(buf, len, s);
  return s;
}

const char *
pool_lookup_str(Pool *pool, Id entry, Id keyname)
{
  if (entry == SOLVID_POS && pool->pos.repo)
    return repo_lookup_str(pool->pos.repo, pool->pos.repodataid ? entry : pool->pos.solvid, keyname);
  if (entry <= 0)
    return 0;
  return solvable_lookup_str(pool->solvables + entry, keyname);
}

Id
pool_lookup_id(Pool *pool, Id entry, Id keyname)
{
  if (entry == SOLVID_POS && pool->pos.repo)
    return repo_lookup_id(pool->pos.repo, pool->pos.repodataid ? entry : pool->pos.solvid, keyname);
  if (entry <= 0)
    return 0;
  return solvable_lookup_id(pool->solvables + entry, keyname);
}

unsigned long long
pool_lookup_num(Pool *pool, Id entry, Id keyname, unsigned long long notfound)
{
  if (entry == SOLVID_POS && pool->pos.repo)
    return repo_lookup_num(pool->pos.repo, pool->pos.repodataid ? entry : pool->pos.solvid, keyname, notfound);
  if (entry <= 0)
    return notfound;
  return solvable_lookup_num(pool->solvables + entry, keyname, notfound);
}

int
pool_lookup_void(Pool *pool, Id entry, Id keyname)
{
  if (entry == SOLVID_POS && pool->pos.repo)
    return repo_lookup_void(pool->pos.repo, pool->pos.repodataid ? entry : pool->pos.solvid, keyname);
  if (entry <= 0)
    return 0;
  return solvable_lookup_void(pool->solvables + entry, keyname);
}

const unsigned char *
pool_lookup_bin_checksum(Pool *pool, Id entry, Id keyname, Id *typep)
{
  if (entry == SOLVID_POS && pool->pos.repo)
    return repo_lookup_bin_checksum(pool->pos.repo, pool->pos.repodataid ? entry : pool->pos.solvid, keyname, typep);
  if (entry <= 0)
    return 0;
  return solvable_lookup_bin_checksum(pool->solvables + entry, keyname, typep);
}

const char *
pool_lookup_checksum(Pool *pool, Id entry, Id keyname, Id *typep)
{
  if (entry == SOLVID_POS && pool->pos.repo)
    return repo_lookup_checksum(pool->pos.repo, pool->pos.repodataid ? entry : pool->pos.solvid, keyname, typep);
  if (entry <= 0)
    return 0;
  return solvable_lookup_checksum(pool->solvables + entry, keyname, typep);
}

int
pool_lookup_idarray(Pool *pool, Id entry, Id keyname, Queue *q)
{
  if (entry == SOLVID_POS && pool->pos.repo)
    return repo_lookup_idarray(pool->pos.repo, pool->pos.repodataid ? entry : pool->pos.solvid, keyname, q);
  if (entry <= 0)
    return 0;
  return solvable_lookup_idarray(pool->solvables + entry, keyname, q);
}

const char *
pool_lookup_deltalocation(Pool *pool, Id entry, unsigned int *medianrp)
{
  const char *loc;
  if (medianrp)
    *medianrp = 0;
  if (entry != SOLVID_POS)
    return 0;
  loc = pool_lookup_str(pool, entry, DELTA_LOCATION_DIR);
  loc = pool_tmpjoin(pool, loc, loc ? "/" : 0, pool_lookup_str(pool, entry, DELTA_LOCATION_NAME));
  loc = pool_tmpappend(pool, loc, "-", pool_lookup_str(pool, entry, DELTA_LOCATION_EVR));
  loc = pool_tmpappend(pool, loc, ".", pool_lookup_str(pool, entry, DELTA_LOCATION_SUFFIX));
  return loc;
}

static void
add_new_provider(Pool *pool, Id id, Id p)
{
  Queue q;
  Id *pp;

  while (ISRELDEP(id))
    {
      Reldep *rd = GETRELDEP(pool, id);
      id = rd->name;
    }

  queue_init(&q);
  for (pp = pool->whatprovidesdata + pool->whatprovides[id]; *pp; pp++)
    {
      if (*pp == p)
	{
	  queue_free(&q);
	  return;
	}
      if (*pp > p)
	{
	  queue_push(&q, p);
	  p = 0;
	}
      queue_push(&q, *pp);
    }
  if (p)
    queue_push(&q, p);
  pool->whatprovides[id] = pool_queuetowhatprovides(pool, &q);
  if (id < pool->whatprovidesauxoff)
    pool->whatprovidesaux[id] = 0;	/* sorry */
  queue_free(&q);
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
  for (i = 0; i < conflicts->count; i += 6)
    {
      fn = conflicts->elements[i];
      p = conflicts->elements[i + 1];
      md5 = conflicts->elements[i + 2];
      q = conflicts->elements[i + 4];
      id = pool_rel2id(pool, fn, md5, REL_FILECONFLICT, 1);
      s = pool->solvables + p;
      if (!s->repo)
	continue;
      s->provides = repo_addid_dep(s->repo, s->provides, id, SOLVABLE_FILEMARKER);
      if (pool->whatprovides)
	add_new_provider(pool, fn, p);
      if (pool->whatprovides_rel)
	pool->whatprovides_rel[GETRELID(id)] = 0;	/* clear cache */
      s = pool->solvables + q;
      if (!s->repo)
	continue;
      s->conflicts = repo_addid_dep(s->repo, s->conflicts, id, 0);
    }
  if (!hadhashes)
    pool_freeidhashes(pool);
}

char *
pool_prepend_rootdir(Pool *pool, const char *path)
{
  if (!path)
    return 0;
  if (!pool->rootdir)
    return solv_strdup(path);
  return solv_dupjoin(pool->rootdir, "/", *path == '/' ? path + 1 : path);
}

const char *
pool_prepend_rootdir_tmp(Pool *pool, const char *path)
{
  if (!path)
    return 0;
  if (!pool->rootdir)
    return path;
  return pool_tmpjoin(pool, pool->rootdir, "/", *path == '/' ? path + 1 : path);
}

void
pool_set_rootdir(Pool *pool, const char *rootdir)
{
  solv_free(pool->rootdir);
  pool->rootdir = solv_strdup(rootdir);
}

const char *
pool_get_rootdir(Pool *pool)
{
  return pool->rootdir;
}

/* only used in libzypp */
void
pool_set_custom_vendorcheck(Pool *pool, int (*vendorcheck)(Pool *, Solvable *, Solvable *))
{
  pool->custom_vendorcheck = vendorcheck;
}

/* EOF */
