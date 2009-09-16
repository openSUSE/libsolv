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

static Id verticals[] = {
  SOLVABLE_AUTHORS,
  SOLVABLE_DESCRIPTION,
  SOLVABLE_MESSAGEDEL,
  SOLVABLE_MESSAGEINS,
  SOLVABLE_EULA,
  SOLVABLE_DISKUSAGE,
  SOLVABLE_FILELIST,
  0
};

static char *languagetags[] = {
  "solvable:summary:",
  "solvable:description:",
  "solvable:messageins:",
  "solvable:messagedel:",
  "solvable:eula:",
  0
};

static int test_separate = 0;

struct keyfilter_data {
  char **languages;
  int nlanguages;
  int haveaddedfileprovides;
  int haveexternal;
};

static int
keyfilter_solv(Repo *data, Repokey *key, void *kfdata)
{
  struct keyfilter_data *kd = kfdata;
  int i;
  const char *keyname;

  if (test_separate && key->storage != KEY_STORAGE_SOLVABLE)
    return KEY_STORAGE_DROPPED;
  if (!kd->haveaddedfileprovides && key->name == REPOSITORY_ADDEDFILEPROVIDES)
    return KEY_STORAGE_DROPPED;
  if (!kd->haveexternal && key->name == REPOSITORY_EXTERNAL)
    return KEY_STORAGE_DROPPED;
  for (i = 0; verticals[i]; i++)
    if (key->name == verticals[i])
      return KEY_STORAGE_VERTICAL_OFFSET;
  keyname = id2str(data->pool, key->name);
  for (i = 0; languagetags[i] != 0; i++)
    if (!strncmp(keyname, languagetags[i], strlen(languagetags[i])))
      return KEY_STORAGE_VERTICAL_OFFSET;
  return KEY_STORAGE_INCORE;
}

static int
keyfilter_attr(Repo *data, Repokey *key, void *kfdata)
{
  int i;
  const char *keyname;
  if (key->storage == KEY_STORAGE_SOLVABLE)
    return KEY_STORAGE_DROPPED;
  /* those two must only be in the main solv file */
  if (key->name == REPOSITORY_EXTERNAL || key->name == REPOSITORY_ADDEDFILEPROVIDES)
    return KEY_STORAGE_DROPPED;
  for (i = 0; verticals[i]; i++)
    if (key->name == verticals[i])
      return KEY_STORAGE_VERTICAL_OFFSET;
  keyname = id2str(data->pool, key->name);
  for (i = 0; languagetags[i] != 0; i++)
    if (!strncmp(keyname, languagetags[i], strlen(languagetags[i])))
      return KEY_STORAGE_VERTICAL_OFFSET;
  return KEY_STORAGE_INCORE;
}

static int
keyfilter_language(Repo *repo, Repokey *key, void *kfdata)
{
  Pool *pool = repo->pool;
  const char *name, *p;
  char *lang = kfdata;
  int i;

  name = id2str(repo->pool, key->name);
  p = strrchr(name, ':');
  if (!p || strcmp(p + 1, lang) != 0)
    return KEY_STORAGE_DROPPED;
  for (i = 0; verticals[i]; i++)
    {
      const char *vname = id2str(pool, verticals[i]);
      if (!strncmp(name, vname, p - name) && vname[p - name] == 0)
	return KEY_STORAGE_VERTICAL_OFFSET;
    }
  return KEY_STORAGE_INCORE;
}

static int
keyfilter_DU(Repo *repo, Repokey *key, void *kfdata)
{
  int i;
  if (key->name != SOLVABLE_DISKUSAGE)
    return KEY_STORAGE_DROPPED;
  for (i = 0; verticals[i]; i++)
    if (key->name == verticals[i])
      return KEY_STORAGE_VERTICAL_OFFSET;
  return KEY_STORAGE_INCORE;
}

static int
keyfilter_FL(Repo *repo, Repokey *key, void *kfdata)
{
  int i;
  if (key->name != SOLVABLE_FILELIST)
    return KEY_STORAGE_DROPPED;
  for (i = 0; verticals[i]; i++)
    if (key->name == verticals[i])
      return KEY_STORAGE_VERTICAL_OFFSET;
  return KEY_STORAGE_INCORE;
}

static int
keyfilter_other(Repo *repo, Repokey *key, void *kfdata)
{
  const char *name, *p;
  struct keyfilter_data *kd = kfdata;
  int i;

  if (!kd->haveaddedfileprovides && key->name == REPOSITORY_ADDEDFILEPROVIDES)
    return KEY_STORAGE_DROPPED;
  if (!kd->haveexternal && key->name == REPOSITORY_EXTERNAL)
    return KEY_STORAGE_DROPPED;

  if (key->name == SOLVABLE_FILELIST || key->name == SOLVABLE_DISKUSAGE)
    return KEY_STORAGE_DROPPED;

  name = id2str(repo->pool, key->name);
  p = strrchr(name, ':');
  if (p)
    {
      for (i = 0; i < kd->nlanguages; i++)
	if (!strcmp(p + 1, kd->languages[i]))
	  return KEY_STORAGE_DROPPED;
    }
  for (i = 0; verticals[i]; i++)
    if (key->name == verticals[i])
      return KEY_STORAGE_VERTICAL_OFFSET;
  return KEY_STORAGE_INCORE;
}

/*
 * Write <repo> to stdout
 * If <attrname> is given, write attributes to <attrname>
 * If <basename> is given, split attributes
 */

#define REPODATAFILE_BLOCK 15

static void
write_info(Repo *repo, FILE *fp, int (*keyfilter)(Repo *repo, Repokey *key, void *kfdata), void *kfdata, Repodata *info, const char *location)
{
  Id h, *keyarray = 0;
  int i;

  repo_write(repo, fp, keyfilter, kfdata, &keyarray);
  h = repodata_new_handle(info);
  if (keyarray)
    {
      for (i = 0; keyarray[i]; i++)
        repodata_add_idarray(info, h, REPOSITORY_KEYS, keyarray[i]);
    }
  sat_free(keyarray);
  repodata_set_str(info, h, REPOSITORY_LOCATION, location);
  repodata_add_flexarray(info, SOLVID_META, REPOSITORY_EXTERNAL, h);
}

int
tool_write(Repo *repo, const char *basename, const char *attrname)
{
  Repodata *data;
  Repodata *info = 0;
  Repokey *key;
  char **languages = 0;
  int nlanguages = 0;
  int i, j, k, l;
  Id *addedfileprovides = 0;
  struct keyfilter_data kd;

  memset(&kd, 0, sizeof(kd));
  info = repo_add_repodata(repo, 0);
  pool_addfileprovides_ids(repo->pool, 0, &addedfileprovides);
  if (addedfileprovides && *addedfileprovides)
    {
      kd.haveaddedfileprovides = 1;
      for (i = 0; addedfileprovides[i]; i++)
        repodata_add_idarray(info, SOLVID_META, REPOSITORY_ADDEDFILEPROVIDES, addedfileprovides[i]);
    }
  sat_free(addedfileprovides);

  pool_freeidhashes(repo->pool);	/* free some mem */

  if (basename)
    {
      char fn[4096];
      FILE *fp;
      int has_DU = 0;
      int has_FL = 0;

      /* find languages and other info */
      for (i = 0, data = repo->repodata; i < repo->nrepodata; i++, data++)
	{
	  for (j = 1, key = data->keys + j; j < data->nkeys; j++, key++)
	    {
	      const char *keyname = id2str(repo->pool, key->name);
	      if (key->name == SOLVABLE_DISKUSAGE)
		has_DU = 1;
	      if (key->name == SOLVABLE_FILELIST)
		has_FL = 1;
	      for (k = 0; languagetags[k] != 0; k++)
		if (!strncmp(keyname, languagetags[k], strlen(languagetags[k])))
		  break;
	      if (!languagetags[k])
		continue;
	      l = strlen(languagetags[k]);
	      if (strlen(keyname + l) > 5)
		continue;
	      for (k = 0; k < nlanguages; k++)
		if (!strcmp(languages[k], keyname + l))
		  break;
	      if (k < nlanguages)
		continue;
	      languages = sat_realloc2(languages, nlanguages + 1, sizeof(char *));
	      languages[nlanguages++] = strdup(keyname + l);
	    }
	}
      /* write language subfiles */
      for (i = 0; i < nlanguages; i++)
        {
	  sprintf(fn, "%s.%s.solv", basename, languages[i]);
	  if (!(fp = fopen(fn, "w")))
	    {
	      perror(fn);
	      exit(1);
	    }
	  write_info(repo, fp, keyfilter_language, languages[i], info, fn);
	  fclose(fp);
	  kd.haveexternal = 1;
        }
      /* write DU subfile */
      if (has_DU)
	{
	  sprintf(fn, "%s.DU.solv", basename);
	  if (!(fp = fopen(fn, "w")))
	    {
	      perror(fn);
	      exit(1);
	    }
	  write_info(repo, fp, keyfilter_DU, 0, info, fn);
	  fclose(fp);
	  kd.haveexternal = 1;
	}
      /* write filelist */
      if (has_FL)
	{
	  sprintf(fn, "%s.FL.solv", basename);
	  if (!(fp = fopen(fn, "w")))
	    {
	      perror(fn);
	      exit(1);
	    }
	  write_info(repo, fp, keyfilter_FL, 0, info, fn);
	  fclose(fp);
	  kd.haveexternal = 1;
	}
      /* write everything else */
      sprintf(fn, "%s.solv", basename);
      if (!(fp = fopen(fn, "w")))
	{
	  perror(fn);
	  exit(1);
	}
      kd.languages = languages;
      kd.nlanguages = nlanguages;
      repodata_internalize(info);
      repo_write(repo, fp, keyfilter_other, &kd, 0);
      fclose(fp);
      for (i = 0; i < nlanguages; i++)
	free(languages[i]);
      sat_free(languages);
      repodata_free(info);
      return 0;
    }
  if (attrname)
    {
      test_separate = 1;
      FILE *fp = fopen(attrname, "w");
      write_info(repo, fp, keyfilter_attr, 0, info, attrname);
      fclose(fp);
      kd.haveexternal = 1;
    }
  repodata_internalize(info);
  repo_write(repo, stdout, keyfilter_solv, &kd, 0);
  repodata_free(info);
  return 0;
}
