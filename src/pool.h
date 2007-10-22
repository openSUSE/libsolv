/*
 * pool.h
 * 
 */

#ifndef POOL_H
#define POOL_H

#ifdef __cplusplus
extern "C" {
#endif

#include "pooltypes.h"
#include "poolid.h"
#include "source.h"
#include "solvable.h"
#include "queue.h"

// bool
#ifndef __cplusplus
typedef _Bool bool;
#endif

// see initpool_data[] in pool.c

/* well known ids */
#define ID_NULL			0
#define ID_EMPTY		1
#define SOLVABLE_NAME		2
#define SOLVABLE_ARCH		3
#define SOLVABLE_EVR		4
#define SOLVABLE_PROVIDES	5
#define SOLVABLE_OBSOLETES	6
#define SOLVABLE_CONFLICTS	7
#define SOLVABLE_REQUIRES	8
#define SOLVABLE_RECOMMENDS	9
#define SOLVABLE_SUGGESTS	10
#define SOLVABLE_SUPPLEMENTS	11
#define SOLVABLE_ENHANCES	12
#define SOLVABLE_FRESHENS	13
#define RPM_RPMDBID		14
#define SOLVABLE_PREREQMARKER	15		// normal requires before this, prereqs after this
#define SOLVABLE_FILEMARKER	16		// normal provides before this, generated file provides after this
#define NAMESPACE_INSTALLED	17
#define NAMESPACE_MODALIAS	18
#define ARCH_SRC		19
#define ARCH_NOSRC		20
#define ARCH_NOARCH		21

//-----------------------------------------------

struct _Pool {
  int verbose;		// pool is used everywhere, so put the verbose flag here

  Offset *strings;            // table of offsets into stringspace, indexed by Id: Id -> Offset
  int nstrings;               // number of unique strings in stringspace
  char *stringspace;          // space for all unique strings: stringspace + Offset = string
  Offset sstrings;            // next free pos in stringspace

  Hashtable stringhashtbl;    // hash table: (string ->) Hash -> Id
  Hashmask stringhashmask;    // modulo value for hash table (size of table - 1)

  Reldep *rels;               // table of rels: Id -> Reldep
  int nrels;                  // number of unique rels
  Hashtable relhashtbl;       // hash table: (name,evr,op ->) Hash -> Id
  Hashmask relhashmask;

  Source **sources;
  int nsources;

  Solvable *solvables;
  int nsolvables;

  bool promoteepoch;

  Id *id2arch;			/* map arch ids to scores */
  Id lastarch;			/* last valid entry in id2arch */

  /* providers data, as two-step indirect list
   * whatprovides[Id] -> Offset into whatprovidesdata for name
   * whatprovidesdata[Offset] -> ID_NULL-terminated list of solvables providing Id
   */
  Offset *whatprovides;		/* Offset to providers of a specific name, Id -> Offset  */
  Id *whatprovidesdata;		/* Ids of solvable providing Id */
  Offset whatprovidesdataoff;	/* next free slot within whatprovidesdata */
  int whatprovidesdataleft;	/* number of 'free slots' within whatprovidesdata */
};

#define TYPE_ID			1
#define TYPE_IDARRAY		2
#define TYPE_STR		3
#define TYPE_U32		4
#define TYPE_BITMAP		128


//-----------------------------------------------

// whatprovides
//  "foo" -> Id -> lookup in table, returns offset into whatprovidesdata where array of Ids providing 'foo' are located, 0-terminated


#define GET_PROVIDESP(d, v) (ISRELDEP(d) ?				\
             (v = GETRELID(pool, d), pool->whatprovides[v] ?		\
               pool->whatprovidesdata + pool->whatprovides[v] :		\
	       pool_addrelproviders(pool, d)				\
             ) :							\
             (pool->whatprovidesdata + pool->whatprovides[d]))

/* loop over all providers of d */
#define FOR_PROVIDES(v, vp, d) 						\
  for (vp = GET_PROVIDESP(d, v) ; (v = *vp++) != ID_NULL; )

/* mark dependencies with relation by setting bit31 */

#define MAKERELDEP(id) ((id) | 0x80000000)
#define ISRELDEP(id) (((id) & 0x80000000) != 0)
#define GETRELID(pool, id) ((pool)->nstrings + ((id) ^ 0x80000000))     /* returns Id  */
#define GETRELDEP(pool, id) ((pool)->rels + ((id) ^ 0x80000000))	/* returns Reldep* */

#define REL_GT		1
#define REL_EQ		2
#define REL_LT		4

#define REL_AND		16
#define REL_OR		17
#define REL_WITH	18
#define REL_NAMESPACE	19

extern Pool *pool_create(void);
extern void pool_free(Pool *pool);
extern void pool_freesource(Pool *pool, Source *source);
extern void pool_prepare(Pool *pool);
extern void pool_freewhatprovides(Pool *pool);
extern Id pool_queuetowhatprovides(Pool *pool, Queue *q);

extern Id *pool_addrelproviders(Pool *pool, Id d);

static inline int pool_installable(Pool *pool, Solvable *s)
{
  if (s->arch == ARCH_SRC || s->arch == ARCH_NOSRC)
    return 0;
  if (pool->id2arch && (s->arch > pool->lastarch || !pool->id2arch[s->arch]))
    return 0;
  return 1;
}

#ifdef __cplusplus
}
#endif

#endif /* POOL_H */
