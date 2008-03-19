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
  0
};

static int test_separate = 0;

static int
keyfilter_solv(Repo *data, Repokey *key, void *kfdata)
{
  int i;
  if (test_separate && key->storage != KEY_STORAGE_SOLVABLE)
    return KEY_STORAGE_DROPPED;
  for (i = 0; verticals[i]; i++)
    if (key->name == verticals[i])
      return KEY_STORAGE_VERTICAL_OFFSET;
  return KEY_STORAGE_INCORE;
}

static int
keyfilter_attr(Repo *data, Repokey *key, void *kfdata)
{
  int i;
  if (key->storage == KEY_STORAGE_SOLVABLE)
    return KEY_STORAGE_DROPPED;
  for (i = 0; verticals[i]; i++)
    if (key->name == verticals[i])
      return KEY_STORAGE_VERTICAL_OFFSET;
  return KEY_STORAGE_INCORE;
}

static int
keyfilter_language(Repo *repo, Repokey *key, void *kfdata)
{
  const char *name, *p;
  char *lang = kfdata, *bname;
  int i;
  Id id;

  name = id2str(repo->pool, key->name);
  p = strrchr(name, ':');
  if (!p || strcmp(p + 1, lang) != 0)
    return KEY_STORAGE_DROPPED;
  /* find base name id */
  bname = strdup(name);
  bname[p - name] = 0;
  id = str2id(repo->pool, bname, 1);
  for (i = 0; verticals[i]; i++)
    if (id == verticals[i])
      return KEY_STORAGE_VERTICAL_OFFSET;
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

struct keyfilter_other_data {
  char **languages;
  int nlanguages;
};

static int
keyfilter_other(Repo *repo, Repokey *key, void *kfdata)
{
  const char *name, *p;
  struct keyfilter_other_data *kd = kfdata;
  int i;

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

int
tool_write(Repo *repo, const char *basename, const char *attrname)
{
  Repodata *data;
  Repokey *key;
  Repodatafile *fileinfos = 0;
  int nfileinfos = 0;
  char **languages = 0;
  int nlanguages = 0;
  int i, j, k, l;

  fileinfos = sat_zextend(fileinfos, nfileinfos, 1, sizeof(Repodatafile), REPODATAFILE_BLOCK);
  pool_addfileprovides_ids(repo->pool, 0, &fileinfos[nfileinfos].addedfileprovides);
  nfileinfos++;

  if (basename)
    {
      struct keyfilter_other_data kd;
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
      fileinfos = sat_zextend(fileinfos, nfileinfos, nlanguages + 2, sizeof(Repodatafile), REPODATAFILE_BLOCK);
      /* write language subfiles */
      for (i = 0; i < nlanguages; i++)
        {
	  sprintf(fn, "%s.%s.solv", basename, languages[i]);
	  if (!(fp = fopen(fn, "w")))
	    {
	      perror(fn);
	      exit(1);
	    }
          repo_write(repo, fp, keyfilter_language, languages[i], fileinfos + nfileinfos, 0);
	  fileinfos[nfileinfos].location = strdup(fn);
	  fclose(fp);
	  nfileinfos++;
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
	  repo_write(repo, fp, keyfilter_DU, 0, fileinfos + nfileinfos, 0);
	  fileinfos[nfileinfos].location = strdup(fn);
	  fclose(fp);
	  nfileinfos++;
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
	  repo_write(repo, fp, keyfilter_FL, 0, fileinfos + nfileinfos, 0);
	  fileinfos[nfileinfos].location = strdup(fn);
	  fclose(fp);
	  nfileinfos++;
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
      repo_write(repo, fp, keyfilter_other, &kd, fileinfos, nfileinfos);
      fclose(fp);
      for (i = 0; i < nlanguages; i++)
	free(languages[i]);
      sat_free(languages);
      for (i = 0; i < nfileinfos; i++)
	{
	  sat_free(fileinfos[i].addedfileprovides);
	  sat_free(fileinfos[i].location);
	  sat_free(fileinfos[i].keys);
	}
      sat_free(fileinfos);
      return 0;
    }
  if (attrname)
    {
      fileinfos = sat_zextend(fileinfos, nfileinfos, 1, sizeof(Repodatafile), REPODATAFILE_BLOCK);
      test_separate = 1;
      FILE *fp = fopen (attrname, "w");
      repo_write(repo, fp, keyfilter_attr, 0, fileinfos + nfileinfos, 0);
      fileinfos[nfileinfos].location = strdup(attrname);
      fclose(fp);
      nfileinfos++;
    }
  repo_write(repo, stdout, keyfilter_solv, 0, fileinfos, nfileinfos);
  for (i = 0; i < nfileinfos; i++)
    {
      sat_free(fileinfos[i].addedfileprovides);
      sat_free(fileinfos[i].location);
      sat_free(fileinfos[i].keys);
    }
  sat_free(fileinfos);
  return 0;
}
