/*
 * repo_zyppdb.c
 *
 * Parses legacy /var/lib/zypp/db/products/... files.
 * They are old (pre Code11) product descriptions. See bnc#429177
 *
 * Copyright (c) 2008, Novell Inc.
 *
 * This program is licensed under the BSD license, read LICENSE.BSD
 * for further information
 */

#include <sys/types.h>
#include <sys/stat.h>
#include <unistd.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <assert.h>
#include <dirent.h>
#include <errno.h>

#include "pool.h"
#include "repo.h"
#include "util.h"
#include "solv_xmlparser.h"
#define DISABLE_SPLIT
#include "tools_util.h"
#include "repo_zyppdb.h"


enum state {
  STATE_START,
  STATE_PRODUCT,
  STATE_NAME,
  STATE_VERSION,
  STATE_ARCH,
  STATE_SUMMARY,
  STATE_VENDOR,
  STATE_INSTALLTIME,
  NUMSTATES
};

static struct solv_xmlparser_element stateswitches[] = {
  { STATE_START,     "product",       STATE_PRODUCT,       0 },
  { STATE_PRODUCT,   "name",          STATE_NAME,          1 },
  { STATE_PRODUCT,   "version",       STATE_VERSION,       0 },
  { STATE_PRODUCT,   "arch",          STATE_ARCH,          1 },
  { STATE_PRODUCT,   "summary",       STATE_SUMMARY,       1 },
  { STATE_PRODUCT,   "install-time",  STATE_INSTALLTIME,   1 },
  { STATE_PRODUCT,   "vendor",        STATE_VENDOR,        1 },
  { NUMSTATES }
};

struct parsedata {
  Pool *pool;
  Repo *repo;
  Repodata *data;
  const char *filename;
  const char *tmplang;
  Solvable *solvable;
  Id handle;
  struct solv_xmlparser xmlp;
  struct joindata jd;
};



static void
startElement(struct solv_xmlparser *xmlp, int state, const char *name, const char **atts)
{
  struct parsedata *pd = xmlp->userdata;
  Pool *pool = pd->pool;
  Solvable *s = pd->solvable;

  switch(state)
    {
    case STATE_PRODUCT:
      {
	/* parse 'type' */
	const char *type = solv_xmlparser_find_attr("type", atts);
	s = pd->solvable = pool_id2solvable(pool, repo_add_solvable(pd->repo));
	pd->handle = s - pool->solvables;
	if (type)
	  repodata_set_str(pd->data, pd->handle, PRODUCT_TYPE, type);
      }
      break;
    case STATE_VERSION:
      {
	const char *ver = solv_xmlparser_find_attr("ver", atts);
	const char *rel = solv_xmlparser_find_attr("rel", atts);
	/* const char *epoch = solv_xmlparser_find_attr("epoch", atts); ignored */
	s->evr = makeevr(pd->pool, join2(&pd->jd, ver, "-", rel));
      }
      break;
    case STATE_SUMMARY:		/* <summary lang="xy">... */
      pd->tmplang = join_dup(&pd->jd, solv_xmlparser_find_attr("lang", atts));
      break;
    default:
      break;
    }
}


static void
endElement(struct solv_xmlparser *xmlp, int state, char *content)
{
  struct parsedata *pd = xmlp->userdata;
  Solvable *s = pd->solvable;

  switch (state)
    {
    case STATE_PRODUCT:
      if (!s->arch)
	s->arch = ARCH_NOARCH;
      if (!s->evr)
	s->evr = ID_EMPTY;
      if (s->name && s->arch != ARCH_SRC && s->arch != ARCH_NOSRC)
	s->provides = repo_addid_dep(pd->repo, s->provides, pool_rel2id(pd->pool, s->name, s->evr, REL_EQ, 1), 0);
      pd->solvable = 0;
      break;
    case STATE_NAME:
      s->name = pool_str2id(pd->pool, join2(&pd->jd, "product", ":", content), 1);
      break;
    case STATE_ARCH:
      s->arch = pool_str2id(pd->pool, content, 1);
      break;
    case STATE_SUMMARY:
      repodata_set_str(pd->data, pd->handle, pool_id2langid(pd->pool, SOLVABLE_SUMMARY, pd->tmplang, 1), content);
      break;
    case STATE_VENDOR:
      s->vendor = pool_str2id(pd->pool, content, 1);
      break;
    case STATE_INSTALLTIME:
      repodata_set_num(pd->data, pd->handle, SOLVABLE_INSTALLTIME, atol(content));
    default:
      break;
    }
}

static void
errorCallback(struct solv_xmlparser *xmlp, const char *errstr, unsigned int line, unsigned int column)
{
  struct parsedata *pd = xmlp->userdata;
  pool_debug(pd->pool, SOLV_ERROR, "repo_zyppdb: %s: %s at line %u:%u\n", pd->filename, errstr, line, column);
  if (pd->solvable)
    {
      repo_free_solvable(pd->repo, pd->solvable - pd->pool->solvables, 1);
      pd->solvable = 0;
    }
}


/*
 * read all installed products
 *
 * parse each one as a product
 */

int
repo_add_zyppdb_products(Repo *repo, const char *dirpath, int flags)
{
  struct parsedata pd;
  struct dirent *entry;
  char *fullpath;
  DIR *dir;
  FILE *fp;
  Repodata *data;

  data = repo_add_repodata(repo, flags);
  memset(&pd, 0, sizeof(pd));
  pd.repo = repo;
  pd.pool = repo->pool;
  pd.data = data;
  solv_xmlparser_init(&pd.xmlp, stateswitches, &pd, startElement, endElement, errorCallback);

  if (flags & REPO_USE_ROOTDIR)
    dirpath = pool_prepend_rootdir(repo->pool, dirpath);
  dir = opendir(dirpath);
  if (dir)
    {
      while ((entry = readdir(dir)))
	{
	  if (entry->d_name[0] == '.')
	    continue;	/* skip dot files */
	  fullpath = join2(&pd.jd, dirpath, "/", entry->d_name);
	  if ((fp = fopen(fullpath, "r")) == 0)
	    {
	      pool_error(repo->pool, 0, "%s: %s", fullpath, strerror(errno));
	      continue;
	    }
          pd.filename = entry->d_name;
	  solv_xmlparser_parse(&pd.xmlp, fp);
	  fclose(fp);
	}
    }
  closedir(dir);

  solv_xmlparser_free(&pd.xmlp);
  join_freemem(&pd.jd);
  if (flags & REPO_USE_ROOTDIR)
    solv_free((char *)dirpath);
  if (!(flags & REPO_NO_INTERNALIZE))
    repodata_internalize(data);
  return 0;
}

/* EOF */
