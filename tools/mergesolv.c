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
#include "common_write.h"

int
main(int argc, char **argv)
{
  Pool *pool = pool_create();
  Repo *repo;

  repo = repo_create(pool, "");
  while (argc-- > 1)
    {
      FILE *fp;
      argv++;
      if ((fp = fopen(*argv, "r")) == NULL)
	{
	  perror(argv[1]);
	  exit(0);
	}
      repo_add_solv(repo, fp);
      fclose(fp);
    }

  tool_write(repo, 0, 0);
  pool_free(pool);

  return 0;
}
