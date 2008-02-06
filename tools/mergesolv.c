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
#include "repo_write.h"

static char *verticals[] = {
  "authors",
  "description",
  "messagedel",
  "messageins",
  "eula",
  "diskusage",
  0
};

static unsigned char *filter;
static int nfilter;

static void
create_filter(Pool *pool)
{
  char **s;
  Id id;
  for (s = verticals; *s; s++)
    {
      id = str2id(pool, *s, 1);
      if (id >= nfilter)
	{
	  filter = sat_realloc(filter, id + 16);
	  memset(filter + nfilter, 0, id + 16 - nfilter);
	  nfilter = id + 16;
	}
      filter[id] = 1;
    }
}

static int
keyfilter(Repo *data, Repokey *key, void *kfdata)
{
  if (key->name < nfilter && filter[key->name])
    return KEY_STORAGE_VERTICAL_OFFSET;
  return KEY_STORAGE_INCORE;
}

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

  create_filter(pool);
  repo_write(repo, stdout, keyfilter, 0);
  pool_free(pool);

  return 0;
}
