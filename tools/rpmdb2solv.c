/*
 * rpmdb2solv
 * 
 */

#include <sys/types.h>
#include <limits.h>
#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "pool.h"
#include "repo_rpmdb.h"
#include "repo_solv.h"
#include "repo_write.h"

int
main(int argc, char **argv)
{
  Pool *pool = pool_create();
  Repo *ref = NULL;
  FILE *fp;

  if (argc != 1)
    {
      Pool *refpool = pool;
      if ((fp = fopen(argv[1], "r")) == NULL)
	{
	  perror(argv[1]);
	  exit(0);
	}
      ref = pool_addrepo_solv(refpool, fp, "rpmdb");
      fclose(fp);
    }

  Repo *repo = pool_addrepo_rpmdb(pool, ref);
  if (ref)
    {
      if (ref->pool != pool)
	pool_free(ref->pool);
      else
	pool_freerepo(pool, ref);
      ref = NULL;
    }

  pool_writerepo(pool, repo, stdout);
  pool_free(pool);

  exit(0);
}
