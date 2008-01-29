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
#include "repo_write.h"
#if 0
#include "attr_store.h"

extern Attrstore *attr;
#endif

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
  int with_attr = 0;
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
	      default : break;
	    }
      argv++;
    }
  Pool *pool = pool_create();
  Repo *repo = repo_create(pool, "<stdin>");
  repo_add_susetags(repo, stdin, 0, with_attr);
  create_filter(pool);
  repo_write(repo, stdout, keyfilter, 0);
#if 0
  if (with_attr && attr)
    {
      FILE *fp = fopen ("test.attr", "w");
      write_attr_store (fp, attr);
      fclose (fp);
    }
#endif
  pool_free(pool);
  exit(0);
}
