/*
 * Copyright (c) 2007, Novell Inc.
 *
 * This program is licensed under the BSD license, read LICENSE.BSD
 * for further information
 */

/*
 * rpms2solv - create a solv file from multiple rpms
 * 
 */

#include <sys/types.h>
#include <stdio.h>
#include <stdlib.h>
#include <unistd.h>
#include <string.h>

#include "util.h"
#include "pool.h"
#include "repo.h"
#include "repo_rpmdb.h"
#include "repo_solv.h"
#include "common_write.h"

int
main(int argc, char **argv)
{
  const char **rpms = 0;
  char *manifest = 0;
  int c, nrpms = 0;
  Pool *pool = pool_create();
  Repo *repo;
  FILE *fp;
  char buf[4096], *p;
  const char *basefile = 0;

  while ((c = getopt(argc, argv, "b:m:")) >= 0)
    {
      switch(c)
	{
	case 'b':
	  basefile = optarg;
	  break;
	case 'm':
	  manifest = optarg;
	  break;
	default:
	  exit(1);
	}
    }
  if (manifest)
    {
      if ((fp = fopen(manifest, "r")) == 0)
	{
	  perror(manifest);
	  exit(1);
	}
      while(fgets(buf, sizeof(buf), fp))
	{
	  if ((p = strchr(buf, '\n')) != 0)
	    *p = 0;
          rpms = sat_extend(rpms, nrpms, 1, sizeof(char *), 15);
	  rpms[nrpms++] = strdup(buf);
	}
      fclose(fp);
    }
  while (optind < argc)
    {
      rpms = sat_extend(rpms, nrpms, 1, sizeof(char *), 15);
      rpms[nrpms++] = strdup(argv[optind++]);
    }
  repo = repo_create(pool, "rpms2solv");
  repo_add_rpms(repo, rpms, nrpms);
  tool_write(repo, basefile, 0);
  pool_free(pool);
  for (c = 0; c < nrpms; c++)
    free((char *)rpms[c]);
  sat_free(rpms);
  exit(0);
}

