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
#if 0
#include "attr_store.h"
#endif
#include "repodata.h"

typedef struct _Repokey {
  Id name;
  Id type;
  Id size;
  Id storage;
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

  Id *rpmdbid;			/* hmm, go to repodata? */

  Repodata *repodata;		/* our stores for non-solvable related data */
  unsigned nrepodata;		/* number of our stores..  */
} Repo;

extern Repo *repo_create(Pool *pool, const char *name);
extern void repo_free(Repo *repo, int reuseids);
extern void repo_freeallrepos(Pool *pool, int reuseids);

extern Offset repo_addid(Repo *repo, Offset olddeps, Id id);
extern Offset repo_addid_dep(Repo *repo, Offset olddeps, Id id, Id marker);
extern Offset repo_reserve_ids(Repo *repo, Offset olddeps, int num);
extern Offset repo_fix_legacy(Repo *repo, Offset provides, Offset supplements);

#if 0
extern void repo_add_attrstore (Repo *repo, Attrstore *s, const char *location);
#endif

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
  for (p = (r)->start, s = (r)->pool->solvables + p; p < (r)->end; p++, s++)	\
    if (s->repo == (r))


/* search callback values */

#define SEARCH_NEXT_KEY         1
#define SEARCH_NEXT_SOLVABLE    2
#define SEARCH_STOP             3

typedef struct _KeyValue {
  Id id;
  const char *str;
  int num;
  int num2;
  int eof;
} KeyValue;

/* search flags */
#define SEARCH_STRINGMASK	15
#define SEARCH_STRING		1
#define SEARCH_SUBSTRING	2
#define SEARCH_GLOB 		3
#define SEARCH_REGEX 		4

#define	SEARCH_NOCASE			(1<<8)
#define	SEARCH_NO_STORAGE_SOLVABLE	(1<<9)

Repodata *repo_add_repodata(Repo *repo);
void repo_search(Repo *repo, Id p, Id key, const char *match, int flags, int (*callback)(void *cbdata, Solvable *s, Repodata *data, Repokey *key, KeyValue *kv), void *cbdata);

/* returns the string value of the attribute, or NULL if not found */
const char * repo_lookup_str(Solvable *s, Id key);
/* returns the string value of the attribute, or 0 if not found */
int repo_lookup_num(Solvable *s, Id key);

void repo_set_id(Repo *repo, Id p, Id keyname, Id id);
void repo_set_num(Repo *repo, Id p, Id keyname, Id num);
void repo_set_str(Repo *repo, Id p, Id keyname, const char *str);
void repo_set_poolstr(Repo *repo, Id p, Id keyname, const char *str);
void repo_internalize(Repo *repo);

#endif /* SATSOLVER_REPO_H */
