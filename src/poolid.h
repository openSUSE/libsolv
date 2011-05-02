/*
 * Copyright (c) 2007, Novell Inc.
 *
 * This program is licensed under the BSD license, read LICENSE.BSD
 * for further information
 */

/*
 * poolid.h
 * 
 */

#ifndef SATSOLVER_POOLID_H
#define SATSOLVER_POOLID_H

#include "pooltypes.h"
#include "hash.h"

/*-----------------------------------------------
 * Ids with relation
 */

typedef struct _Reldep {
  Id name;		// "package"
  Id evr;		// "0:42-3"
  int flags;		// operation/relation, see REL_x in pool.h
} Reldep;

extern Id pool_str2id(Pool *pool, const char *, int);
extern Id pool_strn2id(Pool *pool, const char *, unsigned int, int);
extern Id pool_rel2id(Pool *pool, Id, Id, int, int);
extern const char *pool_id2str(const Pool *pool, Id);
extern const char *pool_id2rel(const Pool *pool, Id);
extern const char *pool_id2evr(const Pool *pool, Id);
extern const char *pool_dep2str(Pool *pool, Id); /* might alloc tmpspace */

extern void pool_shrink_strings(Pool *pool);
extern void pool_shrink_rels(Pool *pool);
extern void pool_freeidhashes(Pool *pool);


/* deprecated names, do not use in new code */
static inline Id str2id(Pool *pool, const char *str, int create)
{
  return pool_str2id(pool, str, create);
}
static inline Id strn2id(Pool *pool, const char *str, unsigned int len, int create)
{
  return pool_strn2id(pool, str, len, create);
}
static inline Id rel2id(Pool *pool, Id name, Id evr, int flags, int create)
{
  return pool_rel2id(pool, name, evr, flags, create);
}
static inline const char *id2str(const Pool *pool, Id id)
{
  return pool_id2str(pool, id);
}
static inline const char *id2rel(const Pool *pool, Id id)
{
  return pool_id2rel(pool, id);
}
static inline const char *id2evr(const Pool *pool, Id id)
{
  return pool_id2evr(pool, id);
}
static inline const char *dep2str(Pool *pool, Id id)
{
  return pool_dep2str(pool, id);
}

#endif /* SATSOLVER_POOLID_H */
