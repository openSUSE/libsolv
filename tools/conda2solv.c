/*
 * Copyright (c) 2019, SUSE LLC
 *
 * This program is licensed under the BSD license, read LICENSE.BSD
 * for further information
 */

/*
 * conda2solv.c
 *
 * parse a conda repository file
 *
 * reads from stdin
 * writes to stdout
 */

#include <sys/types.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#include "pool.h"
#include "repo.h"
#include "repo_conda.h"
#include "solv_xfopen.h"
#include "common_write.h"


static void
usage(int status)
{
  fprintf(stderr, "\nUsage:\n"
          "conda2solv\n"
          "  reads a 'synthesis' repository from <stdin> and writes a .solv file to <stdout>\n"
          "  -h : print help & exit\n"
         );
   exit(status);
}

int
main(int argc, char **argv)
{
  Pool *pool;
  Repo *repo;
  int c;

  while ((c = getopt(argc, argv, "h")) >= 0)
    {
      switch(c)
	{
	case 'h':
	  usage(0);
	  break;
	default:
	  usage(1);
	  break;
	}
    }
  pool = pool_create();
  repo = repo_create(pool, "<stdin>");
  if (repo_add_conda(repo, stdin, 0))
    {
      fprintf(stderr, "conda2solv: %s\n", pool_errstr(pool));
      exit(1);
    }
  repo_internalize(repo);
  tool_write(repo, stdout);
  pool_free(pool);
  exit(0);
}
