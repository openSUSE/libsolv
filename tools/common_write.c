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
#include "repo_write.h"
#include "common_write.h"

static char *verticals[] = {
  "authors",
  "description",
  "messagedel",
  "messageins",
  "eula",
  "diskusage",
  "filelist",
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

static int test_separate = 0;

static int
keyfilter_solv(Repo *data, Repokey *key, void *kfdata)
{
  if (test_separate && key->storage != KEY_STORAGE_SOLVABLE)
    return KEY_STORAGE_DROPPED;
  if (key->name < nfilter && filter[key->name])
    return KEY_STORAGE_VERTICAL_OFFSET;
  return KEY_STORAGE_INCORE;
}

static int
keyfilter_attr(Repo *data, Repokey *key, void *kfdata)
{
  if (key->storage == KEY_STORAGE_SOLVABLE)
    return KEY_STORAGE_DROPPED;
  if (key->name < nfilter && filter[key->name])
    return KEY_STORAGE_VERTICAL_OFFSET;
  return KEY_STORAGE_INCORE;
}

/*
 * Write <repo> to stdout
 * If <attrname> is given, write attributes to <attrname>
 */

int
tool_write(Repo *repo, const char *basename, const char *attrname)
{
  Pool *pool = repo->pool;
  Repodatafile fileinfoa[1];
  Repodatafile *fileinfo = 0;
  int nsubfiles = 0;

  create_filter(pool);
  memset (fileinfoa, 0, sizeof fileinfoa);
  if (attrname)
    {
      test_separate = 1;
      fileinfo = fileinfoa;
      FILE *fp = fopen (attrname, "w");
      repo_write(repo, fp, keyfilter_attr, 0, fileinfo, 0);
      fclose (fp);
      fileinfo->location = strdup (attrname);
      fileinfo++;

      nsubfiles = fileinfo - fileinfoa;
      fileinfo = fileinfoa;
    }
  repo_write(repo, stdout, keyfilter_solv, 0, fileinfo, nsubfiles);
  sat_free(filter);
  return 0;
}
