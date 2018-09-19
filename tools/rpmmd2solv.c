/*
 * Copyright (c) 2007, Novell Inc.
 *
 * This program is licensed under the BSD license, read LICENSE.BSD
 * for further information
 */

#define _GNU_SOURCE

#include <sys/types.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#include "pool.h"
#include "repo.h"
#include "repo_rpmmd.h"
#ifdef SUSE
#include "repo_autopattern.h"
#endif
#include "common_write.h"
#include "solv_xfopen.h"


static void
usage(int status)
{
  fprintf(stderr, "\nUsage:\n"
          "rpmmd2solv [-h]\n"
	  "  reads 'primary' from a 'rpmmd' repository from <stdin> and writes a .solv file to <stdout>\n"
	  "  -h : print help & exit\n"
	 );
   exit(status);
}

int
main(int argc, char **argv)
{
  int c;
#ifdef SUSE
  int add_auto = 0;
#endif
  
  Pool *pool = pool_create();
  Repo *repo = repo_create(pool, "<stdin>");

  while ((c = getopt (argc, argv, "hX")) >= 0)
    {
      switch (c)
	{
        case 'h':
          usage(0);
          break;
	case 'X':
#ifdef SUSE
	  add_auto = 1;
#endif
	  break;
        default:
          usage(1);
          break;
	}
    }
  if (repo_add_rpmmd(repo, stdin, 0, 0))
    {
      fprintf(stderr, "rpmmd2solv: %s\n", pool_errstr(pool));
      exit(1);
    }
#ifdef SUSE
  if (add_auto)
    repo_add_autopattern(repo, 0);
#endif
  tool_write(repo, stdout);
  pool_free(pool);
  exit(0);
}
