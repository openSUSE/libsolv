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
#include <unistd.h>
#include <limits.h>
#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <assert.h>

#include "pool.h"
#include "repo_solv.h"
#include "common_write.h"

static void
usage()
{
  fprintf(stderr, "\nUsage:\n"
	  "mergesolv [file] [file] [...]\n"
	  "  merges multiple solv files into one and writes it to stdout\n"
	  );
  exit(0);
}

static FILE *
loadcallback (Pool *pool, Repodata *data, void *vdata)
{
  FILE *fp = 0;
  const char *location = repodata_lookup_str(data, 0, REPOSITORY_LOCATION);
  if (location)
    {
      fprintf(stderr, "Loading SOLV file %s\n", location);
      fp = fopen (location, "r");
      if (!fp)
	perror(location);
    }
  return fp;
}

int
main(int argc, char **argv)
{
  Pool *pool;
  Repo *repo;
  const char *basefile = 0;
  int with_attr = 0;
  int c;

  pool = pool_create();
  repo = repo_create(pool, "<mergesolv>");
  
  while ((c = getopt(argc, argv, "ahb:")) >= 0)
    {
      switch (c)
      {
	case 'h':
	  usage();
	  break;
	case 'a':
	  with_attr = 1;
	  break;
	case 'b':
	  basefile = optarg;
	  break;
	default:
	  exit(1);
      }
    }
  if (with_attr)
    pool_setloadcallback(pool, loadcallback, 0);

  for (; optind < argc; optind++)
    {
      FILE *fp;
      if ((fp = fopen(argv[optind], "r")) == NULL)
	{
	  perror(argv[optind]);
	  exit(1);
	}
      repo_add_solv(repo, fp);
      fclose(fp);
    }
  tool_write(repo, basefile, 0);
  pool_free(pool);
  return 0;
}
