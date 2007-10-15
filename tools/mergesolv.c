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

static void
adjust (Id **val, Id * new_id, Source *source)
{
  if (!*val)
    return;
  assert (source->idarraydata <= *val);
  assert (*val < source->idarraydata + source->idarraysize);
  *val = new_id + (*val - source->idarraydata);
}

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
	  adjust (&s->provides, new_id + new_id_size, source);
	  adjust (&s->obsoletes, new_id + new_id_size, source);
	  adjust (&s->conflicts, new_id + new_id_size, source);
	  adjust (&s->requires, new_id + new_id_size, source);
	  adjust (&s->recommends, new_id + new_id_size, source);
	  adjust (&s->suggests, new_id + new_id_size, source);
	  adjust (&s->supplements, new_id + new_id_size, source);
	  adjust (&s->enhances, new_id + new_id_size, source);
	  adjust (&s->freshens, new_id + new_id_size, source);
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
