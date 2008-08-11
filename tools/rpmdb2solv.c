/*
 * Copyright (c) 2007, Novell Inc.
 *
 * This program is licensed under the BSD license, read LICENSE.BSD
 * for further information
 */

/*
 * rpmdb2solv
 * 
 * Reads rpm database (and evtl. more, like product metadata) to build
 * a .solv file of 'installed' solvables.
 * 
 */

#include <sys/types.h>
#include <limits.h>
#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#include "pool.h"
#include "repo.h"
#include "repo_rpmdb.h"
#include "repo_products.h"
#include "repo_solv.h"
#include "common_write.h"

int
main(int argc, char **argv)
{
  Pool *pool = pool_create();
  Repo *repo, *ref = 0;
  FILE *fp;
  Pool *refpool;
  int c;
  int extrapool = 0;
  const char *root = 0;
  const char *basefile = 0;
  const char *proddir = 0;

  /*
   * parse arguments
   */
  
  while ((c = getopt (argc, argv, "xb:r:p:")) >= 0)
    switch (c)
      {
      case 'r':
        root = optarg;
        break;
      case 'b':
        basefile = optarg;
        break;
      case 'p':
	proddir = optarg;
	break;
      case 'x':
        extrapool = 1;
        break;
      default:
	exit(1);
      }
  
  /*
   * ???
   */
  
  if (optind < argc)
    {
      if (extrapool)
	refpool = pool_create();
      else
        refpool = pool;
      if ((fp = fopen(argv[optind], "r")) == NULL)
        {
          perror(argv[optind]);
          exit(1);
        }
      ref = repo_create(refpool, "ref");
      repo_add_solv(ref, fp);
      repo_disable_paging(ref);
      fclose(fp);
    }

  /*
   * create 'installed' repository
   * add products
   * add rpmdb
   * write .solv
   */

  repo = repo_create(pool, "installed");

  repo_add_rpmdb(repo, ref, root);

  if (proddir)
    {
      repo_add_products(repo, proddir);
    }
  if (ref)
    {
      if (ref->pool != pool)
	pool_free(ref->pool);
      else
	repo_free(ref, 1);
      ref = 0;
    }

  tool_write(repo, basefile, 0);
  pool_free(pool);

  exit(0);
}
