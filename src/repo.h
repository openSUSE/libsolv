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
#include "attr_store.h"

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

struct _Repo;

typedef struct _Repodata {
  struct _Repo *repo;		/* back pointer to repo */

  int start;			/* start of solvables this repodata is valid for */
  int end;			/* last solvable + 1 of this repodata */

  /* Keys provided by this attribute store, sorted by name value.
     The same keys may be provided by multiple attribute stores, but
     then only for different solvables.  I.e. the relation
       (solvable,name) -> store
     has to be injective.  */

  Repokey *keys;		/* keys, first entry is always zero */
  unsigned int nkeys;		/* length of keys array */

  Id *schemata;			/* schema -> offset into schemadata */
  unsigned int nschemata;	/* number of schemata */

  Id *schemadata;		/* schema storage */

  unsigned char *entryschemau8;	/* schema for entry */
  Id *entryschema;		/* schema for entry */

  unsigned char *incoredata;	/* in-core data */
  unsigned int incoredatalen;	/* data len */
  unsigned int incoredatafree;	/* data len */

  Id *incoreoffset;		/* offset for all entries */

  FILE *fp;			/* for paged access */
  Id verticaloffset;		/* offset of verticals */

  char *strbuf;
  int strbuflen;


  /* The attribute store itself.  */
  Attrstore *s;
  /* A filename where to find this attribute store, or where to store
     it.  May be "", in which case we can't load it on demand or store
     into it.  It may also be NULL for at most one of the repodata per
     repo, in which case these are the embedded attributes.  */

  const char *location;
  /* The SHA1 checksum of the file.  */
  unsigned char checksum[20];
} Repodata;


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

  /* The attribute stores we know about.  */
  Repodata *repodata;
  /* Number of attribute stores..  */
  unsigned nrepodata;
} Repo;

extern Repo *repo_create(Pool *pool, const char *name);
extern void repo_free(Repo *repo, int reuseids);
extern void repo_freeallrepos(Pool *pool, int reuseids);

extern Offset repo_addid(Repo *repo, Offset olddeps, Id id);
extern Offset repo_addid_dep(Repo *repo, Offset olddeps, Id id, int isreq);
extern Offset repo_reserve_ids(Repo *repo, Offset olddeps, int num);
extern Offset repo_fix_legacy(Repo *repo, Offset provides, Offset supplements);

extern void repo_add_attrstore (Repo *repo, Attrstore *s, const char *location);

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

#endif /* SATSOLVER_REPO_H */
