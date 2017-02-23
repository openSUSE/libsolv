/*
 * repo_products.c
 *
 * Parses all files below 'proddir'
 * See http://en.opensuse.org/Product_Management/Code11
 *
 *
 * Copyright (c) 2008, Novell Inc.
 *
 * This program is licensed under the BSD license, read LICENSE.BSD
 * for further information
 */

#define _GNU_SOURCE
#define _XOPEN_SOURCE
#include <time.h>
#include <sys/types.h>
#include <sys/stat.h>
#include <unistd.h>
#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <assert.h>
#include <dirent.h>

#include "pool.h"
#include "repo.h"
#include "util.h"
#include "solv_xmlparser.h"
#define DISABLE_SPLIT
#include "tools_util.h"
#include "repo_content.h"
#include "repo_zyppdb.h"
#include "repo_products.h"
#include "repo_releasefile_products.h"


enum state {
  STATE_START,
  STATE_PRODUCT,
  STATE_VENDOR,
  STATE_NAME,
  STATE_VERSION,
  STATE_RELEASE,
  STATE_ARCH,
  STATE_SUMMARY,
  STATE_SHORTSUMMARY,
  STATE_DESCRIPTION,
  STATE_UPDATEREPOKEY,
  STATE_CPEID,
  STATE_URLS,
  STATE_URL,
  STATE_RUNTIMECONFIG,
  STATE_LINGUAS,
  STATE_LANG,
  STATE_REGISTER,
  STATE_TARGET,
  STATE_REGRELEASE,
  STATE_REGFLAVOR,
  STATE_PRODUCTLINE,
  STATE_REGUPDATES,
  STATE_REGUPDREPO,
  STATE_ENDOFLIFE,
  NUMSTATES
};

static struct solv_xmlparser_element stateswitches[] = {
  { STATE_START,     "product",       STATE_PRODUCT,       0 },
  { STATE_PRODUCT,   "vendor",        STATE_VENDOR,        1 },
  { STATE_PRODUCT,   "name",          STATE_NAME,          1 },
  { STATE_PRODUCT,   "version",       STATE_VERSION,       1 },
  { STATE_PRODUCT,   "release",       STATE_RELEASE,       1 },
  { STATE_PRODUCT,   "arch",          STATE_ARCH,          1 },
  { STATE_PRODUCT,   "productline",   STATE_PRODUCTLINE,   1 },
  { STATE_PRODUCT,   "summary",       STATE_SUMMARY,       1 },
  { STATE_PRODUCT,   "shortsummary",  STATE_SHORTSUMMARY,  1 },
  { STATE_PRODUCT,   "description",   STATE_DESCRIPTION,   1 },
  { STATE_PRODUCT,   "register",      STATE_REGISTER,      0 },
  { STATE_PRODUCT,   "urls",          STATE_URLS,          0 },
  { STATE_PRODUCT,   "runtimeconfig", STATE_RUNTIMECONFIG, 0 },
  { STATE_PRODUCT,   "linguas",       STATE_LINGUAS,       0 },
  { STATE_PRODUCT,   "updaterepokey", STATE_UPDATEREPOKEY, 1 },
  { STATE_PRODUCT,   "cpeid",         STATE_CPEID,         1 },
  { STATE_PRODUCT,   "endoflife",     STATE_ENDOFLIFE,     1 },
  { STATE_URLS,      "url",           STATE_URL,           1 },
  { STATE_LINGUAS,   "lang",          STATE_LANG,          0 },
  { STATE_REGISTER,  "target",        STATE_TARGET,        1 },
  { STATE_REGISTER,  "release",       STATE_REGRELEASE,    1 },
  { STATE_REGISTER,  "flavor",        STATE_REGFLAVOR,     1 },
  { STATE_REGISTER,  "updates",       STATE_REGUPDATES,    0 },
  { STATE_REGUPDATES, "repository",   STATE_REGUPDREPO,    0 },
  { NUMSTATES }
};

struct parsedata {
  const char *filename;
  const char *basename;
  Pool *pool;
  Repo *repo;
  Repodata *data;

  struct solv_xmlparser xmlp;
  struct joindata jd;

  const char *tmplang;

  const char *tmpvers;
  const char *tmprel;
  Id urltype;

  unsigned int ctime;

  Solvable *solvable;
  Id handle;

  ino_t baseproduct;
  ino_t currentproduct;
  int productscheme;
};


static time_t
datestr2timestamp(const char *date)
{
  const char *p; 
  struct tm tm; 

  if (!date || !*date)
    return 0;
  for (p = date; *p >= '0' && *p <= '9'; p++)
    ;   
  if (!*p)
    return atoi(date);
  memset(&tm, 0, sizeof(tm));
  p = strptime(date, "%F%T", &tm);
  if (!p)
    {
      memset(&tm, 0, sizeof(tm));
      p = strptime(date, "%F", &tm);
      if (!p || *p)
	return 0;
    }
  return timegm(&tm);
}

static void
startElement(struct solv_xmlparser *xmlp, int state, const char *name, const char **atts)
{
  struct parsedata *pd = xmlp->userdata;
  Pool *pool = pd->pool;
  Solvable *s = pd->solvable;

  switch(state)
    {
    case STATE_PRODUCT:
      /* parse 'schemeversion' and store in global variable */
      {
        const char * scheme = solv_xmlparser_find_attr("schemeversion", atts);
        pd->productscheme = (scheme && *scheme) ? atoi(scheme) : -1;
      }
      if (!s)
	{
	  s = pd->solvable = pool_id2solvable(pool, repo_add_solvable(pd->repo));
	  pd->handle = s - pool->solvables;
	}
      break;

      /* <summary lang="xy">... */
    case STATE_SUMMARY:
    case STATE_DESCRIPTION:
      pd->tmplang = join_dup(&pd->jd, solv_xmlparser_find_attr("lang", atts));
      break;
    case STATE_URL:
      pd->urltype = pool_str2id(pd->pool, solv_xmlparser_find_attr("name", atts), 1);
      break;
    case STATE_REGUPDREPO:
      {
        const char *repoid = solv_xmlparser_find_attr("repoid", atts);
	if (repoid && *repoid)
	  {
	    Id h = repodata_new_handle(pd->data);
	    repodata_set_str(pd->data, h, PRODUCT_UPDATES_REPOID, repoid);
	    repodata_add_flexarray(pd->data, pd->handle, PRODUCT_UPDATES, h);
	  }
	break;
      }
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
      /* product done, finish solvable */
      if (pd->ctime)
        repodata_set_num(pd->data, pd->handle, SOLVABLE_INSTALLTIME, pd->ctime);

      if (pd->basename)
        repodata_set_str(pd->data, pd->handle, PRODUCT_REFERENCEFILE, pd->basename);

      /* this is where <productsdir>/baseproduct points to */
      if (pd->currentproduct == pd->baseproduct)
	repodata_set_str(pd->data, pd->handle, PRODUCT_TYPE, "base");

      if (pd->tmprel)
	{
	  if (pd->tmpvers)
	    s->evr = makeevr(pd->pool, join2(&pd->jd, pd->tmpvers, "-", pd->tmprel));
	  else
	    {
	      fprintf(stderr, "Seen <release> but no <version>\n");
	    }
	}
      else if (pd->tmpvers)
	s->evr = makeevr(pd->pool, pd->tmpvers); /* just version, no release */
      pd->tmpvers = solv_free((void *)pd->tmpvers);
      pd->tmprel = solv_free((void *)pd->tmprel);
      if (!s->arch)
	s->arch = ARCH_NOARCH;
      if (!s->evr)
	s->evr = ID_EMPTY;
      if (s->name && s->arch != ARCH_SRC && s->arch != ARCH_NOSRC)
	s->provides = repo_addid_dep(pd->repo, s->provides, pool_rel2id(pd->pool, s->name, s->evr, REL_EQ, 1), 0);
      pd->solvable = 0;
      break;
    case STATE_VENDOR:
      s->vendor = pool_str2id(pd->pool, content, 1);
      break;
    case STATE_NAME:
      s->name = pool_str2id(pd->pool, join2(&pd->jd, "product", ":", content), 1);
      break;
    case STATE_VERSION:
      pd->tmpvers = solv_strdup(content);
      break;
    case STATE_RELEASE:
      pd->tmprel = solv_strdup(content);
      break;
    case STATE_ARCH:
      s->arch = pool_str2id(pd->pool, content, 1);
      break;
    case STATE_PRODUCTLINE:
      repodata_set_str(pd->data, pd->handle, PRODUCT_PRODUCTLINE, content);
    break;
    case STATE_UPDATEREPOKEY:
      /** obsolete **/
      break;
    case STATE_SUMMARY:
      repodata_set_str(pd->data, pd->handle, pool_id2langid(pd->pool, SOLVABLE_SUMMARY, pd->tmplang, 1), content);
      break;
    case STATE_SHORTSUMMARY:
      repodata_set_str(pd->data, pd->handle, PRODUCT_SHORTLABEL, content);
      break;
    case STATE_DESCRIPTION:
      repodata_set_str(pd->data, pd->handle, pool_id2langid(pd->pool, SOLVABLE_DESCRIPTION, pd->tmplang, 1), content);
      break;
    case STATE_URL:
      if (pd->urltype)
        {
          repodata_add_poolstr_array(pd->data, pd->handle, PRODUCT_URL, content);
          repodata_add_idarray(pd->data, pd->handle, PRODUCT_URL_TYPE, pd->urltype);
        }
      break;
    case STATE_TARGET:
      repodata_set_str(pd->data, pd->handle, PRODUCT_REGISTER_TARGET, content);
      break;
    case STATE_REGRELEASE:
      repodata_set_str(pd->data, pd->handle, PRODUCT_REGISTER_RELEASE, content);
      break;
    case STATE_REGFLAVOR:
      repodata_set_str(pd->data, pd->handle, PRODUCT_REGISTER_FLAVOR, content);
      break;
    case STATE_CPEID:
      if (*content)
        repodata_set_str(pd->data, pd->handle, SOLVABLE_CPEID, content);
      break;
    case STATE_ENDOFLIFE:
      /* FATE#320699: Support tri-state product-endoflife (tag absent, present but nodate(0), present + date) */
      repodata_set_num(pd->data, pd->handle, PRODUCT_ENDOFLIFE, (*content ? datestr2timestamp(content) : 0));
      break;
    default:
      break;
    }
}

static void
errorCallback(struct solv_xmlparser *xmlp, const char *errstr, unsigned int line, unsigned int column)
{
  struct parsedata *pd = xmlp->userdata;
  pool_debug(pd->pool, SOLV_ERROR, "%s: %s at line %u:%u\n", pd->filename, errstr, line, column);
  if (pd->solvable)
    {   
      repo_free_solvable(pd->repo, pd->solvable - pd->pool->solvables, 1); 
      pd->solvable = 0;
    }   
}


int
repo_add_code11_products(Repo *repo, const char *dirpath, int flags)
{
  Repodata *data;
  struct parsedata pd;
  DIR *dir;

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
      struct dirent *entry;
      struct stat st;
      char *fullpath;

      /* check for <productsdir>/baseproduct on code11 and remember its target inode */
      if (stat(join2(&pd.jd, dirpath, "/", "baseproduct"), &st) == 0) /* follow symlink */
	pd.baseproduct = st.st_ino;
      else
	pd.baseproduct = 0;

      while ((entry = readdir(dir)))
	{
	  int len = strlen(entry->d_name);
	  FILE *fp;
	  if (len <= 5 || strcmp(entry->d_name + len - 5, ".prod") != 0)
	    continue;
	  fullpath = join2(&pd.jd, dirpath, "/", entry->d_name);
	  fp = fopen(fullpath, "r");
	  if (!fp)
	    {
	      pool_error(repo->pool, 0, "%s: %s", fullpath, strerror(errno));
	      continue;
	    }
	  if (fstat(fileno(fp), &st))
	    {
	      pool_error(repo->pool, 0, "%s: %s", fullpath, strerror(errno));
	      fclose(fp);
	      continue;
	    }
	  pd.currentproduct = st.st_ino;
	  pd.ctime = (unsigned int)st.st_ctime;
	  pd.filename = fullpath;
	  pd.basename = entry->d_name;
	  solv_xmlparser_parse(&pd.xmlp, fp);
	  fclose(fp);
	}
      closedir(dir);
    }
  solv_xmlparser_free(&pd.xmlp);
  join_freemem(&pd.jd);
  if (flags & REPO_USE_ROOTDIR)
    solv_free((char *)dirpath);

  if (!(flags & REPO_NO_INTERNALIZE))
    repodata_internalize(data);
  return 0;
}


/******************************************************************************************/


/*
 * read all installed products
 *
 * try proddir (reading all .xml files from this directory) first
 * if not available, assume non-code11 layout and parse /etc/xyz-release
 *
 * parse each one as a product
 */

/* Oh joy! Three parsers for the price of one! */

int
repo_add_products(Repo *repo, const char *proddir, int flags)
{
  const char *fullpath;
  DIR *dir;

  if (proddir)
    {
      dir = opendir(flags & REPO_USE_ROOTDIR ? pool_prepend_rootdir_tmp(repo->pool, proddir) : proddir);
      if (dir)
	{
	  /* assume code11 stype products */
	  closedir(dir);
	  return repo_add_code11_products(repo, proddir, flags);
	}
    }

  /* code11 didn't work, try old code10 zyppdb */
  fullpath = "/var/lib/zypp/db/products";
  if (flags & REPO_USE_ROOTDIR)
    fullpath = pool_prepend_rootdir_tmp(repo->pool, fullpath);
  dir = opendir(fullpath);
  if (dir)
    {
      closedir(dir);
      /* assume code10 style products */
      return repo_add_zyppdb_products(repo, "/var/lib/zypp/db/products", flags);
    }

  /* code10/11 didn't work, try -release files parsing */
  fullpath = "/etc";
  if (flags & REPO_USE_ROOTDIR)
    fullpath = pool_prepend_rootdir_tmp(repo->pool, fullpath);
  dir = opendir(fullpath);
  if (dir)
    {
      closedir(dir);
      return repo_add_releasefile_products(repo, "/etc", flags);
    }

  /* no luck. check if the rootdir exists */
  fullpath = pool_get_rootdir(repo->pool);
  if (fullpath && *fullpath)
    {
      dir = opendir(fullpath);
      if (!dir)
	return pool_error(repo->pool, -1, "%s: %s", fullpath, strerror(errno));
      closedir(dir);
    }

  /* the least we can do... */
  if (!(flags & REPO_NO_INTERNALIZE) && (flags & REPO_REUSE_REPODATA) != 0)
    repodata_internalize(repo_last_repodata(repo));
  return 0;
}

/* EOF */
