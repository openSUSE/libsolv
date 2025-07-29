/*
 * Copyright (c) 2025, SUSE LLC
 *
 * This program is licensed under the BSD license, read LICENSE.BSD
 * for further information
 */

/*
 * poolwhatprovides.c
 *
 * dependency provider lookup functions
 */

#include <stdio.h>
#include <stdlib.h>
#include <stdarg.h>
#include <unistd.h>
#include <string.h>
#include <sys/types.h>

#include "pool.h"
#include "poolid_private.h"
#include "repo.h"
#include "util.h"
#include "evr.h"
#ifdef ENABLE_CONDA
#include "conda.h"
#endif

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
      if (!pool_installable_whatprovides(pool, s))
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
      if (!pool_installable_whatprovides(pool, s))
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
  if (pool->addedfileprovides == 1)
    {
      /* lazyly add file provides for nonstd */
      for (i = 0; i < pool->nonstd_nids; i++)
	{
	  Id id = pool->nonstd_ids[i];
	  const char *str = pool->ss.stringspace + pool->ss.strings[id];
	  if (str[0] != '/')		/* just in case, all entries should start with '/' */
	    continue;
	  /* setup lazy adding, but remember old value */
	  if (pool->whatprovides[id] > 1)
	    queue_push2(&pool->lazywhatprovidesq, id, pool->whatprovides[id]);
	  pool->whatprovides[id] = 0;
	  if (pool->whatprovidesaux)
	    pool->whatprovidesaux[id] = 0;	/* sorry */
	}
    }
  else if (!pool->addedfileprovides && pool->disttype == DISTTYPE_RPM)
    {
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
    }
  if (pool->lazywhatprovidesq.count)
    POOL_DEBUG(SOLV_DEBUG_STATS, "lazywhatprovidesq size: %d entries\n", pool->lazywhatprovidesq.count / 2);

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
  dataiterator_init(&di, pool, 0, 0, SOLVABLE_FILELIST, str, SEARCH_STRING|SEARCH_FILES);
  for (; dataiterator_step(&di); dataiterator_skip_solvable(&di))
    {
      Solvable *s = pool->solvables + di.solvid;
      /* XXX: maybe should add a provides dependency to the solvables
       * OTOH this is only needed for rel deps that filter the provides,
       * and those should not use filelist entries */
      if (s->repo->disabled)
	continue;
      if (!pool_installable_whatprovides(pool, s))
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


#if defined(MULTI_SEMANTICS)
# define EVRCMP_DEPCMP (pool->disttype == DISTTYPE_DEB ? EVRCMP_COMPARE : EVRCMP_MATCH_RELEASE)
#elif defined(DEBIAN)
# define EVRCMP_DEPCMP EVRCMP_COMPARE
#else
# define EVRCMP_DEPCMP EVRCMP_MATCH_RELEASE
#endif

/* match (flags, evr) against provider (pflags, pevr) */
/* this copies parts of pool_intersect_evrs for speed */
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
    return pool_intersect_evrs(pool, pflags, pevr, flags, evr);		/* too complex */
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
		  if (!pool_installable_whatprovides(pool, s))
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
#ifdef ENABLE_CONDA
	case REL_CONDA:
          wp = pool_addrelproviders_conda(pool, name, evr, &plist);
	  break;
#endif
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
      if (!ISRELDEP(name) && (Offset)name < pool->whatprovidesauxoff)
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
	  if (!s->provides || s->arch == ARCH_SRC || s->arch == ARCH_NOSRC)
	    {
	      /* no provides or src rpm - check nevr */
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
      if (pool->whatprovides_rel[d])
        pool_set_whatprovides(pool, MAKERELDEP(d), 0);
    }
}

void
pool_set_whatprovides(Pool *pool, Id id, Id providers)
{
  int d, nrels = pool->nrels;
  Reldep *rd;
  Map m;

  /* set new entry */
  if (ISRELDEP(id))
    {
      d = GETRELID(id);
      pool->whatprovides_rel[d] = providers;
      d++;
    }
  else
    {
      pool->whatprovides[id] = providers;
      if ((Offset)id < pool->whatprovidesauxoff)
	pool->whatprovidesaux[id] = 0;	/* sorry */
      d = 1;
    }
  if (!pool->whatprovides_rel)
    return;
  /* clear cache of all rels that use it */
  map_init(&m, 0);
  for (rd = pool->rels + d; d < nrels; d++, rd++)
    {
      if (rd->name == id || rd->evr == id ||
	  (m.size && ISRELDEP(rd->name) && MAPTST(&m, GETRELID(rd->name))) || 
	  (m.size && ISRELDEP(rd->evr)  && MAPTST(&m, GETRELID(rd->evr))))
	{
	  pool->whatprovides_rel[d] = 0;	/* clear cache */
	  if (!m.size)
	    map_init(&m, nrels);
	  MAPSET(&m, d);
	}
    }
  map_free(&m);
}

void
pool_add_new_provider(Pool *pool, Id id, Id p)
{
  Queue q;
  Id *pp;

  /* find whatprovides entry */
  while (ISRELDEP(id))
    {
      Reldep *rd = GETRELDEP(pool, id);
      id = rd->name;
    }

  /* add new provider to existing list keeping it sorted */
  queue_init(&q);
  for (pp = pool->whatprovidesdata + pool->whatprovides[id]; *pp; pp++)
    {
      if (*pp == p)
	{
	  queue_free(&q);	/* already have it */
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
  pool_set_whatprovides(pool, id, pool_queuetowhatprovides(pool, &q));
  queue_free(&q);
}

/* EOF */
