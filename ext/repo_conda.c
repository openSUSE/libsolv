/*
 * Copyright (c) 2019, SUSE LLC.
 *
 * This program is licensed under the BSD license, read LICENSE.BSD
 * for further information
 */

#define _GNU_SOURCE
#include <sys/types.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "pool.h"
#include "repo.h"
#include "chksum.h"
#include "solv_jsonparser.h"
#include "conda.h"
#include "repo_conda.h"

struct parsedata {
  Pool *pool;
  Repo *repo;
  Repodata *data;

  Stringpool fnpool;
  Queue fndata;
};

static int
parse_deps(struct parsedata *pd, struct solv_jsonparser *jp, Offset *depp)
{
  int type = JP_ARRAY;
  while (type > 0 && (type = jsonparser_parse(jp)) > 0 && type != JP_ARRAY_END)
    {
      if (type == JP_STRING)
	{
	  Id id = pool_conda_matchspec(pd->pool, jp->value);
	  if (id)
	    *depp = repo_addid_dep(pd->repo, *depp, id, 0);
	}
      else
	type = jsonparser_skip(jp, type);
    }
  return type;
}

static int
parse_otherdeps(struct parsedata *pd, struct solv_jsonparser *jp, Id handle, Id keyname)
{
  int type = JP_ARRAY;
  while (type > 0 && (type = jsonparser_parse(jp)) > 0 && type != JP_ARRAY_END)
    {
      if (type == JP_STRING)
	{
	  Id id = pool_conda_matchspec(pd->pool, jp->value);
	  if (id)
	    repodata_add_idarray(pd->data, handle, keyname, id);
	}
      else
	type = jsonparser_skip(jp, type);
    }
  return type;
}

static int
parse_trackfeatures_array(struct parsedata *pd, struct solv_jsonparser *jp, Id handle)
{
  int type = JP_ARRAY;
  while (type > 0 && (type = jsonparser_parse(jp)) > 0 && type != JP_ARRAY_END)
    {
      if (type == JP_STRING)
	{
	  char *p = jp->value, *pe;
	  while (*p == ' ' || *p == '\t')
	    p++;
	  if (!*p)
	    continue;
	  for (pe = p + strlen(p) - 1; pe > p; pe--)
	    if (*pe != ' ' && *pe != '\t')
	      break;
	  repodata_add_idarray(pd->data, handle, SOLVABLE_TRACK_FEATURES, pool_strn2id(pd->pool, p, pe - p + 1, 1));
	}
      else
	type = jsonparser_skip(jp, type);
    }
  return type;
}

static void
parse_trackfeatures_string(struct parsedata *pd, const char *p, Id handle)
{
  const char *pe;
  for (; *p; p++)
    {
      if (*p == ' ' || *p == '\t' || *p == ',')
	continue;
      pe = p + 1;
      while (*pe && *pe != ' ' && *pe != '\t' && *pe != ',')
	pe++;
      repodata_add_idarray(pd->data, handle, SOLVABLE_TRACK_FEATURES, pool_strn2id(pd->pool, p, pe - p, 1));
      p = pe - 1;
    }
}

static void 
swap_solvables(Pool *pool, Repodata *data, Id pa, Id pb)
{
  Solvable tmp; 

  tmp = pool->solvables[pa];
  pool->solvables[pa] = pool->solvables[pb];
  pool->solvables[pb] = tmp; 
  repodata_swap_attrs(data, pa, pb); 
}

static Id *
fn2data(struct parsedata *pd, const char *fn, Id *fntypep, int create)
{
  size_t l = strlen(fn), extl = 0;
  Id fnid;
  if (l > 6 && !strcmp(fn + l - 6, ".conda"))
    extl = 6;
  else if (l > 8 && !strcmp(fn + l - 8, ".tar.bz2"))
    extl = 8;
  else
    return 0;
  fnid = stringpool_strn2id(&pd->fnpool, fn, l - extl, create);
  if (!fnid)
    return 0;
  if (fnid * 2 + 2 > pd->fndata.count)
    queue_insertn(&pd->fndata, pd->fndata.count, fnid * 2 + 2 - pd->fndata.count, 0);
  if (fntypep)
    *fntypep = extl == 8 ? 1 : 2;	/* 1: legacy .tar.bz2  2: .conda */
  return pd->fndata.elements + 2 * fnid;
}

static int
parse_package(struct parsedata *pd, struct solv_jsonparser *jp, char *kfn)
{
  int type = JP_OBJECT;
  Pool *pool= pd->pool;
  Repodata *data = pd->data;
  Solvable *s;
  Id handle;
  char *fn = 0;
  char *subdir = 0;
  Id *fndata = 0, fntype = 0;

  handle = repo_add_solvable(pd->repo);
  s = pool_id2solvable(pool, handle);
  while (type > 0 && (type = jsonparser_parse(jp)) > 0 && type != JP_OBJECT_END)
    {
      if (type == JP_STRING && !strcmp(jp->key, "build"))
	repodata_add_poolstr_array(data, handle, SOLVABLE_BUILDFLAVOR, jp->value);
      else if (type == JP_NUMBER && !strcmp(jp->key, "build_number"))
	repodata_set_str(data, handle, SOLVABLE_BUILDVERSION, jp->value);
      else if (type == JP_ARRAY && !strcmp(jp->key, "depends"))
	type = parse_deps(pd, jp, &s->requires);
      else if (type == JP_ARRAY && !strcmp(jp->key, "requires"))
	type = parse_deps(pd, jp, &s->requires);
      else if (type == JP_ARRAY && !strcmp(jp->key, "constrains"))
	type = parse_otherdeps(pd, jp, handle, SOLVABLE_CONSTRAINS);
      else if (type == JP_STRING && !strcmp(jp->key, "license"))
	repodata_add_poolstr_array(data, handle, SOLVABLE_LICENSE, jp->value);
      else if (type == JP_STRING && !strcmp(jp->key, "md5"))
	repodata_set_checksum(data, handle, SOLVABLE_PKGID, REPOKEY_TYPE_MD5, jp->value);
      else if (type == JP_STRING && !strcmp(jp->key, "sha256"))
	repodata_set_checksum(data, handle, SOLVABLE_CHECKSUM, REPOKEY_TYPE_SHA256, jp->value);
      else if (type == JP_STRING && !strcmp(jp->key, "name"))
	s->name = pool_str2id(pool, jp->value, 1);
      else if (type == JP_STRING && !strcmp(jp->key, "version"))
	s->evr= pool_str2id(pool, jp->value, 1);
      else if (type == JP_STRING && !strcmp(jp->key, "fn") && !fn)
	fn = solv_strdup(jp->value);
      else if (type == JP_STRING && !strcmp(jp->key, "subdir") && !subdir)
	subdir = solv_strdup(jp->value);
      else if (type == JP_NUMBER && !strcmp(jp->key, "size"))
	repodata_set_num(data, handle, SOLVABLE_DOWNLOADSIZE, strtoull(jp->value, 0, 10));
      else if (type == JP_NUMBER && !strcmp(jp->key, "timestamp"))
	{
	  unsigned long long ts = strtoull(jp->value, 0, 10);
	  if (ts > 253402300799ULL)
	    ts /= 1000;
	  repodata_set_num(data, handle, SOLVABLE_BUILDTIME, ts);
	}
      else if (type == JP_STRING && !strcmp(jp->key, "track_features"))
	parse_trackfeatures_string(pd, jp->value, handle);
      else if (type == JP_ARRAY && !strcmp(jp->key, "track_features"))
	type = parse_trackfeatures_array(pd, jp, handle);
      else
	type = jsonparser_skip(jp, type);
    }
  if (fn || kfn)
    {
      repodata_set_location(data, handle, 0, subdir, fn ? fn : kfn);
      fndata = fn2data(pd, fn ? fn : kfn, &fntype, 1);
    }
  solv_free(fn);
  solv_free(subdir);
  if (!s->evr)
    s->evr = 1;
  if (s->name)
    s->provides = repo_addid_dep(pd->repo, s->provides, pool_rel2id(pool, s->name, s->evr, REL_EQ, 1), 0);

  if (fndata)
    {
      /* deal with legacy package entries */
      if (fndata[0] && fndata[0] > fntype)
	{
	  /* ignore this package */
	  repo_free_solvable(pd->repo, handle, 1);
	  return type;
	}
      if (fndata[0] && fndata[0] < fntype)
	{
	  /* replace old package */
	  swap_solvables(pool, data, handle, fndata[1]);
	  repo_free_solvable(pd->repo, handle, 1);
	  handle = fndata[1];
	}
      fndata[0] = fntype;
      fndata[1] = handle;
    }
  return type;
}

static int
parse_packages(struct parsedata *pd, struct solv_jsonparser *jp)
{
  int type = JP_OBJECT;
  while (type > 0 && (type = jsonparser_parse(jp)) > 0 && type != JP_OBJECT_END)
    {
      if (type == JP_OBJECT)
	{
	  char *fn = solv_strdup(jp->key);
	  type = parse_package(pd, jp, fn);
	  solv_free(fn);
	}
      else
	type = jsonparser_skip(jp, type);
    }
  return type;
}

static int
parse_packages2(struct parsedata *pd, struct solv_jsonparser *jp)
{
  int type = JP_ARRAY;
  while (type > 0 && (type = jsonparser_parse(jp)) > 0 && type != JP_ARRAY_END)
    {
      if (type == JP_OBJECT)
	type = parse_package(pd, jp, 0);
      else
	type = jsonparser_skip(jp, type);
    }
  return type;
}

static int
parse_main(struct parsedata *pd, struct solv_jsonparser *jp, int flags)
{
  int type = JP_OBJECT;
  while (type > 0 && (type = jsonparser_parse(jp)) > 0 && type != JP_OBJECT_END)
    {
      if (type == JP_OBJECT && !strcmp("packages", jp->key))
	type = parse_packages(pd, jp);
      else if (type == JP_ARRAY && !strcmp("packages", jp->key))
	type = parse_packages2(pd, jp);
      else if (type == JP_OBJECT && !strcmp("packages.conda", jp->key) && !(flags & CONDA_ADD_USE_ONLY_TAR_BZ2))
	type = parse_packages(pd, jp);
      else if (type == JP_ARRAY && !strcmp("packages.conda", jp->key) && !(flags & CONDA_ADD_USE_ONLY_TAR_BZ2))
	type = parse_packages2(pd, jp);
      else
	type = jsonparser_skip(jp, type);
    }
  return type;
}

int
repo_add_conda(Repo *repo, FILE *fp, int flags)
{
  Pool *pool = repo->pool;
  struct solv_jsonparser jp;
  struct parsedata pd;
  Repodata *data;
  int type, ret = 0;

  data = repo_add_repodata(repo, flags);

  memset(&pd, 0, sizeof(pd));
  pd.pool = pool;
  pd.repo = repo;
  pd.data = data;
  stringpool_init_empty(&pd.fnpool);
  queue_init(&pd.fndata);

  jsonparser_init(&jp, fp);
  if ((type = jsonparser_parse(&jp)) != JP_OBJECT)
    ret = pool_error(pool, -1, "repository does not start with an object");
  else if ((type = parse_main(&pd, &jp, flags)) != JP_OBJECT_END)
    ret = pool_error(pool, -1, "parse error line %d", jp.line);
  jsonparser_free(&jp);

  queue_free(&pd.fndata);
  stringpool_free(&pd.fnpool);
  if (!(flags & REPO_NO_INTERNALIZE))
    repodata_internalize(data);

  return ret;
}

