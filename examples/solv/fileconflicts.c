#if defined(ENABLE_RPMDB) && (defined(SUSE) || defined(FEDORA) || defined(MANDRIVA) || defined(MAGEIA))

#include <stdio.h>
#include <stdlib.h>
#include <unistd.h>

#include "pool.h"
#include "repo.h"
#include "repo_rpmdb.h"
#include "pool_fileconflicts.h"

#include "fileconflicts.h"

struct fcstate {
  FILE **newpkgsfps;
  Queue *checkq;
  int newpkgscnt;
  void *rpmstate;
};

static void *
fileconflict_cb(Pool *pool, Id p, void *cbdata)
{
  struct fcstate *fcstate = cbdata;
  Solvable *s;
  Id rpmdbid;
  int i;
  FILE *fp; 

  s = pool_id2solvable(pool, p);
  if (pool->installed && s->repo == pool->installed)
    {    
      if (!s->repo->rpmdbid)
        return 0;
      rpmdbid = s->repo->rpmdbid[p - s->repo->start];
      if (!rpmdbid)
        return 0;
      return rpm_byrpmdbid(fcstate->rpmstate, rpmdbid);
    }    
  for (i = 0; i < fcstate->newpkgscnt; i++) 
    if (fcstate->checkq->elements[i] == p)
      break;
  if (i == fcstate->newpkgscnt)
    return 0;
  fp = fcstate->newpkgsfps[i];
  if (!fp)
    return 0;
  rewind(fp);
  return rpm_byfp(fcstate->rpmstate, fp, pool_solvable2str(pool, s)); 
}

int
checkfileconflicts(Pool *pool, Queue *checkq, int newpkgs, FILE **newpkgsfps, Queue *conflicts)
{
  struct fcstate fcstate;
  int i;

  printf("Searching for file conflicts\n");
  queue_init(conflicts);
  fcstate.rpmstate = rpm_state_create(pool, pool_get_rootdir(pool));
  fcstate.newpkgscnt = newpkgs;
  fcstate.checkq = checkq;
  fcstate.newpkgsfps = newpkgsfps;
  pool_findfileconflicts(pool, checkq, newpkgs, conflicts, FINDFILECONFLICTS_USE_SOLVABLEFILELIST | FINDFILECONFLICTS_CHECK_DIRALIASING | FINDFILECONFLICTS_USE_ROOTDIR, &fileconflict_cb, &fcstate);
  fcstate.rpmstate = rpm_state_free(fcstate.rpmstate);
  if (conflicts->count)
    {
      printf("\n");
      for (i = 0; i < conflicts->count; i += 6)
	printf("file %s of package %s conflicts with package %s\n", pool_id2str(pool, conflicts->elements[i]), pool_solvid2str(pool, conflicts->elements[i + 1]), pool_solvid2str(pool, conflicts->elements[i + 4]));
      printf("\n");
    }
  return conflicts->count;
}

#endif
