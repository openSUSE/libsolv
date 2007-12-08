/*
 * Copyright (c) 2007, Novell Inc.
 *
 * This program is licensed under the BSD license, read LICENSE.BSD
 * for further information
 */

#include <stdio.h>
#include <stdlib.h>
#include <unistd.h>
#include <string.h>

#include "pool.h"
#include "repo_solv.h"
#include "repo_write.h"
#include "attr_store.h"
#include "attr_store_p.h"

int
main(int argc, char **argv)
{
  Repo *repo;
  Pool *pool;
  int i;
  FILE *fp;

  if (argc == 1)
    {
      printf ("%s repo.solv one.attr [more.attr ...] > out.solv\n", argv[0]);
      exit (1);
    }
  if ((fp = fopen (argv[1], "r")) == 0)
    {
      perror (argv[1]);
      exit (1);
    }
  pool = pool_create ();
  repo = repo_create (pool, argv[1]);
  repo_add_solv (repo, fp);
  fclose (fp);
  for (i = 2; i < argc; i++)
    {
      if ((fp = fopen (argv[i], "r")) == 0)
        {
          perror (argv[1]);
	  exit (1);
        }
      Attrstore *s = attr_store_read (fp, pool);
      fclose (fp);
      /* XXX We should probably use the basename here.
         And calculate the SHA1 sum of the file and store it.  */
      repo_add_attrstore (repo, s, argv[i]);
    }
  repo_write(repo, stdout);
  pool_free(pool);
  exit(0);
}
