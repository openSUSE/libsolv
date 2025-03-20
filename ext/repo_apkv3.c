/*
 * Copyright (c) 2024, SUSE LLC
 *
 * This program is licensed under the BSD license, read LICENSE.BSD
 * for further information
 */

/*
 * implement processing of uncompressed apkv3 data (without the leading "ADB.")
 */

#include <sys/types.h>
#include <sys/stat.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#include "pool.h"
#include "repo.h"
#include "util.h"
#include "chksum.h"

#include "repo_apk.h"
#include "repo_apkv3.h"


#define ADB_MAX_SIZE		0x10000000


/* low level */

static inline unsigned int
adb_u32(const unsigned char *p)
{
  return p[0] | p[1] << 8 | p[2] << 16 | p[3] << 24;
}

static const unsigned char *
adb_blob(const unsigned char *adb, size_t adblen, unsigned int v, size_t *bloblp)
{
  size_t blobl;
  int type = (v >> 28) & 15;
  v &= 0xfffffff;
  if (type != 8 && type != 9 && type != 10)
    return 0;
  if (v + (type == 8 ? 1 : type == 9 ? 2 : 4) > adblen)
    return 0;
  blobl = adb[v++];
  if (type > 8)
    blobl |= adb[v++] << 8;
  if (type > 9)
    {
      blobl |= adb[v++] << 16;
      blobl |= adb[v++] << 24;
    }
  if (v + blobl > adblen)
    return 0;
  *bloblp = blobl;
  return adb + v;
}

static int
adb_num(const unsigned char *adb, size_t adblen, unsigned int v, unsigned long long *nump)
{
  unsigned long long num;
  int type = (v >> 28) & 15;
  v &= 0xfffffff;
  if (type != 1 && type != 2 && type != 3)
    return 0;
  if (type == 1)
    {
      *nump = v;
      return 1;
    }
  if (v + (type == 2 ? 4 : 8) > adblen)
    return 0;
  num = adb_u32(adb + v);
  if (type == 3)
    num |= (unsigned long long)adb_u32(adb + v + 4) << 32;
  *nump = num;
  return 1;
}

static unsigned int
adb_arr(const unsigned char *adb, size_t adblen, unsigned int v)
{
  unsigned int cnt;
  int type = (v >> 28) & 15;
  v &= 0xfffffff;
  if (type != 13 && type != 14)
    return 0;
  if (v + 4 > adblen)
    return 0;
  cnt = adb_u32(adb + v);
  if (cnt == 0 || cnt >= 0x1000000 || v + 4 * cnt > adblen)
    return 0;
  return cnt;
}

static inline unsigned int
adb_idx(const unsigned char *adb, unsigned int v, unsigned int cnt, unsigned int idx)
{
  if (idx >= cnt)
    return 0;
  v = (v & 0xfffffff) + idx * 4;
  return adb_u32(adb + v);
}

/* high level */

static Id
adb_poolid(const unsigned char *adb, size_t adblen, unsigned int v, Pool *pool)
{
  size_t blobl;
  const unsigned char *blob = adb_blob(adb, adblen, v, &blobl);
  return blob && blobl < 0x1000000 ? pool_strn2id(pool, (const char *)blob, (unsigned int)blobl, 1) : 0;
}

static void
adb_setstr(const unsigned char *adb, size_t adblen, unsigned int v, Repodata *data, Id p, Id key)
{
  size_t blobl;
  const unsigned char *blob = adb_blob(adb, adblen, v, &blobl);
  if (blob && blobl < 0x1000000)
    {
      char *space = pool_alloctmpspace(data->repo->pool, blobl + 1);
      memcpy(space, blob, blobl);
      space[blobl] = 0;
      repodata_set_str(data, p, key, space);
      pool_freetmpspace(data->repo->pool, space);
    }
}

static void
adb_setnum(const unsigned char *adb, size_t adblen, unsigned int v, Repodata *data, Id p, Id key)
{
  unsigned long long x;
  if (adb_num(adb, adblen, v, &x))
    repodata_set_num(data, p, key, x);
}

static void
adb_setchksum(const unsigned char *adb, size_t adblen, unsigned int v, Repodata *data, Id p, Id key)
{
  size_t blobl;
  const unsigned char *blob = adb_blob(adb, adblen, v, &blobl);
  if (blob && blobl == 20)
    repodata_set_bin_checksum(data, p, key, REPOKEY_TYPE_SHA1, blob);
  else if (blob && blobl == 32)
    repodata_set_bin_checksum(data, p, key, REPOKEY_TYPE_SHA256, blob);
  else if (blob && blobl == 64)
    repodata_set_bin_checksum(data, p, key, REPOKEY_TYPE_SHA512, blob);
}


static Id
adb_get_dep(const unsigned char *adb, size_t adblen, unsigned int v, Pool *pool, Id *whatp)
{
  unsigned int cnt;
  Id id, evr;
  unsigned long long match;
  if (!(cnt = adb_arr(adb, adblen, v)))
    return 0;
  id = adb_poolid(adb, adblen, adb_idx(adb, v, cnt, 1), pool);
  if (!id)
    return 0;
  evr = cnt > 2 ? adb_poolid(adb, adblen, adb_idx(adb, v, cnt, 2), pool) : 0;
  match = 1;
  if (cnt > 3 && !adb_num(adb, adblen, adb_idx(adb, v, cnt, 3), &match))
    return 0;
  if (match & 16)
    {
      if (!whatp || *whatp != SOLVABLE_REQUIRES)
	return 0;
      *whatp = SOLVABLE_CONFLICTS;
    }
  if (evr)
    {
      int flags = 0;
      if (match & 1)
	flags |= REL_EQ;
      if (match & 2)
	flags |= REL_LT;
      if (match & 4)
	flags |= REL_GT;
      if (match & 8)
	{
	  /* fuzzy match, prepend ~ to evr */
	  char *space = pool_alloctmpspace(pool, strlen(pool_id2str(pool, evr)) + 2);
	  space[0] = '~';
	  strcpy(space + 1, pool_id2str(pool, evr));
	  evr = pool_str2id(pool, space, 1);
	  pool_freetmpspace(pool, space);
	}
      id = pool_rel2id(pool, id, evr, flags, 1);
    }
  return id;
}

static void
adb_add_deps(const unsigned char *adb, size_t adblen, unsigned int v, Repodata *data, Id p, Id what)
{
  Id supplements = 0;
  Id id, oldwhat = what;
  Repo *repo = data->repo;
  Pool *pool = repo->pool;
  Solvable *s = pool->solvables + p;
  unsigned int cnt, idx;
  if (!(cnt = adb_arr(adb, adblen, v)))
    return;
  for (idx = 1; idx < cnt; idx++)
    {
      unsigned int vv = adb_idx(adb, v, cnt, idx);
      if (!vv)
	continue;
      what = oldwhat;
      id = adb_get_dep(adb, adblen, vv, pool, &what);
      if (!id)
	continue;
      if (what == SOLVABLE_PROVIDES)
        s->provides = repo_addid_dep(repo, s->provides, id, 0);
      else if (what == SOLVABLE_REQUIRES)
        s->requires = repo_addid_dep(repo, s->requires, id, 0);
      else if (what == SOLVABLE_CONFLICTS)
        s->conflicts = repo_addid_dep(repo, s->conflicts, id, 0);
      else if (what == SOLVABLE_RECOMMENDS)
        s->recommends = repo_addid_dep(repo, s->recommends, id, 0);
      else if (what == SOLVABLE_SUPPLEMENTS)
	supplements = supplements ? pool_rel2id(pool, id, supplements, REL_AND, 1) : id;
    }
  if (supplements)
    s->supplements = repo_addid_dep(repo, s->supplements, supplements, 0);
}

static Id
adb_add_pkg_info(Pool *pool, Repo *repo, Repodata *data, const unsigned char *adb, size_t adblen, unsigned int v, int flags)
{
  Solvable *s;
  unsigned int cnt;
  Id name, origin, license;
  
  if (!(cnt = adb_arr(adb, adblen, v)))
    return 0;
  name = adb_poolid(adb, adblen, adb_idx(adb, v, cnt, 1), pool);
  if (!name)
    return 0;
  s = pool_id2solvable(pool, repo_add_solvable(repo));
  s->name = name;
  s->evr = adb_poolid(adb, adblen, adb_idx(adb, v, cnt, 2), pool);
  adb_setstr(adb, adblen, adb_idx(adb, v, cnt, 4), data, s - pool->solvables, SOLVABLE_SUMMARY);
  adb_setstr(adb, adblen, adb_idx(adb, v, cnt, 4), data, s - pool->solvables, SOLVABLE_DESCRIPTION);
  s->arch = adb_poolid(adb, adblen, adb_idx(adb, v, cnt, 5), pool);
  license = adb_poolid(adb, adblen, adb_idx(adb, v, cnt, 6), pool);
  if (license)
    repodata_set_id(data, s - pool->solvables, SOLVABLE_LICENSE, license);
  origin = adb_poolid(adb, adblen, adb_idx(adb, v, cnt, 7), pool);
  if (origin && origin != s->name)
    repodata_set_id(data, s - pool->solvables, SOLVABLE_SOURCENAME, origin);
  else
    repodata_set_void(data, s - pool->solvables, SOLVABLE_SOURCENAME);
  adb_setstr(adb, adblen, adb_idx(adb, v, cnt, 9), data, s - pool->solvables, SOLVABLE_URL);
  adb_setnum(adb, adblen, adb_idx(adb, v, cnt, 11), data, s - pool->solvables, SOLVABLE_BUILDTIME);
  adb_setnum(adb, adblen, adb_idx(adb, v, cnt, 12), data, s - pool->solvables, SOLVABLE_INSTALLSIZE);
  adb_setnum(adb, adblen, adb_idx(adb, v, cnt, 13), data, s - pool->solvables, SOLVABLE_DOWNLOADSIZE);
  if ((flags & APK_ADD_WITH_HDRID) != 0)
    adb_setchksum(adb, adblen, adb_idx(adb, v, cnt, 3), data, s - pool->solvables, SOLVABLE_HDRID);
  adb_add_deps(adb, adblen, adb_idx(adb, v, cnt, 15), data, s - pool->solvables, SOLVABLE_REQUIRES);
  adb_add_deps(adb, adblen, adb_idx(adb, v, cnt, 16), data, s - pool->solvables, SOLVABLE_PROVIDES);
  adb_add_deps(adb, adblen, adb_idx(adb, v, cnt, 18), data, s - pool->solvables, SOLVABLE_SUPPLEMENTS);
  adb_add_deps(adb, adblen, adb_idx(adb, v, cnt, 19), data, s - pool->solvables, SOLVABLE_RECOMMENDS);
  if (!s->arch)
    s->arch = ARCH_NOARCH;
  if (!s->evr)
    s->evr = ID_EMPTY;
  s->provides = repo_addid_dep(repo, s->provides, pool_rel2id(pool, s->name, s->evr, REL_EQ, 1), 0);
  return s - pool->solvables;
}

static Id
add_add_idb_pkg(Pool *pool, Repo *repo, Repodata *data, const unsigned char *adb, size_t adblen, unsigned int v, int flags)
{
  unsigned int cnt, type_size;
  size_t blobl;
  const unsigned char *blob = adb_blob(adb, adblen, v, &blobl);
  if (blobl < 12 || blobl >= ADB_MAX_SIZE)
    return 0;
  type_size = adb_u32(blob);
  if ((type_size & 0xc0000000) == 0 && blobl == type_size + 4)
    {
      adb = (unsigned char *)blob + 4;
      adblen = blobl - 4;
    }
  else if (type_size == 0xc0000000)
    {
      if (blobl < 16 + 8 || adb_u32(blob + 8) != (unsigned int)(blobl - 16) || adb_u32(blob + 12) != 0)
	return 0;
      adb = (unsigned char *)blob + 16;
      adblen = blobl - 16;
    }
  else
    return 0;
  v = adb_u32(adb + 4);
  if (!(cnt = adb_arr(adb, adblen, v)))
    return 0;
  return adb_add_pkg_info(pool, repo, data, adb, adblen, adb_idx(adb, v, cnt, 1), flags);
}

/* file reading */

static int
adb_read_blk_header(FILE *fp, unsigned long long *sizep)
{
  unsigned char buf[12];
  unsigned int size;
  unsigned long long lsize;
  if (fread(buf, 4, 1, fp) != 1)
    return -1;
  size = buf[0] | buf[1] << 8 | buf[2] << 16 | (buf[3] & 0x3f) << 24;
  if ((buf[3] & 0xc0) != 0xc0)
    {
      if (size < 4)
	return -1;
      *sizep = size - 4;
      return (buf[3] & 0xc0) >> 6;
    }
  if (fread(buf, 12, 1, fp) != 1)
    return -1;
  lsize = adb_u32(buf + 4);
  lsize |= (unsigned long long)adb_u32(buf + 8) << 32;
  if (lsize < 16)
    return -1;
  *sizep = lsize - 16;
  return size;
}

static const unsigned char *
adb_read_adb_blk(Pool *pool, FILE *fp, const char *fn, size_t *adblenp)
{
  unsigned char *adb;
  unsigned long long size;
  if (adb_read_blk_header(fp, &size) != 0)
    {
      pool_error(pool, 0, "%s: missing adb block", fn);
      return 0;
    }
  if (size > ADB_MAX_SIZE)
    {
      pool_error(pool, 0, "%s: oversized adb block", fn);
      return 0;
    }
  adb = solv_malloc((size_t)size);
  if (fread(adb, (size_t)size, 1, fp) != 1)
    {
      solv_free(adb);
      pool_error(pool, 0, "%s: adb block read error", fn);
      return 0;
    }
  *adblenp = (size_t)size;
  return adb;
}

Id
apkv3_add_pkg(Repo *repo, Repodata *data, const char *fn, FILE *fp, int flags)
{
  Pool *pool = repo->pool;
  char buf[4];
  const unsigned char *adb;
  size_t adblen;
  unsigned int v, cnt;
  Id p = 0;

  if (fread(buf, 4, 1, fp) != 1 || buf[0] != 'p' || buf[1] != 'c' || buf[2] != 'k' || buf[3] != 'g')
    return pool_error(pool, 0, "%s: not an apkv3 package", fn);

  if (!(adb = adb_read_adb_blk(pool, fp, fn, &adblen)))
    return 0;

  v = adb_u32(adb + 4);
  if ((cnt = adb_arr(adb, adblen, v)) != 0)
    p = adb_add_pkg_info(pool, repo, data, adb, adblen, adb_idx(adb, v, cnt, 1), flags);
  if (p && (flags & APK_ADD_WITH_PKGID) != 0)
    {
      unsigned char pkgid[16];
      Chksum *pkgidchk = solv_chksum_create(REPOKEY_TYPE_MD5);
      solv_chksum_add(pkgidchk, adb, adblen);
      solv_chksum_free(pkgidchk, pkgid);
      repodata_set_bin_checksum(data, p, SOLVABLE_PKGID, REPOKEY_TYPE_MD5, pkgid);
    }
  solv_free((void *)adb);
  return p;
}

int
apkv3_add_idx(Repo *repo, Repodata *data, FILE *fp, int flags)
{
  Pool *pool = repo->pool;
  char buf[4];
  const unsigned char *adb;
  size_t adblen;
  unsigned int v, cnt, idx;
  int idb = flags & APK_ADD_INSTALLED_DB ? 1 : 0;

  if (fread(buf, 4, 1, fp) != 1 || memcmp(buf, (idb ? "idb" : "indx") , 4) != 0)
    return pool_error(pool, -1, (idb ?  "not an apkv3 installed database" : "not an apkv3 package index"));

  if (!(adb = adb_read_adb_blk(pool, fp, idb ? "installed database" : "index", &adblen)))
    return -1;

  v = adb_u32(adb + 4);
  if ((cnt = adb_arr(adb, adblen, v)) != 0)
    {
      v = adb_idx(adb, v, cnt, idb ? 1 : 2);
      if ((cnt = adb_arr(adb, adblen, v)) != 0)
	{
	  for (idx = 1; idx < cnt; idx++)
	    {
	      if (idb)
		add_add_idb_pkg(pool, repo, data, adb, adblen, adb_idx(adb, v, cnt, idx), flags);
	      else
		adb_add_pkg_info(pool, repo, data, adb, adblen, adb_idx(adb, v, cnt, idx), flags);
	    }
	}
    }
  solv_free((void *)adb);
  return 0;
}

