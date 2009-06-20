#include <stdio.h>
#include <stdlib.h>

#include "pool.h"
#include "repo.h"
#include "solver.h"
#include "solverdebug.h"
#include "hash.h"
#include "repo_rpmdb.h"
#include "pool_fileconflicts.h"

static void *
iterate_handle(Pool *pool, Id p, void *cbdata)
{
  Solvable *s = pool->solvables + p;
  Id rpmdbid;
  
  if (!p)
    {
      rpm_byrpmdbid(0, 0, (void **)cbdata);
      return 0;
    }
  if (!s->repo->rpmdbid)
    return 0;
  rpmdbid = s->repo->rpmdbid[p - s->repo->start];
  if (!rpmdbid)
    return 0;
  return rpm_byrpmdbid(rpmdbid, 0, (void **)cbdata);
}

int main()
{
  Pool *pool;
  Repo *installed;
  Solvable *s;
  Id p;
  int i;
  Queue todo, conflicts;
  void *state = 0;
 
  pool = pool_create();
  pool_setdebuglevel(pool, 1);
  installed = repo_create(pool, "@System");
  pool_set_installed(pool, installed);
  repo_add_rpmdb(installed, 0, 0, 0);
  queue_init(&todo);
  queue_init(&conflicts);
  FOR_REPO_SOLVABLES(installed, p, s)
    queue_push(&todo, p);
  pool_findfileconflicts(pool, &todo, 0, &conflicts, &iterate_handle, (void *)&state);
  queue_free(&todo);
  for (i = 0; i < conflicts.count; i += 5)
    printf("%s: %s[%s] %s[%s]\n", id2str(pool, conflicts.elements[i]), solvid2str(pool, conflicts.elements[i + 1]), id2str(pool, conflicts.elements[i + 2]), solvid2str(pool, conflicts.elements[i + 3]), id2str(pool, conflicts.elements[i + 4]));
  if (conflicts.count)
    {
      Queue job;
      queue_init(&job);
      pool_add_fileconflicts_deps(pool, &conflicts);
      pool_addfileprovides(pool);
      pool_createwhatprovides(pool);
      pool_setdebuglevel(pool, 0);
      Solver *solv = solver_create(pool);
      solv->fixsystem = 1;
#if 0
      solv->allowuninstall = 1;
#endif
      solver_solve(solv, &job);
      if (solv->problems.count)
        solver_printallsolutions(solv);
      else
        solver_printtransaction(solv);
      queue_free(&job);
      solver_free(solv);
    }
  queue_free(&conflicts);
  exit(0);
}
