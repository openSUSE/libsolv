/*
 * Copyright (c) 2026 SUSE LLC
 *
 * This program is licensed under the BSD license, read LICENSE.BSD
 * for further information
 */

/*
 * poollang.c
 *
 * handle language dependent lookups
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "pool.h"
#include "util.h"

static void
pool_free_languages(Pool *pool)
{
  int i;

  pool->languagecache = solv_free(pool->languagecache);
  for (i = 0; i < pool->nlanguages; i++)
    free((char *)pool->languages[i]);
  pool->languages = solv_free((void *)pool->languages);
}

void
pool_set_languages(Pool *pool, const char **languages, int nlanguages)
{
  int i;

  pool_free_languages(pool);
  if (nlanguages < 0 || nlanguages >= 1024)
    solv_ovfl("language count overflow");
  pool->nlanguages = nlanguages;
  if (!nlanguages)
    return;
  pool->languages = solv_calloc(nlanguages, sizeof(const char **));
  for (i = 0; i < pool->nlanguages; i++)
    pool->languages[i] = solv_strdup(languages[i]);
}

Id
pool_id2langid(Pool *pool, Id id, const char *lang, int create)
{
  const char *n;
  char buf[256], *p;
  size_t l;

  if (!lang || !*lang)
    return id;
  n = pool_id2str(pool, id);
  l = strlen(n) + strlen(lang) + 2;
  if (l > sizeof(buf))
    p = solv_malloc(l);
  else
    p = buf;
  sprintf(p, "%s:%s", n, lang);
  id = pool_str2id(pool, p, create);
  if (p != buf)
    free(p);
  return id;
}

/* 
 * The languagecache is used to cache the result of pool_id2langid().
 * It returns space for nlanguages Id elements for a given keyname.
 */
Id *
pool_lookup_languagecache_row(Pool *pool, Id keyname)
{
  int cols = pool->nlanguages + 1;
  Id *row;
  if (!pool->languagecache)
    {
      pool->languagecache = solv_calloc(ID_NUM_INTERNAL + cols + 1, sizeof(Id));
      pool->languagecache[0] = ID_NUM_INTERNAL;		/* current size */
    }
  if (keyname > 0 && keyname < ID_NUM_INTERNAL)
    {
      if (cols == 2)
	return pool->languagecache + keyname;		/* special case for just one language */
      if (pool->languagecache[keyname])
	return pool->languagecache + pool->languagecache[keyname];
    }
  else
    {
      /* find our row, terminate if we reach the trailing zero */
      for (row = pool->languagecache + ID_NUM_INTERNAL; *row; row += cols)
	if (*row == keyname)
	  return row + 1;
    }
  /* we need to add a new row (plus the trailing zero) */
  if (pool->languagecache[0] + cols + 1 >= SOLV_MAX_INDEX)
    solv_ovfl("languagecache size overflow");
  pool->languagecache = solv_realloc2(pool->languagecache, pool->languagecache[0] + cols + 1, sizeof(Id));
  if (keyname < ID_NUM_INTERNAL)
    pool->languagecache[keyname] = pool->languagecache[0] + 1;
  row = pool->languagecache + pool->languagecache[0];
  pool->languagecache[0] += cols;
  memset(row, 0, (cols + 1) * sizeof(Id));	/* +1 for the trailing zero */
  *row = keyname;
  return row + 1;
}
