/*
 * Copyright (c) 2009-2013, Novell Inc.
 *
 * This program is licensed under the BSD license, read LICENSE.BSD
 * for further information
 */

#include <stdio.h>
#include <sys/stat.h>

#include "pool.h"
#include "repo.h"
#include "hash.h"
#include "repo_rpmdb.h"
#include "pool_fileconflicts.h"

struct cbdata {
  Pool *pool;
  int create;

  Queue lookat;		/* conflict candidates */
  Queue lookat_dir;	/* not yet conflicting directories */

  Hashtable cflmap;
  Hashval cflmapn;
  unsigned int cflmapused;

  Hashtable dirmap;
  Hashval dirmapn;
  unsigned int dirmapused;
  int dirconflicts;

  Map idxmap;

  unsigned int lastdiridx;	/* last diridx we have seen */
  unsigned int lastdirhash;	/* strhash of last dir we have seen */

  Id idx;	/* index of package we're looking at */
  Id hx;	/* used in findfileconflicts2_cb, limit to files matching hx */

  Queue files;
  unsigned char *filesspace;
  unsigned int filesspacen;
};

#define FILESSPACE_BLOCK 255

static Hashtable
growhash(Hashtable map, Hashval *mapnp)
{
  Hashval mapn = *mapnp;
  Hashval newn = (mapn + 1) * 2 - 1;
  Hashval i, h, hh;
  Hashtable m;
  Id hx, qx;

  m = solv_calloc(newn + 1, 2 * sizeof(Id));
  for (i = 0; i <= mapn; i++)
    {
      hx = map[2 * i];
      if (!hx)
	continue;
      h = hx & newn;
      hh = HASHCHAIN_START;
      for (;;)
	{
	  qx = m[2 * h];
	  if (!qx)
	    break;
	  h = HASHCHAIN_NEXT(h, hh, newn);
	}
      m[2 * h] = hx;
      m[2 * h + 1] = map[2 * i + 1];
    }
  solv_free(map);
  *mapnp = newn;
  return m;
}

static void
finddirs_cb(void *cbdatav, const char *fn, struct filelistinfo *info)
{
  struct cbdata *cbdata = cbdatav;
  Hashval h, hh;
  Id hx, qx;
  Id oidx, idx = cbdata->idx;

  hx = strhash(fn);
  if (!hx)
    hx = strlen(fn) + 1;
  h = hx & cbdata->dirmapn;
  hh = HASHCHAIN_START;
  for (;;)
    {
      qx = cbdata->dirmap[2 * h];
      if (!qx)
	break;
      if (qx == hx)
	break;
      h = HASHCHAIN_NEXT(h, hh, cbdata->dirmapn);
    }
  if (!qx)
    {
      /* a miss */
      if (!cbdata->create)
	return;
      cbdata->dirmap[2 * h] = hx;
      cbdata->dirmap[2 * h + 1] = idx;
      cbdata->dirmapused++;
      if (cbdata->dirmapused * 2 > cbdata->dirmapn)
	cbdata->dirmap = growhash(cbdata->dirmap, &cbdata->dirmapn);
      return;
    }
  oidx = cbdata->dirmap[2 * h + 1];
  if (oidx == idx)
    return;
  /* found a conflict, this dir may be used in multiple packages */
  if (oidx != -1)
    {
      MAPSET(&cbdata->idxmap, oidx);
      cbdata->dirmap[2 * h + 1] = -1;
      cbdata->dirconflicts++;
    }
  MAPSET(&cbdata->idxmap, idx);
}

static inline int
isindirmap(struct cbdata *cbdata, Id hx)
{
  Hashval h, hh;
  Id qx;

  h = hx & cbdata->dirmapn;
  hh = HASHCHAIN_START;
  for (;;)
    {
      qx = cbdata->dirmap[2 * h];
      if (!qx)
	return 0;
      if (qx == hx)
	return cbdata->dirmap[2 * h + 1] == -1 ? 1 : 0;
      h = HASHCHAIN_NEXT(h, hh, cbdata->dirmapn);
    }
}

static void
findfileconflicts_cb(void *cbdatav, const char *fn, struct filelistinfo *info)
{
  struct cbdata *cbdata = cbdatav;
  int isdir = S_ISDIR(info->mode);
  const char *dp;
  Id idx, oidx;
  Id hx, qx;
  Hashval h, hh, dhx;

  idx = cbdata->idx;

  if (!info->dirlen)
    return;
  dp = fn + info->dirlen;
  if (info->diridx != cbdata->lastdiridx)
    {
      cbdata->lastdiridx = info->diridx;
      cbdata->lastdirhash = strnhash(fn, dp - fn);
    }
  dhx = cbdata->lastdirhash;
#if 1
  /* this mirrors the "if (!hx) hx = strlen(fn) + 1" in finddirs_cb */
  if (!isindirmap(cbdata, dhx ? dhx : dp - fn + 1))
    return;
#endif
  hx = strhash_cont(dp, dhx);
  if (!hx)
    hx = strlen(fn) + 1;

  h = hx & cbdata->cflmapn;
  hh = HASHCHAIN_START;
  for (;;)
    {
      qx = cbdata->cflmap[2 * h];
      if (!qx)
	break;
      if (qx == hx)
	break;
      h = HASHCHAIN_NEXT(h, hh, cbdata->cflmapn);
    }
  if (!qx)
    {
      /* a miss */
      if (!cbdata->create)
	return;
      cbdata->cflmap[2 * h] = hx;
      cbdata->cflmap[2 * h + 1] = (isdir ? ~idx : idx);
      cbdata->cflmapused++;
      if (cbdata->cflmapused * 2 > cbdata->cflmapn)
	cbdata->cflmap = growhash(cbdata->cflmap, &cbdata->cflmapn);
      return;
    }
  oidx = cbdata->cflmap[2 * h + 1];
  if (oidx < 0)
    {
      int i;
      if (isdir)
	{
	  /* both are directories. delay the conflict, keep oidx in slot */
          queue_push2(&cbdata->lookat_dir, hx, idx);
	  return;
	}
      oidx = ~oidx;
      /* now have file, had directories before. */
      cbdata->cflmap[2 * h + 1] = oidx;	/* make it a file */
      /* dump all delayed directory hits for hx */
      for (i = 0; i < cbdata->lookat_dir.count; i += 2)
	if (cbdata->lookat_dir.elements[i] == hx)
	  queue_push2(&cbdata->lookat, hx, cbdata->lookat_dir.elements[i + 1]);
    }
  else if (oidx == idx)
    return;	/* no conflicts with ourself, please */
  queue_push2(&cbdata->lookat, hx, oidx);
  queue_push2(&cbdata->lookat, hx, idx);
}

static inline void
addfilesspace(struct cbdata *cbdata, unsigned char *data, int len)
{
  cbdata->filesspace = solv_extend(cbdata->filesspace, cbdata->filesspacen, len, 1, FILESSPACE_BLOCK);
  memcpy(cbdata->filesspace + cbdata->filesspacen, data, len);
  cbdata->filesspacen += len;
}

static void
findfileconflicts2_cb(void *cbdatav, const char *fn, struct filelistinfo *info)
{
  struct cbdata *cbdata = cbdatav;
  Hashval hx;
  const char *dp;
  char md5padded[34];

  if (!info->dirlen)
    return;
  dp = fn + info->dirlen;
  if (info->diridx != cbdata->lastdiridx)
    {
      cbdata->lastdiridx = info->diridx;
      cbdata->lastdirhash = strnhash(fn, dp - fn);
    }
  hx = cbdata->lastdirhash;
  hx = strhash_cont(dp, hx);
  if (!hx)
    hx = strlen(fn) + 1;
  if ((Id)hx != cbdata->hx)
    return;
  strncpy(md5padded, info->digest, 32);
  md5padded[32] = 0;
  md5padded[33] = info->color;
  /* printf("%d, hx %x -> %s   %d %s\n", cbdata->idx, hx, fn, fmode, md5); */
  queue_push(&cbdata->files, cbdata->filesspacen);
  addfilesspace(cbdata, (unsigned char *)md5padded, 34);
  addfilesspace(cbdata, (unsigned char *)fn, strlen(fn) + 1);
}

static int
lookat_cmp(const void *ap, const void *bp, void *dp)
{
  const Id *a = ap;
  const Id *b = bp;
  unsigned int ahx = (unsigned int)a[0];	/* a[0] can be < 0 */
  unsigned int bhx = (unsigned int)b[0];
  return ahx < bhx ? -1 : ahx > bhx ? 1 : a[1] - b[1];
}

static int
conflicts_cmp(const void *ap, const void *bp, void *dp)
{
  Pool *pool = dp;
  const Id *a = ap;
  const Id *b = bp;
  if (a[0] != b[0])	/* filename1 */
    return strcmp(pool_id2str(pool, a[0]), pool_id2str(pool, b[0]));
  if (a[3] != b[3])	/* filename2 */
    return strcmp(pool_id2str(pool, a[3]), pool_id2str(pool, b[3]));
  if (a[1] != b[1])	/* pkgid1 */
    return a[1] - b[1];
  if (a[4] != b[4])	/* pkgid2 */
    return a[4] - b[4];
  return 0;
}

int
pool_findfileconflicts(Pool *pool, Queue *pkgs, int cutoff, Queue *conflicts, void *(*handle_cb)(Pool *, Id, void *) , void *handle_cbdata)
{
  int i, j, cflmapn, idxmapset;
  struct cbdata cbdata;
  unsigned int now, start;
  void *handle;
  Id p;
  int obsoleteusescolors = pool_get_flag(pool, POOL_FLAG_OBSOLETEUSESCOLORS);

  queue_empty(conflicts);
  if (!pkgs->count)
    return 0;

  now = start = solv_timems(0);
  POOL_DEBUG(SOLV_DEBUG_STATS, "searching for file conflicts\n");
  POOL_DEBUG(SOLV_DEBUG_STATS, "packages: %d, cutoff %d\n", pkgs->count, cutoff);

  memset(&cbdata, 0, sizeof(cbdata));
  cbdata.pool = pool;
  queue_init(&cbdata.lookat);
  queue_init(&cbdata.lookat_dir);
  queue_init(&cbdata.files);
  map_init(&cbdata.idxmap, pkgs->count);

  if (cutoff <= 0)
    cutoff = pkgs->count;

  /* avarage file list size: 200 files per package */
  /* avarage dir count: 20 dirs per package */

  /* first pass: scan dirs */
  cflmapn = (cutoff + 3) * 64;
  while ((cflmapn & (cflmapn - 1)) != 0)
    cflmapn = cflmapn & (cflmapn - 1);
  cbdata.dirmap = solv_calloc(cflmapn, 2 * sizeof(Id));
  cbdata.dirmapn = cflmapn - 1;	/* make it a mask */
  cbdata.create = 1;
  idxmapset = 0;
  for (i = 0; i < pkgs->count; i++)
    {
      p = pkgs->elements[i];
      cbdata.idx = i;
      if (i == cutoff)
	cbdata.create = 0;
      handle = (*handle_cb)(pool, p, handle_cbdata);
      if (handle)
        rpm_iterate_filelist(handle, RPM_ITERATE_FILELIST_ONLYDIRS, finddirs_cb, &cbdata);
      if (MAPTST(&cbdata.idxmap, i))
        idxmapset++;
    }

  POOL_DEBUG(SOLV_DEBUG_STATS, "dirmap size: %d, used %d\n", cbdata.dirmapn + 1, cbdata.dirmapused);
  POOL_DEBUG(SOLV_DEBUG_STATS, "dirmap memory usage: %d K\n", (cbdata.dirmapn + 1) * 2 * (int)sizeof(Id) / 1024);
  POOL_DEBUG(SOLV_DEBUG_STATS, "dirmap creation took %d ms\n", solv_timems(now));
  POOL_DEBUG(SOLV_DEBUG_STATS, "dir conflicts found: %d, idxmap %d of %d\n", cbdata.dirconflicts, idxmapset, pkgs->count);

  /* second pass: scan files */
  now = solv_timems(0);
  cflmapn = (cutoff + 3) * 128;
  while ((cflmapn & (cflmapn - 1)) != 0)
    cflmapn = cflmapn & (cflmapn - 1);
  cbdata.cflmap = solv_calloc(cflmapn, 2 * sizeof(Id));
  cbdata.cflmapn = cflmapn - 1;	/* make it a mask */
  cbdata.create = 1;
  for (i = 0; i < pkgs->count; i++)
    {
      if (!MAPTST(&cbdata.idxmap, i))
	continue;
      p = pkgs->elements[i];
      cbdata.idx = i;
      if (i == cutoff)
	cbdata.create = 0;
      handle = (*handle_cb)(pool, p, handle_cbdata);
      if (!handle)
	continue;
      cbdata.lastdiridx = -1;
      rpm_iterate_filelist(handle, RPM_ITERATE_FILELIST_NOGHOSTS, findfileconflicts_cb, &cbdata);
    }

  POOL_DEBUG(SOLV_DEBUG_STATS, "filemap size: %d, used %d\n", cbdata.cflmapn + 1, cbdata.cflmapused);
  POOL_DEBUG(SOLV_DEBUG_STATS, "filemap memory usage: %d K\n", (cbdata.cflmapn + 1) * 2 * (int)sizeof(Id) / 1024);
  POOL_DEBUG(SOLV_DEBUG_STATS, "filemap creation took %d ms\n", solv_timems(now));

  cbdata.dirmap = solv_free(cbdata.dirmap);
  cbdata.dirmapn = 0;
  cbdata.dirmapused = 0;
  cbdata.cflmap = solv_free(cbdata.cflmap);
  cbdata.cflmapn = 0;
  cbdata.cflmapused = 0;
  map_free(&cbdata.idxmap);

  now = solv_timems(0);
  POOL_DEBUG(SOLV_DEBUG_STATS, "lookat_dir size: %d\n", cbdata.lookat_dir.count);
  queue_free(&cbdata.lookat_dir);

  /* sort and unify */
  solv_sort(cbdata.lookat.elements, cbdata.lookat.count / 2, sizeof(Id) * 2, &lookat_cmp, pool);
  for (i = j = 0; i < cbdata.lookat.count; i += 2)
    {
      Id hx = cbdata.lookat.elements[i];
      Id idx = cbdata.lookat.elements[i + 1];
      if (j && hx == cbdata.lookat.elements[j - 2] && idx == cbdata.lookat.elements[j - 1])
	continue;
      cbdata.lookat.elements[j++] = hx;
      cbdata.lookat.elements[j++] = idx;
    }
  queue_truncate(&cbdata.lookat, j);
  POOL_DEBUG(SOLV_DEBUG_STATS, "candidates: %d\n", cbdata.lookat.count / 2);

  /* third pass: scan candidates */
  for (i = 0; i < cbdata.lookat.count - 2; i += 2)
    {
      int pend, ii, jj;
      int iterflags;
      Id hx = cbdata.lookat.elements[i];
      Id pidx = cbdata.lookat.elements[i + 1];

      iterflags = RPM_ITERATE_FILELIST_WITHMD5 | RPM_ITERATE_FILELIST_NOGHOSTS;
      if (obsoleteusescolors)
	iterflags |= RPM_ITERATE_FILELIST_WITHCOL;
      p = pkgs->elements[pidx];
      if (cbdata.lookat.elements[i + 2] != hx)
	continue;	/* no package left */
      queue_empty(&cbdata.files);
      cbdata.filesspace = solv_free(cbdata.filesspace);
      cbdata.filesspacen = 0;

      cbdata.idx = p;
      cbdata.hx = cbdata.lookat.elements[i];
      handle = (*handle_cb)(pool, p, handle_cbdata);
      if (!handle)
	continue;
      cbdata.lastdiridx = -1;
      rpm_iterate_filelist(handle, iterflags, findfileconflicts2_cb, &cbdata);

      pend = cbdata.files.count;
      for (j = i + 2; j < cbdata.lookat.count && cbdata.lookat.elements[j] == hx; j += 2)
	{
	  Id qidx = cbdata.lookat.elements[j + 1];
	  Id q = pkgs->elements[qidx];
	  if (pidx >= cutoff && qidx >= cutoff)
	    continue;	/* no conflicts between packages with idx >= cutoff */
	  cbdata.idx = q;
	  handle = (*handle_cb)(pool, q, handle_cbdata);
	  if (!handle)
	    continue;
	  cbdata.lastdiridx = -1;
	  rpm_iterate_filelist(handle, iterflags, findfileconflicts2_cb, &cbdata);
          for (ii = 0; ii < pend; ii++)
	    for (jj = pend; jj < cbdata.files.count; jj++)
	      {
		char *fsi = (char *)cbdata.filesspace + cbdata.files.elements[ii];
		char *fsj = (char *)cbdata.filesspace + cbdata.files.elements[jj];
		if (strcmp(fsi + 34, fsj + 34))
		  continue;	/* different file names */
		if (!strcmp(fsi, fsj))
		  continue;	/* md5 sum matches */
		if (obsoleteusescolors && fsi[33] && fsj[33] && (fsi[33] & fsj[33]) == 0)
		  continue;	/* colors do not conflict */
		queue_push(conflicts, pool_str2id(pool, fsi + 34, 1));
		queue_push(conflicts, p);
		queue_push(conflicts, pool_str2id(pool, fsi, 1));
		queue_push(conflicts, pool_str2id(pool, fsj + 34, 1));
		queue_push(conflicts, q);
		queue_push(conflicts, pool_str2id(pool, fsj, 1));
	      }
	  queue_truncate(&cbdata.files, pend);
	}
    }
  cbdata.filesspace = solv_free(cbdata.filesspace);
  cbdata.filesspacen = 0;
  queue_free(&cbdata.lookat);
  queue_free(&cbdata.files);
  POOL_DEBUG(SOLV_DEBUG_STATS, "candidate check took %d ms\n", solv_timems(now));
  if (conflicts->count > 6)
    solv_sort(conflicts->elements, conflicts->count / 6, 6 * sizeof(Id), conflicts_cmp, pool);
  POOL_DEBUG(SOLV_DEBUG_STATS, "found %d file conflicts\n", conflicts->count / 6);
  POOL_DEBUG(SOLV_DEBUG_STATS, "file conflict detection took %d ms\n", solv_timems(start));
  return conflicts->count;
}

