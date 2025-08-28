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

#define REL_BLOCK		1023	/* hashtable for relations */

static inline void
grow_whatprovides(Pool *pool, Id id)
{
  if ((id & WHATPROVIDES_BLOCK) == 0)
    {
      /* grow whatprovides array */
      pool->whatprovides = solv_realloc(pool->whatprovides, (id + (WHATPROVIDES_BLOCK + 1)) * sizeof(Offset));
      memset(pool->whatprovides + id, 0, (WHATPROVIDES_BLOCK + 1) * sizeof(Offset));
    }
  if (pool->addedfileprovides == 1)
    pool->whatprovides[id] = 1;
}

static inline void
grow_whatprovides_rel(Pool *pool, Id id)
{
  if ((id & WHATPROVIDES_BLOCK) == 0)
    {
      pool->whatprovides_rel = solv_realloc2(pool->whatprovides_rel, id + (WHATPROVIDES_BLOCK + 1), sizeof(Offset));
      memset(pool->whatprovides_rel + id, 0, (WHATPROVIDES_BLOCK + 1) * sizeof(Offset));
    }
}

/* intern string into pool, return id */

Id
pool_str2id(Pool *pool, const char *str, int create)
{
  int oldnstrings = pool->ss.nstrings;
  Id id = stringpool_str2id(&pool->ss, str, create);
  if (create && pool->whatprovides && oldnstrings != pool->ss.nstrings)
    grow_whatprovides(pool, id);
  return id;
}

Id
pool_strn2id(Pool *pool, const char *str, size_t len, int create)
{
  int oldnstrings = pool->ss.nstrings;
  Id id = stringpool_strn2id(&pool->ss, str, len, create);
  if (create && pool->whatprovides && oldnstrings != pool->ss.nstrings)
    grow_whatprovides(pool, id);
  return id;
}

void
pool_init_rels(Pool *pool)
{
  /* alloc space for RelDep 0 */
  pool->rels = solv_extend_resize(0, 1, sizeof(Reldep), REL_BLOCK);
  pool->nrels = 1;
  memset(pool->rels, 0, sizeof(Reldep));
}

void
pool_resize_rels_hash(Pool *pool, int numnew)
{
  Hashval h, hh, hashmask;
  Hashtable hashtbl;
  int i;
  Reldep *rd;

  if (numnew <= 0)
    return;
  hashmask = mkmask(pool->nrels + numnew);
  if (hashmask <= pool->relhashmask)
    return;    /* same as before */

  /* realloc hash table */
  pool->relhashmask = hashmask;
  solv_free(pool->relhashtbl);
  pool->relhashtbl = hashtbl = solv_calloc(hashmask + 1, sizeof(Id));

  /* rehash all rels into new hashtable */
  for (i = 1, rd = pool->rels + i; i < pool->nrels; i++, rd++)
    {
      h = relhash(rd->name, rd->evr, rd->flags) & hashmask;
      hh = HASHCHAIN_START;
      while (hashtbl[h])
	h = HASHCHAIN_NEXT(h, hh, hashmask);
      hashtbl[h] = i;
    }
}

Id
pool_rel2id(Pool *pool, Id name, Id evr, int flags, int create)
{
  Hashval h, hh, hashmask;
  Id id;
  Hashtable hashtbl;
  Reldep *ran;


  /* extend hashtable if needed */
  hashmask = pool->relhashmask;
  if ((Hashval)pool->nrels * 2 >= hashmask)
    {
      pool_resize_rels_hash(pool, REL_BLOCK);
      hashmask = pool->relhashmask;
    }
  hashtbl = pool->relhashtbl;

  /* compute hash and check for match */
  h = relhash(name, evr, flags) & hashmask;
  hh = HASHCHAIN_START;
  ran = pool->rels;
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

  id = pool->nrels++;
  /* extend rel space if needed */
  pool->rels = solv_extend(pool->rels, id, 1, sizeof(Reldep), REL_BLOCK);
  hashtbl[h] = id;
  ran = pool->rels + id;
  ran->name = name;
  ran->evr = evr;
  ran->flags = flags;

  /* extend whatprovides_rel if needed */
  if (pool->whatprovides_rel)
    grow_whatprovides_rel(pool, id);
  return MAKERELDEP(id);
}


/* Id -> String
 * for rels (returns name only) and strings
 */
const char *
pool_id2str(const Pool *pool, Id id)
{
  while (ISRELDEP(id))
    {
      Reldep *rd = GETRELDEP(pool, id);
      id = rd->name;
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

#if defined(MULTI_SEMANTICS)
static const char *rels_nospace[] = {
  "!",
  ">",
  "=",
  ">=",
  "<",
  "<>",
  "<=",
  "<=>"
};
#endif


/* get operator for RelId */
const char *
pool_id2rel(const Pool *pool, Id id)
{
  Reldep *rd;
  if (!ISRELDEP(id))
    return "";
  rd = GETRELDEP(pool, id);

#if defined(MULTI_SEMANTICS)
  if (pool->disttype == DISTTYPE_APK && rd->flags >= 1 && rd->flags <= 7)
    return rels_nospace[rd->flags];
#endif

  switch (rd->flags)
    {
    /* debian special cases < and > */
    /* haiku special cases <> (maybe we should use != for the others as well */
    case 0: case REL_EQ: case REL_GT | REL_EQ:
    case REL_LT | REL_EQ: case REL_LT | REL_EQ | REL_GT:
#if !defined(DEBIAN) && !defined(MULTI_SEMANTICS)
    case REL_LT: case REL_GT:
#endif
#if !defined(HAIKU) && !defined(MULTI_SEMANTICS)
    case REL_LT | REL_GT:
#endif
      return rels[rd->flags];
#if defined(DEBIAN) || defined(MULTI_SEMANTICS)
    case REL_GT:
      return pool->disttype == DISTTYPE_DEB ? " >> " : rels[rd->flags];
    case REL_LT:
      return pool->disttype == DISTTYPE_DEB ? " << " : rels[rd->flags];
#endif
#if defined(HAIKU) || defined(MULTI_SEMANTICS)
    case REL_LT | REL_GT:
      return pool->disttype == DISTTYPE_HAIKU ? " != " : rels[rd->flags];
#endif
    case REL_AND:
      return pool->disttype == DISTTYPE_RPM ? " and " : " & ";
    case REL_OR:
      return pool->disttype == DISTTYPE_RPM ? " or " : " | ";
    case REL_WITH:
      return pool->disttype == DISTTYPE_RPM ? " with " : " + ";
    case REL_WITHOUT:
      return pool->disttype == DISTTYPE_RPM ? " without " : " - ";
    case REL_NAMESPACE:
      return " NAMESPACE ";	/* actually not used in dep2str */
    case REL_ARCH:
      return ".";
    case REL_MULTIARCH:
      return ":";
    case REL_FILECONFLICT:
      return " FILECONFLICT ";
    case REL_COND:
      return pool->disttype == DISTTYPE_RPM ? " if " : " IF ";
    case REL_UNLESS:
      return pool->disttype == DISTTYPE_RPM ? " unless " : " UNLESS ";
    case REL_COMPAT:
      return " compat >= ";
    case REL_KIND:
      return " KIND ";
    case REL_ELSE:
      return pool->disttype == DISTTYPE_RPM ? " else " : " ELSE ";
    case REL_CONDA:
      return " ";
    case REL_ERROR:
      return " ERROR ";
    default:
      break;
    }
  return " ??? ";
}


/* get e:v.r for Id */
const char *
pool_id2evr(const Pool *pool, Id id)
{
  Reldep *rd;
  if (!ISRELDEP(id))
    return "";
  rd = GETRELDEP(pool, id);
  if (ISRELDEP(rd->evr))
    return "(REL)";
  return pool->ss.stringspace + pool->ss.strings[rd->evr];
}

static int
dep2strlen(const Pool *pool, Id id)
{
  int l = 0;

  while (ISRELDEP(id))
    {
      Reldep *rd = GETRELDEP(pool, id);
      /* add 2 for parens */
      l += 2 + dep2strlen(pool, rd->name) + strlen(pool_id2rel(pool, id));
      id = rd->evr;
    }
  return l + strlen(pool->ss.stringspace + pool->ss.strings[id]);
}

static void
dep2strcpy(const Pool *pool, char *p, Id id, int oldrel)
{
  while (ISRELDEP(id))
    {
      Reldep *rd = GETRELDEP(pool, id);
      int rel = rd->flags;
      if (oldrel == REL_AND || oldrel == REL_OR || oldrel == REL_WITH || oldrel == REL_WITHOUT || oldrel == REL_COND || oldrel == REL_UNLESS || oldrel == REL_ELSE || oldrel == -1)
	if (rel == REL_AND || rel == REL_OR || rel == REL_WITH || rel == REL_WITHOUT || rel == REL_COND || rel == REL_UNLESS || rel == REL_ELSE)
	  if ((oldrel != rel || rel == REL_COND || rel == REL_UNLESS || rel == REL_ELSE) && !((oldrel == REL_COND || oldrel == REL_UNLESS) && rel == REL_ELSE))
	    {
	      *p++ = '(';
	      dep2strcpy(pool, p, rd->name, rd->flags);
	      p += strlen(p);
	      strcpy(p, pool_id2rel(pool, id));
	      p += strlen(p);
	      dep2strcpy(pool, p, rd->evr, rd->flags);
	      strcat(p, ")");
	      return;
	    }
      if (rd->flags == REL_KIND)
	{
	  dep2strcpy(pool, p, rd->evr, rd->flags);
	  p += strlen(p);
	  *p++ = ':';
	  id = rd->name;
	  oldrel = rd->flags;
	  continue;
	}
      dep2strcpy(pool, p, rd->name, rd->flags);
      p += strlen(p);
      if (rd->flags == REL_NAMESPACE)
	{
	  *p++ = '(';
	  dep2strcpy(pool, p, rd->evr, rd->flags);
	  strcat(p, ")");
	  return;
	}
      if (rd->flags == REL_FILECONFLICT)
	{
	  *p = 0;
	  return;
	}
      strcpy(p, pool_id2rel(pool, id));
      p += strlen(p);
#if defined(MULTI_SEMANTICS)
      if (pool->disttype == DISTTYPE_APK && (rd->flags == 2 || rd->flags == 3 || rd->flags == 6) && !ISRELDEP(rd->evr) && pool->ss.stringspace[pool->ss.strings[rd->evr]] == '~')
        p--;
#endif
      id = rd->evr;
      oldrel = rd->flags;
    }
  strcpy(p, pool->ss.stringspace + pool->ss.strings[id]);
}

const char *
pool_dep2str(Pool *pool, Id id)
{
  char *p;
  if (!ISRELDEP(id))
    return pool->ss.stringspace + pool->ss.strings[id];
  p = pool_alloctmpspace(pool, dep2strlen(pool, id) + 1);
  dep2strcpy(pool, p, id, pool->disttype == DISTTYPE_RPM ? -1 : 0);
  return p;
}

static void
pool_free_rels_hash(Pool *pool)
{
  pool->relhashtbl = solv_free(pool->relhashtbl);
  pool->relhashmask = 0;
}

void
pool_shrink_strings(Pool *pool)
{
  /* free excessive big hashes */
  if (pool->ss.stringhashmask && pool->ss.stringhashmask > mkmask(pool->ss.nstrings + 8192))
    stringpool_freehash(&pool->ss);
  stringpool_shrink(&pool->ss);
}

void
pool_shrink_rels(Pool *pool)
{
  /* free excessive big hashes */
  if (pool->relhashmask && pool->relhashmask > mkmask(pool->nrels + 4096))
    pool_free_rels_hash(pool);
  pool->rels = solv_extend_resize(pool->rels, pool->nrels, sizeof(Reldep), REL_BLOCK);
}

/* free all hash tables */
void
pool_freeidhashes(Pool *pool)
{
  stringpool_freehash(&pool->ss);
  pool_free_rels_hash(pool);
}

/* EOF */
