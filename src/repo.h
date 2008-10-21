/*
 * Copyright (c) 2007, Novell Inc.
 *
 * This program is licensed under the BSD license, read LICENSE.BSD
 * for further information
 */

/*
 * repo.h
 * 
 */

#ifndef SATSOLVER_REPO_H
#define SATSOLVER_REPO_H

#include "pooltypes.h"
#include "pool.h"
#include "repodata.h"

typedef struct _Repokey {
  Id name;
  Id type;		 /* REPOKEY_TYPE_xxx */
  unsigned int size;
  unsigned int storage; /* KEY_STORAGE_xxx */
} Repokey;

#define KEY_STORAGE_DROPPED             0
#define KEY_STORAGE_SOLVABLE		1
#define KEY_STORAGE_INCORE		2
#define KEY_STORAGE_VERTICAL_OFFSET	3


typedef struct _Repo {
  const char *name;
  struct _Pool *pool;		/* pool containing repo data */

  int start;			/* start of this repo solvables within pool->solvables */
  int end;			/* last solvable + 1 of this repo */
  int nsolvables;		/* number of solvables repo is contributing to pool */

  int priority;			/* priority of this repo */

  Id *idarraydata;		/* array of metadata Ids, solvable dependencies are offsets into this array */
  int idarraysize;
  Offset lastoff;

  Id *rpmdbid;			/* solvable side data */

  Repodata *repodata;		/* our stores for non-solvable related data */
  unsigned nrepodata;		/* number of our stores..  */
} Repo;

extern Repo *repo_create(Pool *pool, const char *name);
extern void repo_free(Repo *repo, int reuseids);
extern void repo_freeallrepos(Pool *pool, int reuseids);
extern void *repo_sidedata_create(Repo *repo, size_t size);
extern void *repo_sidedata_extend(Repo *repo, void *b, size_t size, Id p, int count);

extern Offset repo_addid(Repo *repo, Offset olddeps, Id id);
extern Offset repo_addid_dep(Repo *repo, Offset olddeps, Id id, Id marker);
extern Offset repo_reserve_ids(Repo *repo, Offset olddeps, int num);
extern Offset repo_fix_supplements(Repo *repo, Offset provides, Offset supplements, Offset freshens);
extern Offset repo_fix_conflicts(Repo *repo, Offset conflicts);

static inline const char *repo_name(const Repo *repo)
{
  return repo->name;
}

static inline Id repo_add_solvable(Repo *repo)
{
  extern Id pool_add_solvable(Pool *pool);
  Id p = pool_add_solvable(repo->pool);
  if (!repo->start || repo->start == repo->end)
    {
      repo->start = p;
      repo->end = p + 1;
    }
  else
    {
      if (repo->rpmdbid)
	repo->rpmdbid = (Id *)repo_sidedata_extend(repo, repo->rpmdbid, sizeof(Id), p, 1);
      if (p < repo->start)
	repo->start = p;
      if (p + 1 > repo->end)
	repo->end = p + 1;
    }
  repo->nsolvables++;
  repo->pool->solvables[p].repo = repo;
  return p;
}

static inline Id repo_add_solvable_block(Repo *repo, int count)
{
  extern Id pool_add_solvable_block(Pool *pool, int count);
  Id p;
  Solvable *s;
  if (!count)
    return 0;
  p = pool_add_solvable_block(repo->pool, count);
  if (!repo->start || repo->start == repo->end)
    {
      repo->start = p;
      repo->end = p + count;
    }
  else
    {
      if (repo->rpmdbid)
	repo->rpmdbid = (Id *)repo_sidedata_extend(repo, repo->rpmdbid, sizeof(Id), p, count);
      if (p < repo->start)
	repo->start = p;
      if (p + count > repo->end)
	repo->end = p + count;
    }
  repo->nsolvables += count;
  for (s = repo->pool->solvables + p; count--; s++)
    s->repo = repo;
  return p;
}

static inline void repo_free_solvable_block(Repo *repo, Id start, int count, int reuseids)
{
  extern void pool_free_solvable_block(Pool *pool, Id start, int count, int reuseids);
  Solvable *s;
  int i;
  if (start + count == repo->end)
    repo->end -= count;
  repo->nsolvables -= count;
  for (s = repo->pool->solvables + start, i = count; i--; s++)
    s->repo = 0;
  pool_free_solvable_block(repo->pool, start, count, reuseids);
}

#define FOR_REPO_SOLVABLES(r, p, s)						\
  for (p = (r)->start, s = (r)->pool->solvables + p; p < (r)->end; p++, s = (r)->pool->solvables + p)	\
    if (s->repo == (r))


/* search callback values */

#define SEARCH_NEXT_KEY         1
#define SEARCH_NEXT_SOLVABLE    2
#define SEARCH_STOP             3
#define SEARCH_ENTERSUB		-1

typedef struct _KeyValue {
  Id id;
  const char *str;
  int num;
  int num2;

  int entry;	/* array entry, starts with 0 */
  int eof;	/* last entry reached */

  struct _KeyValue *parent;
} KeyValue;

/* search flags */
#define SEARCH_STRINGMASK	15
#define SEARCH_STRING		1
#define SEARCH_SUBSTRING	2
#define SEARCH_GLOB 		3
#define SEARCH_REGEX 		4
#define SEARCH_ERROR 		5

#define	SEARCH_NOCASE			(1<<8)
#define	SEARCH_NO_STORAGE_SOLVABLE	(1<<9)
#define SEARCH_SUB			(1<<10)
#define SEARCH_ARRAYSENTINEL		(1<<11)
#define SEARCH_SKIP_KIND		(1<<12)


/* By default we don't match in attributes representing filelists
   because the construction of those strings is costly.  Specify this
   flag if you want this.  In that case kv->str will contain the full
   filename (if matched of course).  */
#define SEARCH_FILES			(1<<13)

/* Internal */
#define SEARCH_THISSOLVID		(1<<31)


/* standard flags used in the repo_add functions */
#define REPO_REUSE_REPODATA		(1 << 0)
#define REPO_NO_INTERNALIZE		(1 << 1)

Repodata *repo_add_repodata(Repo *repo, int localpool);
Repodata *repo_last_repodata(Repo *repo);

void repo_search(Repo *repo, Id p, Id key, const char *match, int flags, int (*callback)(void *cbdata, Solvable *s, Repodata *data, Repokey *key, KeyValue *kv), void *cbdata);

/* returns the string value of the attribute, or NULL if not found */
const char *repo_lookup_str(Repo *repo, Id entry, Id key);
/* returns the integer value of the attribute, or 0 if not found */
unsigned int repo_lookup_num(Repo *repo, Id entry, Id key, unsigned int notfound);
Id repo_lookup_id(Repo *repo, Id entry, Id keyid);
int repo_lookup_void(Repo *repo, Id entry, Id keyid);
const unsigned char *repo_lookup_bin_checksum(Repo *repo, Id entry, Id keyid, Id *typep);


#if 0
typedef struct _Dataiterator
{
  Repodata *data;
  Id *keyp;
  unsigned char *nextkeydp;
  unsigned char *dp;
  Repokey *key;
  Repo *repo;
  Id *idp;
  const char *match;
  Id solvid;
  Id keyname;
  unsigned flags;
  unsigned state;
  KeyValue kv;
  regex_t regex;
  int regex_err;
  Id *subkeyp;
  int subnum;
  Id subschema;
} Dataiterator;
#else

typedef struct _Datamatcher {
  int flags;
  void *match;
  int error;
} Datamatcher;

typedef struct _Dataiterator
{
  int state;
  int flags;

  Pool *pool;
  Repo *repo;
  Repodata *data;

  /* data pointers */
  unsigned char *dp;
  unsigned char *ddp;
  Id *idp;
  Id *keyp;

  /* the result */
  Repokey *key;
  KeyValue kv;

  /* our matcher */
  Datamatcher matcher;

  /* iterators/filters */
  Id keyname;
  Id repodataid;
  Id solvid;
  Id repoid;

  /* recursion data */
  struct di_parent {
    KeyValue kv;
    unsigned char *dp;
    Id *keyp;
  } parents[3];
  int nparents;
  Id keynames[3 + 1];
  int nkeynames;

} Dataiterator;

#endif

int datamatcher_init(Datamatcher *ma, const char *match, int flags);
void datamatcher_free(Datamatcher *ma);
int datamatcher_match(Datamatcher *ma, const char *str);

/*
 * Dataiterator
 * 
 * Iterator like interface to 'search' functionality
 * 
 * Dataiterator is per-pool, additional filters can be applied
 * to limit the search domain. See dataiterator_init below.
 * 
 * Use these like:
 *    Dataiterator di;
 *    dataiterator_init(&di, repo->pool, repo, 0, 0, "bla", SEARCH_SUBSTRING);
 *    while (dataiterator_step(&di))
 *      dosomething(di.solvid, di.key, di.kv);
 *    dataiterator_free(&di);
 */

/*
 * Initialize dataiterator
 * 
 * di:      Pointer to Dataiterator to be initialized
 * pool:    Search domain for the iterator
 * repo:    if non-null, limit search to this repo
 * solvid:  if non-null, limit search to this solvable
 * keyname: if non-null, limit search to this keyname
 * match:   if non-null, limit search to this match
 */
int dataiterator_init(Dataiterator *di, Pool *pool, Repo *repo, Id p, Id keyname,
		       const char *match, int flags);
void dataiterator_prepend_keyname(Dataiterator *di, Id keyname);
void dataiterator_free(Dataiterator *di);
int dataiterator_step(Dataiterator *di);
void dataiterator_setpos(Dataiterator *di);
void dataiterator_setpos_parent(Dataiterator *di);
int dataiterator_match(Dataiterator *di, Datamatcher *ma);
void dataiterator_skip_attribute(Dataiterator *di);
void dataiterator_skip_solvable(Dataiterator *di);
void dataiterator_skip_repo(Dataiterator *di);
void dataiterator_jump_to_solvid(Dataiterator *di, Id solvid);
void dataiterator_jump_to_repo(Dataiterator *di, Repo *repo);
void dataiterator_entersub(Dataiterator *di);

/* to be removed ... */
int dataiterator_match_obsolete(Dataiterator *di, int flags, const void *match);

void repo_set_id(Repo *repo, Id p, Id keyname, Id id);
void repo_set_num(Repo *repo, Id p, Id keyname, Id num);
void repo_set_str(Repo *repo, Id p, Id keyname, const char *str);
void repo_set_poolstr(Repo *repo, Id p, Id keyname, const char *str);
void repo_add_poolstr_array(Repo *repo, Id p, Id keyname, const char *str);
void repo_internalize(Repo *repo);
void repo_disable_paging(Repo *repo);

#endif /* SATSOLVER_REPO_H */
