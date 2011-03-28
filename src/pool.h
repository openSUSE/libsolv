/*
 * Copyright (c) 2007, Novell Inc.
 *
 * This program is licensed under the BSD license, read LICENSE.BSD
 * for further information
 */

/*
 * pool.h
 * 
 */

#ifndef SATSOLVER_POOL_H
#define SATSOLVER_POOL_H

#ifdef __cplusplus
extern "C" {
#endif

#include <stdio.h>

#include "satversion.h"
#include "pooltypes.h"
#include "poolid.h"
#include "solvable.h"
#include "bitmap.h"
#include "queue.h"
#include "strpool.h"

/* well known ids */
#include "knownid.h"

/* well known solvable */
#define SYSTEMSOLVABLE		1


/* how many strings to maintain (round robin) */
#define POOL_TMPSPACEBUF 16

//-----------------------------------------------

struct _Repo;
struct _Repodata;
struct _Repokey;
struct _KeyValue;

typedef struct _Datapos {
  struct _Repo *repo;
  Id solvid;
  Id repodataid;
  Id schema;
  Id dp; 
} Datapos;

struct _Pool_tmpspace {
  char *buf[POOL_TMPSPACEBUF];
  int   len[POOL_TMPSPACEBUF];
  int   n;
};

struct _Pool {
  void *appdata;		/* application private pointer */

  struct _Stringpool ss;

  Reldep *rels;			/* table of rels: Id -> Reldep */
  int nrels;			/* number of unique rels */
  Hashtable relhashtbl;		/* hashtable: (name,evr,op)Hash -> Id */
  Hashmask relhashmask;

  struct _Repo **repos;
  int nrepos;

  struct _Repo *installed; 	/* packages considered installed */

  Solvable *solvables;
  int nsolvables;

  const char **languages;
  int nlanguages;
  Id *languagecache;
  int languagecacheother;

  /* flags to tell the library how the installed rpm works */
  int promoteepoch;		/* true: missing epoch is replaced by epoch of dependency   */
  int obsoleteusesprovides;	/* true: obsoletes are matched against provides, not names */
  int implicitobsoleteusesprovides;	/* true: implicit obsoletes due to same name are matched against provides, not names */
  int obsoleteusescolors;	/* true: obsoletes check arch color */
  int noinstalledobsoletes;	/* true: ignore obsoletes of installed packages */
  int novirtualconflicts;	/* true: conflicts on names, not on provides */
  int allowselfconflicts;	/* true: packages which conflict with itself are installable */
#ifdef MULTI_SEMANTICS
  int disttype;
#endif

  Id *id2arch;			/* map arch ids to scores */
  unsigned char *id2color;	/* map arch ids to colors */
  Id lastarch;			/* last valid entry in id2arch/id2color */

  Queue vendormap;		/* map vendor to vendorclasses mask */
  const char **vendorclasses;	/* vendor equivalence classes */

  /* providers data, as two-step indirect list
   * whatprovides[Id] -> Offset into whatprovidesdata for name
   * whatprovidesdata[Offset] -> ID_NULL-terminated list of solvables providing Id
   */
  Offset *whatprovides;		/* Offset to providers of a specific name, Id -> Offset  */
  Offset *whatprovides_rel;	/* Offset to providers of a specific relation, Id -> Offset  */

  Id *whatprovidesdata;		/* Ids of solvable providing Id */
  Offset whatprovidesdataoff;	/* next free slot within whatprovidesdata */
  int whatprovidesdataleft;	/* number of 'free slots' within whatprovidesdata */

  /* If nonzero, then consider only the solvables with Ids set in this
     bitmap for solving.  If zero, consider all solvables.  */
  Map *considered;

  Id (*nscallback)(struct _Pool *, void *data, Id name, Id evr);
  void *nscallbackdata;

  /* our tmp space string space */
  struct _Pool_tmpspace tmpspace;

  /* debug mask and callback */
  int  debugmask;
  void (*debugcallback)(struct _Pool *, void *data, int type, const char *str);
  void *debugcallbackdata;

  /* load callback */
  int (*loadcallback)(struct _Pool *, struct _Repodata *, void *);
  void *loadcallbackdata;

  /* search position */
  Datapos pos;
};

#ifdef MULTI_SEMANTICS
# define DISTTYPE_RPM	0
# define DISTTYPE_DEB	1
#endif

#define SAT_FATAL			(1<<0)
#define SAT_ERROR			(1<<1)
#define SAT_WARN			(1<<2)
#define SAT_DEBUG_STATS			(1<<3)
#define SAT_DEBUG_RULE_CREATION		(1<<4)
#define SAT_DEBUG_PROPAGATE		(1<<5)
#define SAT_DEBUG_ANALYZE		(1<<6)
#define SAT_DEBUG_UNSOLVABLE		(1<<7)
#define SAT_DEBUG_SOLUTIONS		(1<<8)
#define SAT_DEBUG_POLICY		(1<<9)
#define SAT_DEBUG_RESULT		(1<<10)
#define SAT_DEBUG_JOB			(1<<11)
#define SAT_DEBUG_SCHUBI		(1<<12)
#define SAT_DEBUG_SOLVER		(1<<13)
#define SAT_DEBUG_TRANSACTION		(1<<14)

#define SAT_DEBUG_TO_STDERR		(1<<30)

//-----------------------------------------------


/* mark dependencies with relation by setting bit31 */

#define MAKERELDEP(id) ((id) | 0x80000000)
#define ISRELDEP(id) (((id) & 0x80000000) != 0)
#define GETRELID(id) ((id) ^ 0x80000000)				/* returns Id */
#define GETRELDEP(pool, id) ((pool)->rels + ((id) ^ 0x80000000))	/* returns Reldep* */

#define REL_GT		1
#define REL_EQ		2
#define REL_LT		4

#define REL_AND		16
#define REL_OR		17
#define REL_WITH	18
#define REL_NAMESPACE	19
#define REL_ARCH	20
#define REL_FILECONFLICT	21

#if !defined(__GNUC__) && !defined(__attribute__)
# define __attribute__(x)
#endif

/**
 * Creates a new pool
 */
extern Pool *pool_create(void);
/**
 * Delete a pool
 */
extern void pool_free(Pool *pool);

extern void pool_setdebuglevel(Pool *pool, int level);
#ifdef MULTI_SEMANTICS
extern void pool_setdisttype(Pool *pool, int disttype);
#endif
extern void pool_setvendorclasses(Pool *pool, const char **vendorclasses);

extern void pool_debug(Pool *pool, int type, const char *format, ...) __attribute__((format(printf, 3, 4)));

extern char *pool_alloctmpspace(Pool *pool, int len);
extern void  pool_freetmpspace(Pool *pool, const char *space);
extern char *pool_tmpjoin(Pool *pool, const char *str1, const char *str2, const char *str3);
extern char *pool_tmpappend(Pool *pool, const char *str1, const char *str2, const char *str3);
extern const char *pool_bin2hex(Pool *pool, const unsigned char *buf, int len);

extern void pool_set_installed(Pool *pool, struct _Repo *repo);

/**
 * Solvable management
 */
extern Id pool_add_solvable(Pool *pool);
extern Id pool_add_solvable_block(Pool *pool, int count);

extern void pool_free_solvable_block(Pool *pool, Id start, int count, int reuseids);
static inline Solvable *pool_id2solvable(const Pool *pool, Id p)
{
  return pool->solvables + p;
}

extern const char *solvable2str(Pool *pool, Solvable *s);
static inline const char *solvid2str(Pool *pool, Id p)
{
  return solvable2str(pool, pool->solvables + p);
}

void pool_set_languages(Pool *pool, const char **languages, int nlanguages);
Id pool_id2langid(Pool *pool, Id id, const char *lang, int create);

Id solvable_lookup_id(Solvable *s, Id keyname);
unsigned int solvable_lookup_num(Solvable *s, Id keyname, unsigned int notfound);
const char *solvable_lookup_str(Solvable *s, Id keyname);
const char *solvable_lookup_str_poollang(Solvable *s, Id keyname);
const char *solvable_lookup_str_lang(Solvable *s, Id keyname, const char *lang, int usebase);
int solvable_lookup_bool(Solvable *s, Id keyname);
int solvable_lookup_void(Solvable *s, Id keyname);
char * solvable_get_location(Solvable *s, unsigned int *medianrp);
const unsigned char *solvable_lookup_bin_checksum(Solvable *s, Id keyname, Id *typep);
const char *solvable_lookup_checksum(Solvable *s, Id keyname, Id *typep);
int solvable_lookup_idarray(Solvable *s, Id keyname, Queue *q);
int solvable_identical(Solvable *s1, Solvable *s2);
Id solvable_selfprovidedep(Solvable *s);

int solvable_trivial_installable_map(Solvable *s, Map *installedmap, Map *conflictsmap);
int solvable_trivial_installable_repo(Solvable *s, struct _Repo *installed);
int solvable_trivial_installable_queue(Solvable *s, Queue *installed);

void pool_create_state_maps(Pool *pool, Queue *installed, Map *installedmap, Map *conflictsmap);

int pool_match_nevr_rel(Pool *pool, Solvable *s, Id d);
int pool_match_dep(Pool *pool, Id d1, Id d2);

static inline int pool_match_nevr(Pool *pool, Solvable *s, Id d)
{
  if (!ISRELDEP(d))
    return d == s->name;
  else
    return pool_match_nevr_rel(pool, s, d);
}


/**
 * Prepares a pool for solving
 */
extern void pool_createwhatprovides(Pool *pool);
extern void pool_addfileprovides(Pool *pool);
extern void pool_addfileprovides_ids(Pool *pool, struct _Repo *installed, Id **idp);
extern void pool_freewhatprovides(Pool *pool);
extern Id pool_queuetowhatprovides(Pool *pool, Queue *q);

extern Id pool_addrelproviders(Pool *pool, Id d);

static inline Id pool_whatprovides(Pool *pool, Id d)
{
  Id v;
  if (!ISRELDEP(d))
    return pool->whatprovides[d];
  v = GETRELID(d);
  if (pool->whatprovides_rel[v])
    return pool->whatprovides_rel[v];
  return pool_addrelproviders(pool, d);
}

static inline Id *pool_whatprovides_ptr(Pool *pool, Id d)
{
  Id off = pool_whatprovides(pool, d);
  return pool->whatprovidesdata + off;
}

static inline void pool_setdebugcallback(Pool *pool, void (*debugcallback)(struct _Pool *, void *data, int type, const char *str), void *debugcallbackdata)
{
  pool->debugcallback = debugcallback;
  pool->debugcallbackdata = debugcallbackdata;
}

static inline void pool_setdebugmask(Pool *pool, int mask)
{
  pool->debugmask = mask;
}

static inline void pool_setloadcallback(Pool *pool, int (*cb)(struct _Pool *, struct _Repodata *, void *), void *loadcbdata)
{
  pool->loadcallback = cb;
  pool->loadcallbackdata = loadcbdata;
}

/* search the pool. the following filters are available:
 *   p     - search just this solvable
 *   key   - search only this key
 *   match - key must match this string
 */
void pool_search(Pool *pool, Id p, Id key, const char *match, int flags, int (*callback)(void *cbdata, Solvable *s, struct _Repodata *data, struct _Repokey *key, struct _KeyValue *kv), void *cbdata);

void pool_clear_pos(Pool *pool);


typedef struct _DUChanges {
  const char *path;
  int kbytes;
  int files;
} DUChanges;

void pool_calc_duchanges(Pool *pool, Map *installedmap, DUChanges *mps, int nmps);
int pool_calc_installsizechange(Pool *pool, Map *installedmap);
void pool_trivial_installable(Pool *pool, Map *installedmap, Queue *pkgs, Queue *res);
void pool_trivial_installable_noobsoletesmap(Pool *pool, Map *installedmap, Queue *pkgs, Queue *res, Map *noobsoletesmap);

const char *pool_lookup_str(Pool *pool, Id entry, Id keyname);
Id pool_lookup_id(Pool *pool, Id entry, Id keyname);
unsigned int pool_lookup_num(Pool *pool, Id entry, Id keyname, unsigned int notfound);
int pool_lookup_void(Pool *pool, Id entry, Id keyname);
const unsigned char *pool_lookup_bin_checksum(Pool *pool, Id entry, Id keyname, Id *typep);
const char *pool_lookup_checksum(Pool *pool, Id entry, Id keyname, Id *typep);

void pool_add_fileconflicts_deps(Pool *pool, Queue *conflicts);



/* loop over all providers of d */
#define FOR_PROVIDES(v, vp, d) 						\
  for (vp = pool_whatprovides(pool, d) ; (v = pool->whatprovidesdata[vp++]) != 0; )

/* loop over all repositories */
/* note that idx is not the repoid */
#define FOR_REPOS(idx, r)						\
  for (idx = 0; idx < pool->nrepos; idx++)				\
    if ((r = pool->repos[idx]) != 0)
    

#define POOL_DEBUG(type, ...) do {if ((pool->debugmask & (type)) != 0) pool_debug(pool, (type), __VA_ARGS__);} while (0)
#define IF_POOLDEBUG(type) if ((pool->debugmask & (type)) != 0)

#ifdef __cplusplus
}
#endif


#endif /* SATSOLVER_POOL_H */
