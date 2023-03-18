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
#include "solv_xfopen.h"

struct sigdata {
  char *sigs;
};

struct xdata {
  char *fn;
  char *pkgjson;
  int delayedlocation;
};

struct parsedata {
  Pool *pool;
  Repo *repo;
  Repodata *data;
  int flags;

  char *subdir;
  char *error;

  Stringpool fnpool;
  Queue fndata;

  Stringpool sigpool;
  struct sigdata *sigdata;
  int nsigdata;

  struct xdata *xdata;
  int nxdata;
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

struct sigdata *
fn2sigdata(struct parsedata *pd, const char *fn, int create)
{
  Id id = stringpool_str2id(&pd->sigpool, fn, create);
  if (!id && !create)
    return 0;
  if (id >= pd->nsigdata)
    {
      int n = id - pd->nsigdata + 1;
      pd->sigdata = solv_realloc2(pd->sigdata, pd->nsigdata + n, sizeof(struct sigdata));
      memset(pd->sigdata + pd->nsigdata, 0, n * sizeof(struct sigdata));
      pd->nsigdata += n;
    }
  return pd->sigdata + id;
}

void
freesigdata(struct parsedata *pd)
{
  int i;
  for (i = 0; i < pd->nsigdata; i++)
    solv_free(pd->sigdata[i].sigs);
  pd->sigdata = solv_free(pd->sigdata);
  pd->nsigdata = 0;
}

static void
set_xdata(struct parsedata *pd, int handle, char *fn, char *pkgjson, int delayedlocation)
{
  struct xdata *xd;
  handle -= pd->repo->start;
  if (handle >= pd->nxdata)
    {
      int n;
      if (!fn && !pkgjson && !delayedlocation)
	return;
      n = handle - pd->nxdata + 16;
      pd->xdata = solv_realloc2(pd->xdata, pd->nxdata + n, sizeof(struct xdata));
      memset(pd->xdata + pd->nxdata, 0, n * sizeof(struct xdata));
      pd->nxdata += n;
    }
  xd = pd->xdata + handle;
  if (xd->fn)
    solv_free(xd->fn);
  if (xd->pkgjson)
    solv_free(xd->pkgjson);
  xd->fn = fn;
  xd->pkgjson = pkgjson;
  xd->delayedlocation = delayedlocation;
}

static void
move_xdata(struct parsedata *pd, int fromhandle, int tohandle)
{
  char *fn = 0, *pkgjson = 0;
  int delayedlocation = 0;
  fromhandle -= pd->repo->start;
  if (fromhandle < pd->nxdata)
    {
      struct xdata *xd = pd->xdata + fromhandle;
      fn = xd->fn;
      pkgjson = xd->pkgjson;
      delayedlocation = xd->delayedlocation;
      xd->fn = 0;
      xd->pkgjson = 0;
      xd->delayedlocation = 0;
    }
  set_xdata(pd, tohandle, fn, pkgjson, delayedlocation);
}

static int parse_package(struct parsedata *pd, struct solv_jsonparser *jp, char *kfn, char *pkgjson);

static int
parse_package_with_pkgjson(struct parsedata *pd, struct solv_jsonparser *jp, char *kfn)
{
  FILE *fp;
  int type;
  char *pkgjson = NULL;
  int line = jp->line;

  type = jsonparser_collect(jp, JP_OBJECT, &pkgjson);
  if (type == JP_OBJECT_END && (fp = solv_fmemopen(pkgjson, strlen(pkgjson), "r")) != 0)
    {
      struct solv_jsonparser jp2;
      jsonparser_init(&jp2, fp);
      jp2.line = line;
      type = jsonparser_parse(&jp2);
      type = type == JP_OBJECT ? parse_package(pd, &jp2, kfn, pkgjson) : JP_ERROR;
      jsonparser_free(&jp2);
      fclose(fp);
    }
  solv_free(pkgjson);
  return type;
}

static int
parse_package(struct parsedata *pd, struct solv_jsonparser *jp, char *kfn, char *pkgjson)
{
  int type = JP_OBJECT;
  Pool *pool= pd->pool;
  Repodata *data = pd->data;
  Solvable *s;
  Id handle;
  char *fn = 0;
  char *subdir = 0;
  Id *fndata = 0, fntype = 0;

  if (!pkgjson && (pd->flags & CONDA_ADD_WITH_SIGNATUREDATA) != 0)
    return parse_package_with_pkgjson(pd, jp, kfn);

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
  /* if we have a global subdir make sure that it matches */
  if (subdir && pd->subdir && strcmp(subdir, pd->subdir) != 0)
    {
      pd->error = "subdir mismatch";
      return JP_ERROR;
    }

  if (fn || kfn)
    {
      int delayedlocation = (subdir || pd->subdir) ? 0 : 1;
      if (pkgjson || delayedlocation)
	set_xdata(pd, handle, solv_strdup(fn ? fn : kfn), pkgjson ? solv_strdup(pkgjson) : 0, delayedlocation);
      if (!delayedlocation)
        repodata_set_location(data, handle, 0, subdir ? subdir : pd->subdir, fn ? fn : kfn);
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
	  set_xdata(pd, handle, 0, 0, 0);
	  return type;
	}
      if (fndata[0] && fndata[0] < fntype)
	{
	  /* replace old package */
	  swap_solvables(pool, data, handle, fndata[1]);
	  move_xdata(pd, handle, fndata[1]);
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
	  type = parse_package(pd, jp, fn, 0);
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
	type = parse_package(pd, jp, 0, 0);
      else
	type = jsonparser_skip(jp, type);
    }
  return type;
}

static int
parse_info(struct parsedata *pd, struct solv_jsonparser *jp)
{
  int type = JP_OBJECT;
  while (type > 0 && (type = jsonparser_parse(jp)) > 0 && type != JP_OBJECT_END)
    {
      if (type == JP_STRING && !strcmp(jp->key, "subdir"))
	{
	  if (!pd->subdir)
	    pd->subdir = strdup(jp->value);
	  else if (strcmp(pd->subdir, jp->value))
	    {
	      pd->error = "subdir mismatch";
	      return JP_ERROR;
	    }
	}
    }
  return type;
}

static int
parse_signatures(struct parsedata *pd, struct solv_jsonparser *jp)
{
  int type = JP_OBJECT;
  while (type > 0 && (type = jsonparser_parse(jp)) > 0 && type != JP_OBJECT_END)
    {
      struct sigdata *sd;
      if (type != JP_OBJECT)
	{
	  type = jsonparser_skip(jp, type);
	  continue;
	}
      sd = fn2sigdata(pd, jp->key, 1);
      sd->sigs = solv_free(sd->sigs);
      type = jsonparser_collect(jp, type, &sd->sigs);
    }
  return type;
}

static int
parse_main(struct parsedata *pd, struct solv_jsonparser *jp)
{
  int type = JP_OBJECT;
  while (type > 0 && (type = jsonparser_parse(jp)) > 0 && type != JP_OBJECT_END)
    {
      if (type == JP_OBJECT && !strcmp("info", jp->key))
	type = parse_info(pd, jp);
      if (type == JP_OBJECT && !strcmp("packages", jp->key))
	type = parse_packages(pd, jp);
      else if (type == JP_ARRAY && !strcmp("packages", jp->key))
	type = parse_packages2(pd, jp);
      else if (type == JP_OBJECT && !strcmp("packages.conda", jp->key) && !(pd->flags & CONDA_ADD_USE_ONLY_TAR_BZ2))
	type = parse_packages(pd, jp);
      else if (type == JP_ARRAY && !strcmp("packages.conda", jp->key) && !(pd->flags & CONDA_ADD_USE_ONLY_TAR_BZ2))
	type = parse_packages2(pd, jp);
      if (type == JP_OBJECT && !strcmp("signatures", jp->key))
	type = parse_signatures(pd, jp);
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
  pd.flags = flags;
  stringpool_init_empty(&pd.fnpool);
  stringpool_init_empty(&pd.sigpool);
  queue_init(&pd.fndata);

  jsonparser_init(&jp, fp);
  if ((type = jsonparser_parse(&jp)) != JP_OBJECT)
    ret = pool_error(pool, -1, "repository does not start with an object");
  else if ((type = parse_main(&pd, &jp)) != JP_OBJECT_END)
    {
      if (pd.error)
        ret = pool_error(pool, -1, "parse error line %d: %s", jp.line, pd.error);
      else
        ret = pool_error(pool, -1, "parse error line %d", jp.line);
    }
  jsonparser_free(&jp);

  /* finalize parsed packages */
  if (pd.xdata)
    {
      int i;
      struct xdata *xd = pd.xdata;
      for (i = 0; i < pd.nxdata; i++, xd++)
	{
	  if (!xd->fn)
	    continue;
	  if (xd->delayedlocation)
	    repodata_set_location(data, repo->start + i, 0, pd.subdir, xd->fn);
	  if (xd->pkgjson && pd.nsigdata)
	    {
	      struct sigdata *sd = fn2sigdata(&pd, xd->fn, 0);
	      if (sd && sd->sigs)
		{
		  char *s = pool_tmpjoin(pool, "{\"info\":", xd->pkgjson, ",\"signatures\":");
		  s = pool_tmpappend(pool, s, sd->sigs, "}");
		  repodata_set_str(data, repo->start + i, SOLVABLE_SIGNATUREDATA, s);
		}
	    }
	  solv_free(xd->fn);
	  solv_free(xd->pkgjson);
	}
      solv_free(pd.xdata);
    }

  if (pd.sigdata)
    freesigdata(&pd);
  stringpool_free(&pd.sigpool);

  queue_free(&pd.fndata);
  stringpool_free(&pd.fnpool);
  solv_free(pd.subdir);
  if (!(flags & REPO_NO_INTERNALIZE))
    repodata_internalize(data);

  return ret;
}

