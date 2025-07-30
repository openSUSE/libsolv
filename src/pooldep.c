/*
 * Copyright (c) 2025, SUSE LLC
 *
 * This program is licensed under the BSD license, read LICENSE.BSD
 * for further information
 */

/*
 * pooldep.c
 *
 * dependency matching and searching
 */

#include <stdio.h>
#include <stdlib.h>
#include <stdarg.h>
#include <unistd.h>
#include <string.h>
#include <sys/types.h>

#include "pool.h"
#include "repo.h"
#include "util.h"
#include "evr.h"


#if defined(MULTI_SEMANTICS)
# define EVRCMP_DEPCMP (pool->disttype == DISTTYPE_DEB ? EVRCMP_COMPARE : EVRCMP_MATCH_RELEASE)
#elif defined(DEBIAN)
# define EVRCMP_DEPCMP EVRCMP_COMPARE
#else
# define EVRCMP_DEPCMP EVRCMP_MATCH_RELEASE
#endif


#if defined(HAIKU) || defined(MULTI_SEMANTICS)
static int
pool_intersect_evrs_rel_compat(Pool *pool, Reldep *range, int flags, int evr)
{
  /* range->name is the actual version, range->evr the backwards compatibility
     version. If flags are '>=' or '>', we match the compatibility version
     as well, otherwise only the actual version. */
  if (!(flags & REL_GT) || (flags & REL_LT))
    return pool_intersect_evrs(pool, REL_EQ, range->name, flags, evr);
  return pool_intersect_evrs(pool, REL_LT | REL_EQ, range->name, flags, evr) &&
         pool_intersect_evrs(pool, REL_GT | REL_EQ, range->evr, REL_EQ, evr);
}
#endif

/* match (flags, evr) against provider (pflags, pevr) */
/* note that this code is also in poolwhatprovides */
int
pool_intersect_evrs(Pool *pool, int pflags, Id pevr, int flags, int evr)
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
	return pool_intersect_evrs_rel_compat(pool, rd, flags, evr);
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

static int
is_interval_dep(Pool *pool, Id d1, Id d2)
{
  Reldep *rd1, *rd2;
  if (!ISRELDEP(d1) || !ISRELDEP(d2))
    return 0;
  rd1 = GETRELDEP(pool, d1);
  rd2 = GETRELDEP(pool, d2);
  if (rd1->name != rd2->name || rd1->flags >= 8 || rd2->flags >= 8)
    return 0;
  if (((rd1->flags ^ rd2->flags) & (REL_LT|REL_GT)) != (REL_LT|REL_GT))
    return 0;
  return 1;
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
	  if (rd1->flags == REL_WITH && is_interval_dep(pool, rd1->name, rd1->evr))
	    return pool_match_dep(pool, rd1->name, d2) && pool_match_dep(pool, rd1->evr, d2);
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
	  if (rd2->flags == REL_WITH && is_interval_dep(pool, rd2->name, rd2->evr))
	    return pool_match_dep(pool, d1, rd2->name) && pool_match_dep(pool, d1, rd2->evr);
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
	return 0;	/* cannot match as d1 != d2 */
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
        if (pool_match_nevr(pool, pool->solvables + p, dep))
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

/* intersect dependencies in keyname with all provides of solvable solvid,
 * return list of matching packages */
/* this currently only works for installable packages */
void
pool_whatmatchessolvable(Pool *pool, Id keyname, Id solvid, Queue *q, int marker)
{
  Id p;
  Queue qq;
  Map missc;		/* cache for misses */
  int reloff;

  queue_empty(q);
  queue_init(&qq);
  reloff = pool->ss.nstrings;
  map_init(&missc, reloff + pool->nrels);
  FOR_POOL_SOLVABLES(p)
    {
      Solvable *s = pool->solvables + p;
      if (p == solvid)
	continue;	/* filter out self-matches */
      if (s->repo->disabled)
	continue;
      if (s->repo != pool->installed && !pool_installable(pool, s))
	continue;
      if (solvable_matchessolvable_int(s, keyname, marker, solvid, 0, &qq, &missc, reloff, 0))
        queue_push(q, p);
    }
  map_free(&missc);
  queue_free(&qq);
}

static int
pool_dep_fulfilled_in_map_cplx(Pool *pool, const Map *map, Reldep *rd)
{
  if (rd->flags == REL_COND)
    {
      if (ISRELDEP(rd->evr))
	{
	  Reldep *rd2 = GETRELDEP(pool, rd->evr);
	  if (rd2->flags == REL_ELSE)
	    {
	      if (pool_dep_fulfilled_in_map(pool, map, rd2->name))
		return pool_dep_fulfilled_in_map(pool, map, rd->name);
	      return pool_dep_fulfilled_in_map(pool, map, rd2->evr);
	    }
	}
      if (pool_dep_fulfilled_in_map(pool, map, rd->name))
	return 1;
      return !pool_dep_fulfilled_in_map(pool, map, rd->evr);
    }
  if (rd->flags == REL_UNLESS)
    {
      if (ISRELDEP(rd->evr))
	{
	  Reldep *rd2 = GETRELDEP(pool, rd->evr);
	  if (rd2->flags == REL_ELSE)
	    {
	      if (!pool_dep_fulfilled_in_map(pool, map, rd2->name))
		return pool_dep_fulfilled_in_map(pool, map, rd->name);
	      return pool_dep_fulfilled_in_map(pool, map, rd2->evr);
	    }
	}
      if (!pool_dep_fulfilled_in_map(pool, map, rd->name))
	return 0;
      return !pool_dep_fulfilled_in_map(pool, map, rd->evr);
    }
  if (rd->flags == REL_AND)
    {
      if (!pool_dep_fulfilled_in_map(pool, map, rd->name))
	return 0;
      return pool_dep_fulfilled_in_map(pool, map, rd->evr);
    }
  if (rd->flags == REL_OR)
    {
      if (pool_dep_fulfilled_in_map(pool, map, rd->name))
	return 1;
      return pool_dep_fulfilled_in_map(pool, map, rd->evr);
    }
  return 0;
}

int
pool_dep_fulfilled_in_map(Pool *pool, const Map *map, Id dep)
{
  Id p, pp;

  if (ISRELDEP(dep)) {
    Reldep *rd = GETRELDEP(pool, dep);
    if (rd->flags == REL_COND || rd->flags == REL_UNLESS ||
        rd->flags == REL_AND || rd->flags == REL_OR)
      return pool_dep_fulfilled_in_map_cplx(pool, map, rd);
    if (rd->flags == REL_NAMESPACE && rd->name == NAMESPACE_SPLITPROVIDES)
      return 0;
  }
  FOR_PROVIDES(p, pp, dep) {
    if (MAPTST(map, p))
      return 1;
  }
  return 0;
}
/* EOF */
