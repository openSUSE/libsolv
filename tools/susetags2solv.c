/*
 * Copyright (c) 2007, Novell Inc.
 *
 * This program is licensed under the BSD license, read LICENSE.BSD
 * for further information
 */

#include <sys/types.h>
#include <limits.h>
#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "pool.h"
#include "repo.h"
#include "repo_susetags.h"
#include "repo_content.h"
#include "common_write.h"

int
main(int argc, char **argv)
{
  int with_attr = 0;
  int test_separate = 0;
  const char *contentfile = 0;
  Id vendor = 0;
  argv++;
  argc--;
  while (argc--)
    {
      const char *s = argv[0];
      if (*s++ == '-')
        while (*s)
          switch (*s++)
	    {
	      case 'a': with_attr = 1; break;
	      case 's': test_separate = 1; break;
	      case 'c':
	        if (argc)
		  {
		    contentfile = argv[1];
		    argv++;
		    argc--;
		  }
		break;
	      default : break;
	    }
      argv++;
    }
  Pool *pool = pool_create();
  Repo *repo = repo_create(pool, "<stdin>");
  if (contentfile)
    {
      FILE *fp = fopen (contentfile, "r");
      if (!fp)
        {
	  perror("opening content file");
	  exit (1);
	}
      repo_add_content(repo, fp);
      if (!strncmp (id2str(pool, pool->solvables[repo->start].name), "product:", 8))
        vendor = pool->solvables[repo->start].vendor;
      fclose (fp);
    }
  repo_add_susetags(repo, stdin, vendor, with_attr);
  tool_write(repo, 0, with_attr && test_separate);
  pool_free(pool);
  exit(0);
}
