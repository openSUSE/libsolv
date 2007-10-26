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
      pool_addrepo_solv(pool, fp, "");
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
      memcpy (new_id + new_id_size, repo->idarraydata,
      	      repo->idarraysize * sizeof (new_id[0]));
      for (si = repo->start; si < repo->start + repo->nsolvables; si++)
        {
	  Solvable *s = pool->solvables + si;
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
	}
      new_id_size += repo->idarraysize;
      if (i > 0)
        {
	  pool->repos[0]->nsolvables += repo->nsolvables;
	  repo->nsolvables = 0;
	  repo->start = pool->nsolvables;
	  free (repo->idarraydata);
	  repo->idarraydata = 0;
	}
    }
  while (pool->nrepos > 1)
    {
      pool_freerepo (pool, pool->repos[1]);
    }
  free (pool->repos[0]->idarraydata);
  pool->repos[0]->idarraydata = new_id;
  pool->repos[0]->idarraysize = new_id_size;

  pool_writerepo(pool, pool->repos[0], stdout);
  pool_free(pool);

  return 0;
}
