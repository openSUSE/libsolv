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

#ifndef REPO_H
#define REPO_H

#include "pooltypes.h"
#include "pool.h"

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

  Id *rpmdbid;
} Repo;

extern Repo *repo_create(Pool *pool, const char *name);
extern void repo_free(Repo *repo, int reuseids);

extern Offset repo_addid(Repo *repo, Offset olddeps, Id id);
extern Offset repo_addid_dep(Repo *repo, Offset olddeps, Id id, int isreq);
extern Offset repo_reserve_ids(Repo *repo, Offset olddeps, int num);
extern Offset repo_fix_legacy(Repo *repo, Offset provides, Offset supplements);

extern void pool_freeallrepos(Pool *pool, int reuseids);

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

#endif /* REPO_H */
