/*
 * repo.h
 * 
 */

#ifndef REPO_H
#define REPO_H

#include "pooltypes.h"

typedef struct _Repo {
  const char *name;
  struct _Pool *pool;		/* pool containing repo data */
  int start;			/* start of this repo solvables within pool->solvables */
  int nsolvables;		/* number of solvables repo is contributing to pool */

  int priority;			/* priority of this repo */

  Id *idarraydata;		/* array of metadata Ids, solvable dependencies are offsets into this array */
  int idarraysize;
  Offset lastoff;

  Id *rpmdbid;
} Repo;

extern Offset repo_addid(Repo *repo, Offset olddeps, Id id);
extern Offset repo_addid_dep(Repo *repo, Offset olddeps, Id id, int isreq);
extern Offset repo_reserve_ids(Repo *repo, Offset olddeps, int num);
extern Offset repo_fix_legacy(Repo *repo, Offset provides, Offset supplements);

extern Repo *pool_addrepo_empty(Pool *pool);
extern void pool_freerepo(Pool *pool, Repo *repo);

static inline const char *repo_name(const Repo *repo)
{
  return repo->name;
}

#endif /* REPO_H */
