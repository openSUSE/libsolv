/*
 * Copyright (c) 2007, Novell Inc.
 *
 * This program is licensed under the BSD license, read LICENSE.BSD
 * for further information
 */

/*
 * poolid.c
 *
 * Id management
 */

#include <stdlib.h>
#include <string.h>
#include <stdio.h>

#include "pool.h"
#include "poolid.h"
#include "poolid_private.h"
#include "util.h"


// intern string into pool
// return Id

Id
str2id(Pool *pool, const char *str, int create)
{
  int old_nstrings = pool->ss.nstrings;
  Id id = stringpool_str2id (&pool->ss, str, create);
  /* If we changed the ID->string relations we need to get rid of an
     eventually existing provides lookup cache.  */
  if (old_nstrings != pool->ss.nstrings)
    pool_freewhatprovides(pool);
  return id;
}

Id
rel2id(Pool *pool, Id name, Id evr, int flags, int create)
{
  Hashval h;
  unsigned int hh;
  Hashmask hashmask;
  int i;
  Id id;
  Hashtable hashtbl;
  Reldep *ran;

  hashmask = pool->relhashmask;
  hashtbl = pool->relhashtbl;
  ran = pool->rels;
  
  // extend hashtable if needed
  if (pool->nrels * 2 > hashmask)
    {
      xfree(pool->relhashtbl);
      pool->relhashmask = hashmask = mkmask(pool->ss.nstrings + REL_BLOCK);
      pool->relhashtbl = hashtbl = xcalloc(hashmask + 1, sizeof(Id));
      // rehash all rels into new hashtable
      for (i = 1; i < pool->nrels; i++)
	{
	  h = relhash(ran[i].name, ran[i].evr, ran[i].flags) & hashmask;
	  hh = HASHCHAIN_START;
	  while (hashtbl[h])
	    h = HASHCHAIN_NEXT(h, hh, hashmask);
	  hashtbl[h] = i;
	}
    }
  
  // compute hash and check for match

  h = relhash(name, evr, flags) & hashmask;
  hh = HASHCHAIN_START;
  while ((id = hashtbl[h]) != 0)
    {
      if (ran[id].name == name && ran[id].evr == evr && ran[id].flags == flags)
	break;
      h = HASHCHAIN_NEXT(h, hh, hashmask);
    }
  if (id)
    return MAKERELDEP(id);

  if (!create)
    return ID_NULL;

  pool_freewhatprovides(pool);

  id = pool->nrels++;
  // extend rel space if needed
  if ((id & REL_BLOCK) == 0)
    pool->rels = xrealloc(pool->rels, ((pool->nrels + REL_BLOCK) & ~REL_BLOCK) * sizeof(Reldep));
  hashtbl[h] = id;
  ran = pool->rels + id;
  ran->name = name;
  ran->evr = evr;
  ran->flags = flags;
  return MAKERELDEP(id);
}


// Id -> String
// for rels (returns name only) and strings
// 
const char *
id2str(Pool *pool, Id id)
{
  if (ISRELDEP(id))
    {
      Reldep *rd = GETRELDEP(pool, id);
      if (ISRELDEP(rd->name))
	return "REL";
      return pool->ss.stringspace + pool->ss.strings[rd->name];
    }
  return pool->ss.stringspace + pool->ss.strings[id];
}

static const char *rels[] = {
  " ! ",
  " > ",
  " = ",
  " >= ",
  " < ",
  " <> ",
  " <= ",
  " <=> "
};


// get operator for RelId
const char *
id2rel(Pool *pool, Id id)
{
  Reldep *rd;
  if (!ISRELDEP(id))
    return "";
  rd = GETRELDEP(pool, id);
  switch (rd->flags)
    {
    case 0: case 1: case 2: case 3:
    case 4: case 5: case 6: case 7:
      return rels[rd->flags & 7];
    case REL_AND:
      return " AND ";
    case REL_OR:
      return " OR ";
    case REL_WITH:
      return " WITH ";
    case REL_NAMESPACE:
      return " NAMESPACE ";
    default:
      break;
    }
  return " ??? ";
}


// get e:v.r for Id
// 
const char *
id2evr(Pool *pool, Id id)
{
  Reldep *rd;
  if (!ISRELDEP(id))
    return "";
  rd = GETRELDEP(pool, id);
  if (ISRELDEP(rd->evr))
    return "REL";
  return pool->ss.stringspace + pool->ss.strings[rd->evr];
}

const char *
dep2str(Pool *pool, Id id)
{
  Reldep *rd;
  const char *sr;
  char *s1, *s2;
  int n, l, ls1, ls2, lsr;

  if (!ISRELDEP(id))
    return pool->ss.stringspace + pool->ss.strings[id];
  rd = GETRELDEP(pool, id);
  n = pool->dep2strn;

  sr = id2rel(pool, id);
  lsr = strlen(sr);

  s2 = (char *)dep2str(pool, rd->evr);
  pool->dep2strn = n;
  ls2 = strlen(s2);

  s1 = (char *)dep2str(pool, rd->name);
  pool->dep2strn = n;
  ls1 = strlen(s1);

  if (rd->flags == REL_NAMESPACE)
    {
      sr = "(";
      lsr = 1;
      ls2++;
    }

  l = ls1 + ls2 + lsr;
  if (l + 1 > pool->dep2strlen[n])
    {
      if (s1 != pool->dep2strbuf[n])
        pool->dep2strbuf[n] = xrealloc(pool->dep2strbuf[n], l + 32);
      else
	{
          pool->dep2strbuf[n] = xrealloc(pool->dep2strbuf[n], l + 32);
          s1 = pool->dep2strbuf[n];
	}
      pool->dep2strlen[n] = l + 32;
    }
  if (s1 != pool->dep2strbuf[n])
    {
      strcpy(pool->dep2strbuf[n], s1);
      s1 = pool->dep2strbuf[n];
    }
  strcpy(s1 + ls1, sr);
  pool->dep2strbuf[n] = s1 + ls1 + lsr;
  s2 = (char *)dep2str(pool, rd->evr);
  if (s2 != pool->dep2strbuf[n])
    strcpy(pool->dep2strbuf[n], s2);
  pool->dep2strbuf[n] = s1;
  if (rd->flags == REL_NAMESPACE)
    {
      s1[ls1 + ls2 + lsr - 1] = ')';
      s1[ls1 + ls2 + lsr] = 0;
    }
  pool->dep2strn = (n + 1) % DEP2STRBUF;
  return s1;
}


void
pool_shrink_strings(Pool *pool)
{
  stringpool_shrink (&pool->ss);
}

void
pool_shrink_rels(Pool *pool)
{
  pool->rels = (Reldep *)xrealloc(pool->rels, ((pool->nrels + REL_BLOCK) & ~REL_BLOCK) * sizeof(Reldep));
}

// reset all hash tables
// 
void
pool_freeidhashes(Pool *pool)
{
  pool->ss.stringhashtbl = xfree(pool->ss.stringhashtbl);
  pool->ss.stringhashmask = 0;
  pool->relhashtbl = xfree(pool->relhashtbl);
  pool->relhashmask = 0;
}

// EOF
