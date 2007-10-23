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
#include "source_solv.h"
#include "source_write.h"

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
      pool_addsource_solv(pool, fp, "");
      fclose(fp);
    }
  if (!pool->nsources)
    return 0;

  new_id_size = 0;
  for (i = 0; i < pool->nsources; i++)
    new_id_size += pool->sources[i]->idarraysize;
  new_id = (Id*) malloc (sizeof (Id) * new_id_size);
  new_id_size = 0;
  for (i = 0; i < pool->nsources; i++)
    {
      Source *source = pool->sources[i];
      int si;
      memcpy (new_id + new_id_size, source->idarraydata,
      	      source->idarraysize * sizeof (new_id[0]));
      for (si = source->start; si < source->start + source->nsolvables; si++)
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
      new_id_size += source->idarraysize;
      if (i > 0)
        {
	  pool->sources[0]->nsolvables += source->nsolvables;
	  source->nsolvables = 0;
	  source->start = pool->nsolvables;
	  free (source->idarraydata);
	  source->idarraydata = 0;
	}
    }
  while (pool->nsources > 1)
    {
      pool_freesource (pool, pool->sources[1]);
    }
  free (pool->sources[0]->idarraydata);
  pool->sources[0]->idarraydata = new_id;
  pool->sources[0]->idarraysize = new_id_size;

  pool_writesource(pool, pool->sources[0], stdout);
  pool_free(pool);

  return 0;
}
