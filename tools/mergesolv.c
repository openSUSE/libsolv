/*
 * Copyright (c) 2007, Novell Inc.
 *
 * This program is licensed under the BSD license, read LICENSE.BSD
 * for further information
 */

/*
 * mergesolv
 * 
 */

#include <sys/types.h>
#include <limits.h>
#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <assert.h>

#include "pool.h"
#include "repo_solv.h"
#include "repo_write.h"

int
main(int argc, char **argv)
{
  Pool *pool = pool_create();
  int i;
  int new_id_size;
  Id *new_id;

  while (argc-- > 1)
    {
      FILE *fp;
      argv++;
      if ((fp = fopen(*argv, "r")) == NULL)
	{
	  perror(argv[1]);
	  exit(0);
	}
      repo_add_solv(repo_create(pool, ""), fp);
      fclose(fp);
    }
  if (!pool->nrepos)
    return 0;

  new_id_size = 0;
  for (i = 0; i < pool->nrepos; i++)
    new_id_size += pool->repos[i]->idarraysize;
  new_id = (Id*) malloc (sizeof (Id) * new_id_size);
  new_id_size = 0;
  for (i = 0; i < pool->nrepos; i++)
    {
      Repo *repo = pool->repos[i];
      int si;
      Solvable *s;
      memcpy (new_id + new_id_size, repo->idarraydata,
      	      repo->idarraysize * sizeof (new_id[0]));
      FOR_REPO_SOLVABLES (repo, si, s)
        {
	  if (s->provides)
	    s->provides += new_id_size;
	  if (s->obsoletes)
	    s->obsoletes += new_id_size;
	  if (s->conflicts)
	    s->conflicts += new_id_size;
	  if (s->requires)
	    s->requires += new_id_size;
	  if (s->recommends)
	    s->recommends += new_id_size;
	  if (s->suggests)
	    s->suggests+= new_id_size;
	  if (s->supplements)
	    s->supplements += new_id_size;
	  if (s->enhances)
	    s->enhances += new_id_size;
	  if (s->freshens)
	    s->freshens += new_id_size;
	  if (i > 0)
	    s->repo = pool->repos[0];
	}
      new_id_size += repo->idarraysize;
      if (i > 0)
        {
	  pool->repos[0]->nsolvables += repo->nsolvables;
	  if (pool->repos[0]->start > repo->start)
	    pool->repos[0]->start = repo->start;
	  if (pool->repos[0]->end < repo->end)
	    pool->repos[0]->end = repo->end;
	  repo->nsolvables = 0;
	  repo->start = pool->nsolvables;
	  repo->end = repo->start;
	  free (repo->idarraydata);
	  repo->idarraydata = 0;
	}
    }
  while (pool->nrepos > 1)
    {
      repo_free(pool->repos[pool->nrepos - 1], 1);
    }
  free (pool->repos[0]->idarraydata);
  pool->repos[0]->idarraydata = new_id;
  pool->repos[0]->idarraysize = new_id_size;

  repo_write(pool->repos[0], stdout);
  pool_free(pool);

  return 0;
}
