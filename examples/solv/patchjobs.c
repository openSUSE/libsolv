#include <stdio.h>
#include <stdlib.h>
#include <unistd.h>

#include "pool.h"
#include "repo.h"
#include "evr.h"
#include "solver.h"

void
add_patchjobs(Pool *pool, Queue *job)
{
  Id p, pp;
  int pruneyou = 0;
  Map installedmap, multiversionmap;
  Solvable *s;

  map_init(&multiversionmap, 0);
  map_init(&installedmap, pool->nsolvables);
  solver_calculate_multiversionmap(pool, job, &multiversionmap);
  if (pool->installed)
    {
      FOR_REPO_SOLVABLES(pool->installed, p, s)
        MAPSET(&installedmap, p);
    }

  /* install all patches */
  for (p = 1; p < pool->nsolvables; p++)
    {
      const char *type;
      int r;
      Id p2;

      s = pool->solvables + p;
      if (strncmp(pool_id2str(pool, s->name), "patch:", 6) != 0)
	continue;
      FOR_PROVIDES(p2, pp, s->name)
	{
	  Solvable *s2 = pool->solvables + p2;
	  if (s2->name != s->name)
	    continue;
	  r = pool_evrcmp(pool, s->evr, s2->evr, EVRCMP_COMPARE);
	  if (r < 0 || (r == 0 && p > p2))
	    break;
	}
      if (p2)
	continue;
      type = solvable_lookup_str(s, SOLVABLE_PATCHCATEGORY);
      if (type && !strcmp(type, "optional"))
	continue;
      r = solvable_trivial_installable_map(s, &installedmap, 0, &multiversionmap);
      if (r == -1)
	continue;
      if (solvable_lookup_bool(s, UPDATE_RESTART) && r == 0)
	{
	  if (!pruneyou++)
	    queue_empty(job);
	}
      else if (pruneyou)
	continue;
      queue_push2(job, SOLVER_SOLVABLE, p);
    }
  map_free(&installedmap);
  map_free(&multiversionmap);
}
