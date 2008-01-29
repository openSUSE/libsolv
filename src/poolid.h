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

//-----------------------------------------------
// Id's with relation

typedef struct _Reldep {
  Id name;		// "package"
  Id evr;		// "0:42-3"
  int flags;		// operation/relation, see REL_x below
} Reldep;

extern Id str2id(Pool *pool, const char *, int);
extern Id strn2id(Pool *pool, const char *, unsigned int, int);
extern Id rel2id(Pool *pool, Id, Id, int, int);
extern const char *id2str(Pool *pool, Id);
extern const char *dep2str(Pool *pool, Id);
extern const char *id2rel(Pool *pool, Id);
extern const char *id2evr(Pool *pool, Id);

extern void pool_shrink_strings(Pool *pool);
extern void pool_shrink_rels(Pool *pool);
extern void pool_freeidhashes(Pool *pool);

#endif /* SATSOLVER_POOLID_H */
