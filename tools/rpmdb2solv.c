/*
 * Copyright (c) 2007, Novell Inc.
 *
 * This program is licensed under the BSD license, read LICENSE.BSD
 * for further information
 */

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
#include "repo.h"
#include "repo_rpmdb.h"
#include "repo_solv.h"
#include "repo_write.h"

int
main(int argc, char **argv)
{
  Pool *pool = pool_create();
  Repo *repo, *ref = 0;
  FILE *fp;

  if (argc != 1)
    {
      Pool *refpool = pool;
      if ((fp = fopen(argv[1], "r")) == NULL)
	{
	  perror(argv[1]);
	  exit(0);
	}
      ref = repo_create(refpool, "ref");
      repo_add_solv(ref, fp);
      fclose(fp);
    }

  repo = repo_create(pool, "installed");
  repo_add_rpmdb(repo, ref, "/");
  if (ref)
    {
      if (ref->pool != pool)
	pool_free(ref->pool);
      else
	repo_free(ref, 1);
      ref = 0;
    }

  repo_write(repo, stdout, 0, 0, 0, 0);
  pool_free(pool);

  exit(0);
}
