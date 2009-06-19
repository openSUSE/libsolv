#include <stdio.h>
#include <sys/stat.h>

#include "pool.h"
#include "repo.h"
#include "hash.h"
#include "repo_rpmdb.h"

struct cbdata {
  Pool *pool;
  Queue lookat;
  Queue lookat_dir;

  Hashval *cflmap;
  Hashmask cflmapn;
  unsigned int cflmapused;

  Hashval *dirmap;
  Hashmask dirmapn;
  unsigned int dirmapused;

  Hashval idx;
  unsigned int hx;

  Queue files;
  unsigned char *filesspace;
  unsigned int filesspacen;
};

#define FILESSPACE_BLOCK 255

static Hashval *
doublehash(Hashval *map, Hashmask *mapnp)
{
  Hashmask mapn = *mapnp;
  Hashmask i, hx, qx, h, hh;
  Hashmask nn = (mapn + 1) * 2 - 1;
  Hashmask *m;

  m = sat_calloc(nn + 1, 2 * sizeof(Id));
  for (i = 0; i <= mapn; i++)
    {
      hx = map[2 * i];
      if (!hx)
	continue;
      h = hx & nn;
      hh = HASHCHAIN_START;
      for (;;)
	{
	  qx = m[2 * h];
	  if (!qx)
	    break;
	  h = HASHCHAIN_NEXT(h, hh, nn);
	}
      m[2 * h] = hx;
      m[2 * h + 1] = map[2 * i + 1];
    }
  sat_free(map);
  *mapnp = nn;
  return m;
}

static void
finddirs_cb(void *cbdatav, char *fn, int fmode, char *md5)
{
  struct cbdata *cbdata = cbdatav;
  Hashmask h, hh, hx, qx;
  Hashval idx = cbdata->idx;

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
      cbdata->dirmap[2 * h] = hx;
      cbdata->dirmap[2 * h + 1] = idx;
      cbdata->dirmapused++;
      if (cbdata->dirmapused * 2 > cbdata->dirmapn)
	cbdata->dirmap = doublehash(cbdata->dirmap, &cbdata->dirmapn);
      return;
    }
  if (cbdata->dirmap[2 * h + 1] == idx)
    return;
  /* found a conflict, this dir is used in multiple packages */
  cbdata->dirmap[2 * h + 1] = -1;
}

static inline int
isindirmap(struct cbdata *cbdata, Hashmask hx)
{
  Hashmask h, hh, qx;

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
findfileconflicts_cb(void *cbdatav, char *fn, int fmode, char *md5)
{
  struct cbdata *cbdata = cbdatav;
  int isdir = S_ISDIR(fmode);
  char *dp;
  Hashval idx, qidx;
  Hashmask qx, h, hx, hh, dhx;

  idx = cbdata->idx;

  dp = strrchr(fn, '/');
  if (!dp)
    return;
  dhx = strnhash(fn, dp + 1 - fn);
  if (!dhx)
    dhx = 1 + dp + 1 - fn;
#if 1
  if (!isindirmap(cbdata, dhx))
    return;
#endif

  hx = strhash_cont(dp + 1, dhx);
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
      cbdata->cflmap[2 * h] = hx;
      cbdata->cflmap[2 * h + 1] = (isdir ? ~idx : idx);
      cbdata->cflmapused++;
      if (cbdata->cflmapused * 2 > cbdata->cflmapn)
	cbdata->cflmap = doublehash(cbdata->cflmap, &cbdata->cflmapn);
      return;
    }
  qidx = cbdata->cflmap[2 * h + 1];
  if ((int)qidx < 0)
    {
      int i;
      qidx = ~qidx;
      if (isdir)
	{
	  /* delay the conflict */
          queue_push2(&cbdata->lookat_dir, hx, qidx);
          queue_push2(&cbdata->lookat_dir, hx, idx);
	  return;
	}
      cbdata->cflmap[2 * h + 1] = qidx;
      for (i = 0; i < cbdata->lookat_dir.count; i += 2)
	if (cbdata->lookat_dir.elements[i] == hx)
	  queue_push2(&cbdata->lookat, hx, cbdata->lookat_dir.elements[i + 1]);
    }
  if (qidx == idx)
    return;	/* no conflicts with ourself, please */
  queue_push2(&cbdata->lookat, hx, qidx);
  queue_push2(&cbdata->lookat, hx, idx);
}

static inline void
addfilesspace(struct cbdata *cbdata, unsigned char *data, int len)
{
  cbdata->filesspace = sat_extend(cbdata->filesspace, cbdata->filesspacen, len, 1, FILESSPACE_BLOCK);
  memcpy(cbdata->filesspace + cbdata->filesspacen, data, len);
  cbdata->filesspacen += len;
}

static void
findfileconflicts2_cb(void *cbdatav, char *fn, int fmode, char *md5)
{
  struct cbdata *cbdata = cbdatav;
  unsigned int hx = strhash(fn);
  char md5padded[33];

  if (!hx)
    hx = strlen(fn) + 1;
  if (hx != cbdata->hx)
    return;
  strncpy(md5padded, md5, 32);
  md5padded[32] = 0;
  // printf("%d, hx %x -> %s   %d %s\n", cbdata->idx, hx, fn, fmode, md5);
  queue_push(&cbdata->files, cbdata->filesspacen);
  addfilesspace(cbdata, (unsigned char *)md5padded, 33);
  addfilesspace(cbdata, (unsigned char *)fn, strlen(fn) + 1);
}

static int cand_sort(const void *ap, const void *bp, void *dp)
{
  const Id *a = ap;
  const Id *b = bp;

  unsigned int ax = (unsigned int)a[0];
  unsigned int bx = (unsigned int)b[0];
  if (ax < bx)
    return -1;
  if (ax > bx)
    return 1;
  return (a[1] < 0 ? -a[1] : a[1]) - (b[1] < 0 ? -b[1] : b[1]);
}

static int conflicts_cmp(const void *ap, const void *bp, void *dp)
{
  Pool *pool = dp;
  const Id *a = ap;
  const Id *b = bp;
  if (a[0] != b[0])
    return strcmp(id2str(pool, a[0]), id2str(pool, b[0]));
  if (a[1] != b[1])
    return a[1] - b[1];
  if (a[3] != b[3])
    return a[3] - b[3];
  return 0;
}

int
pool_findfileconflicts(Pool *pool, Queue *pkgs, Queue *conflicts, void *(*handle_cb)(Pool *, Id, void *) , void *handle_cbdata)
{
  int i, j, cflmapn;
  unsigned int hx;
  struct cbdata cbdata;
  unsigned int now, start;
  void *handle;

  queue_empty(conflicts);
  if (!pkgs->count)
    return 0;

  now = start = sat_timems(0);
  printf("packages: %d\n", pkgs->count);

  memset(&cbdata, 0, sizeof(cbdata));
  cbdata.pool = pool;
  queue_init(&cbdata.lookat);
  queue_init(&cbdata.lookat_dir);
  queue_init(&cbdata.files);

  /* avarage file list size: 200 files per package */
  /* avarage dir count: 20 dirs per package */

  /* first pass: scan dirs */
  cflmapn = pkgs->count * 64;
  while ((cflmapn & (cflmapn - 1)) != 0)
    cflmapn = cflmapn & (cflmapn - 1);
  cbdata.dirmap = sat_calloc(cflmapn, 2 * sizeof(Id));
  cbdata.dirmapn = cflmapn - 1;	/* make it a mask */
  for (i = 0; i < pkgs->count; i++)
    {
      Id p = pkgs->elements[i];
      cbdata.idx = p;
      handle = (*handle_cb)(pool, p, handle_cbdata);
      if (handle)
        rpm_iterate_filelist(handle, RPM_ITERATE_FILELIST_ONLYDIRS, finddirs_cb, &cbdata);
    }

  printf("dirmap size: %d used %d\n", cbdata.dirmapn + 1, cbdata.dirmapused);
  printf("dirmap memory usage: %d K\n", (cbdata.dirmapn + 1) * 2 * (int)sizeof(Id) / 1024);
  printf("dirmap creation took %d ms\n", sat_timems(now));

  /* second pass: scan files */
  now = sat_timems(0);
  cflmapn = pkgs->count * 128;
  while ((cflmapn & (cflmapn - 1)) != 0)
    cflmapn = cflmapn & (cflmapn - 1);
  cbdata.cflmap = sat_calloc(cflmapn, 2 * sizeof(Id));
  cbdata.cflmapn = cflmapn - 1;	/* make it a mask */
  for (i = 0; i < pkgs->count; i++)
    {
      Id p = pkgs->elements[i];
      cbdata.idx = p;
      handle = (*handle_cb)(pool, p, handle_cbdata);
      if (handle)
        rpm_iterate_filelist(handle, 0, findfileconflicts_cb, &cbdata);
    }

  printf("filemap size: %d used %d\n", cbdata.cflmapn + 1, cbdata.cflmapused);
  printf("filemap memory usage: %d K\n", (cbdata.cflmapn + 1) * 2 * (int)sizeof(Id) / 1024);
  printf("filemap creation took %d ms\n", sat_timems(now));

  cbdata.dirmap = sat_free(cbdata.dirmap);
  cbdata.dirmapn = 0;
  cbdata.dirmapused = 0;
  cbdata.cflmap = sat_free(cbdata.cflmap);
  cbdata.cflmapn = 0;
  cbdata.cflmapused = 0;

  now = sat_timems(0);
  printf("lookat_dir size: %d\n", cbdata.lookat_dir.count);
  queue_free(&cbdata.lookat_dir);
  sat_sort(cbdata.lookat.elements, cbdata.lookat.count / 2, sizeof(Id) * 2, &cand_sort, pool);
  /* unify */
  for (i = j = 0; i < cbdata.lookat.count; i += 2)
    {
      hx = cbdata.lookat.elements[i];
      Id p = cbdata.lookat.elements[i + 1];
      if (j && hx == cbdata.lookat.elements[j - 2] && p == cbdata.lookat.elements[j - 1])
	continue;
      cbdata.lookat.elements[j++] = hx;
      cbdata.lookat.elements[j++] = p;
    }
  printf("candidates: %d\n", cbdata.lookat.count / 2);

  /* third pass: scan candidates */
  for (i = 0; i < cbdata.lookat.count - 2; i += 2)
    {
      int pend, ii, jj;
      Id p = cbdata.lookat.elements[i + 1];

      hx = cbdata.lookat.elements[i];
      if (cbdata.lookat.elements[i + 2] != hx)
	continue;	/* no package left */
      queue_empty(&cbdata.files);
      cbdata.filesspace = sat_free(cbdata.filesspace);
      cbdata.filesspacen = 0;

      cbdata.idx = p;
      cbdata.hx = cbdata.lookat.elements[i];
      handle = (*handle_cb)(pool, p, handle_cbdata);
      if (!handle)
	continue;
      rpm_iterate_filelist(handle, RPM_ITERATE_FILELIST_WITHMD5, findfileconflicts2_cb, &cbdata);

      pend = cbdata.files.count;
      for (j = i + 2; j < cbdata.lookat.count && cbdata.lookat.elements[j] == hx; j++)
	{
	  Id q = cbdata.lookat.elements[j + 1];
	  cbdata.idx = q;
	  handle = (*handle_cb)(pool, q, handle_cbdata);
	  if (!handle)
	    continue;
	  rpm_iterate_filelist(handle, RPM_ITERATE_FILELIST_WITHMD5, findfileconflicts2_cb, &cbdata);
          for (ii = 0; ii < pend; ii++)
	    for (jj = pend; jj < cbdata.files.count; jj++)
	      {
		if (strcmp((char *)cbdata.filesspace + cbdata.files.elements[ii] + 33, (char *)cbdata.filesspace + cbdata.files.elements[jj] + 33))
		  continue;
		if (!strcmp((char *)cbdata.filesspace + cbdata.files.elements[ii], (char *)cbdata.filesspace + cbdata.files.elements[jj]))
		  continue;
		queue_push(conflicts, str2id(pool, (char *)cbdata.filesspace + cbdata.files.elements[ii] + 33, 1));
		queue_push(conflicts, p);
		queue_push(conflicts, str2id(pool, (char *)cbdata.filesspace + cbdata.files.elements[ii], 1));
		queue_push(conflicts, q);
		queue_push(conflicts, str2id(pool, (char *)cbdata.filesspace + cbdata.files.elements[jj], 1));
	      }
	}
    }
  cbdata.filesspace = sat_free(cbdata.filesspace);
  cbdata.filesspacen = 0;
  printf("candidate check took %d ms\n", sat_timems(now));
  if (conflicts->count > 5)
    sat_sort(conflicts->elements, conflicts->count / 5, 5 * sizeof(Id), conflicts_cmp, pool);
  (*handle_cb)(pool, 0, handle_cbdata);
  printf("conflict detection took %d ms\n", sat_timems(start));
  return conflicts->count;
}

