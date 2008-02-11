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
#include "common_write.h"

int
main(int argc, char **argv)
{
  int with_attr = 0;
  int test_separate = 0;
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
	      default : break;
	    }
      argv++;
    }
  Pool *pool = pool_create();
  Repo *repo = repo_create(pool, "<stdin>");
  repo_add_susetags(repo, stdin, 0, with_attr);
  tool_write(repo, 0, with_attr && test_separate);
  pool_free(pool);
  exit(0);
}
