/*
 * Copyright (c) 2009-2013, Novell Inc.
 *
 * This program is licensed under the BSD license, read LICENSE.BSD
 * for further information
 */

#include <stdio.h>
#include <sys/stat.h>
#include <unistd.h>

#include "pool.h"
#include "repo.h"
#include "hash.h"
#include "repo_rpmdb.h"
#include "pool_fileconflicts.h"

struct cbdata {
  Pool *pool;
  int create;
  int aliases;

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
  int lastdiridxbad;

  Id idx;	/* index of package we're looking at */

  unsigned char *filesspace;
  unsigned int filesspacen;

  Hashtable normap;
  Hashval normapn;
  unsigned int normapused;
  Queue norq;

  Hashtable statmap;
  Hashval statmapn;
  unsigned int statmapused;

  int usestat;
  int statsmade;

  const char *rootdir;
  int rootdirl;

  char *canonspace;
  int canonspacen;

  Hashtable fetchmap;
  Hashval fetchmapn;
  Map fetchdirmap;
  int fetchdirmapn;
  Queue newlookat;
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

/* first pass for non-alias mode:
 * create hash (dhx, idx) of directories that may have conflicts.
 * also create map "ixdmap" of packages involved
 */
static void
finddirs_cb(void *cbdatav, const char *fn, struct filelistinfo *info)
{
  struct cbdata *cbdata = cbdatav;
  Hashval h, hh;
  Id dhx, qx;
  Id oidx, idx = cbdata->idx;

  dhx = strhash(fn);
  if (!dhx)
    dhx = strlen(fn) + 1;	/* make sure dhx is not zero */
  h = dhx & cbdata->dirmapn;
  hh = HASHCHAIN_START;
  for (;;)
    {
      qx = cbdata->dirmap[2 * h];
      if (!qx)
	break;
      if (qx == dhx)
	break;
      h = HASHCHAIN_NEXT(h, hh, cbdata->dirmapn);
    }
  if (!qx)
    {
      /* a miss */
      if (!cbdata->create)
	return;
      cbdata->dirmap[2 * h] = dhx;
      cbdata->dirmap[2 * h + 1] = idx;
      if (++cbdata->dirmapused * 2 > cbdata->dirmapn)
	cbdata->dirmap = growhash(cbdata->dirmap, &cbdata->dirmapn);
      return;
    }
  /* we saw this dir before */
  oidx = cbdata->dirmap[2 * h + 1];
  if (oidx == idx)
    return;
  /* found a conflict, this dir may be used in multiple packages */
  if (oidx != -1)
    {
      MAPSET(&cbdata->idxmap, oidx);
      cbdata->dirmap[2 * h + 1] = -1;	/* mark as "multiple packages" */
      cbdata->dirconflicts++;
    }
  MAPSET(&cbdata->idxmap, idx);
}

/* check if a dhx value is marked as "multiple" in the dirmap created by finddirs_cb */
static inline int
isindirmap(struct cbdata *cbdata, Id dhx)
{
  Hashval h, hh;
  Id qx;

  h = dhx & cbdata->dirmapn;
  hh = HASHCHAIN_START;
  for (;;)
    {
      qx = cbdata->dirmap[2 * h];
      if (!qx)
	return 0;
      if (qx == dhx)
	return cbdata->dirmap[2 * h + 1] == -1 ? 1 : 0;
      h = HASHCHAIN_NEXT(h, hh, cbdata->dirmapn);
    }
}

/* collect all possible file conflicts candidates in cbdata->lookat */
/* algorithm: hash file name into hx. Use cflmap the check if we have seen
 * this value before. If yes, we have a file conflict candidate. */
/* we also do extra work to ignore all-directory conflicts */
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

  /* check if the directory is marked as "multiple" in the dirmap */
  /* this mirrors the "if (!dhx) dhx = strlen(fn) + 1" used in  finddirs_cb */
  if (!isindirmap(cbdata, dhx ? dhx : dp - fn + 1))
    return;

  hx = strhash_cont(dp, dhx);	/* extend hash to complete file name */
  if (!hx)
    hx = strlen(fn) + 1;	/* make sure hx is not zero */

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
      if (++cbdata->cflmapused * 2 > cbdata->cflmapn)
	cbdata->cflmap = growhash(cbdata->cflmap, &cbdata->cflmapn);
      return;
    }
  /* we have seen this hx before */
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
	  {
	    queue_push2(&cbdata->lookat, hx, cbdata->lookat_dir.elements[i + 1]);
	    queue_push2(&cbdata->lookat, 0, 0);
	  }
    }
  else if (oidx == idx)
    return;	/* no conflicts with ourself, please */
  queue_push2(&cbdata->lookat, hx, oidx);
  queue_push2(&cbdata->lookat, 0, 0);
  queue_push2(&cbdata->lookat, hx, idx);
  queue_push2(&cbdata->lookat, 0, 0);
}

/* same as findfileconflicts_cb, but
 * - hashes with just the basename
 * - sets idx in a map instead of pushing to lookat
 * - sets the hash element to -1 ("multiple") if there may be a conflict
 * we then use findfileconflicts_alias_cb as second step to generate the candidates.
 * we do it this way because normailzing file names is expensive and thus we
 * only want to do it for entries marked as "multiple"
 */
static void
findfileconflicts_basename_cb(void *cbdatav, const char *fn, struct filelistinfo *info)
{
  struct cbdata *cbdata = cbdatav;
  int isdir = S_ISDIR(info->mode);
  const char *dp;
  Id idx, oidx;
  Id hx, qx;
  Hashval h, hh;

  idx = cbdata->idx;

  if (!info->dirlen)
    return;
  dp = fn + info->dirlen;
  hx = strhash(dp);
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
      cbdata->cflmap[2 * h + 1] = (isdir ? -idx - 2 : idx);
      if (++cbdata->cflmapused * 2 > cbdata->cflmapn)
	cbdata->cflmap = growhash(cbdata->cflmap, &cbdata->cflmapn);
      return;
    }
  oidx = cbdata->cflmap[2 * h + 1];
  if (oidx < -1)
    {
      int i;
      if (isdir)
	{
	  /* both are directories. delay the conflict, keep oidx in slot */
          queue_push2(&cbdata->lookat_dir, hx, idx);
	  return;
	}
      oidx = -idx - 2;
      /* now have file, had directories before. */
      cbdata->cflmap[2 * h + 1] = oidx;	/* make it a file */
      /* dump all delayed directory hits for hx */
      for (i = 0; i < cbdata->lookat_dir.count; i += 2)
	if (cbdata->lookat_dir.elements[i] == hx)
	  MAPSET(&cbdata->idxmap, cbdata->lookat_dir.elements[i + 1]);
    }
  else if (oidx == idx)
    return;	/* no conflicts with ourself, please */
  if (oidx >= 0)
    MAPSET(&cbdata->idxmap, oidx);
  MAPSET(&cbdata->idxmap, idx);
  if (oidx != -1)
    cbdata->cflmap[2 * h + 1] = -1;
}

static inline Id
addfilesspace(struct cbdata *cbdata, int len)
{
  unsigned int off = cbdata->filesspacen;
  cbdata->filesspace = solv_extend(cbdata->filesspace, cbdata->filesspacen, len, 1, FILESSPACE_BLOCK);
  cbdata->filesspacen += len;
  return off;
}

static Id
unifywithstat(struct cbdata *cbdata, Id diroff, int dirl)
{
  struct stat stb;
  int i;
  Hashval h, hh;
  Id hx, qx;
  Id nspaceoff;
  unsigned char statdata[16 + sizeof(stb.st_dev) + sizeof(stb.st_ino)];

  if (dirl > 1 && cbdata->filesspace[diroff + dirl - 1] == '/')
    cbdata->filesspace[diroff + dirl - 1] = 0;
  cbdata->statsmade++;
  i = stat((char *)cbdata->filesspace + diroff, &stb);
  if (dirl > 1 && cbdata->filesspace[diroff + dirl - 1] == 0)
    cbdata->filesspace[diroff + dirl - 1] = '/';
  if (i)
    return diroff;
  memset(statdata, 0, 16);
  memcpy(statdata + 8, &stb.st_dev, sizeof(stb.st_dev));
  memcpy(statdata, &stb.st_ino, sizeof(stb.st_ino));
  hx = 0;
  for (i = 15; i >= 0; i--)
    hx = (unsigned int)hx * 13 + statdata[i];
  h = hx & cbdata->statmapn;
  hh = HASHCHAIN_START;
  for (;;)
    {
      qx = cbdata->statmap[2 * h];
      if (!qx)
	break;
      if (qx == hx)
	{
	  Id off = cbdata->statmap[2 * h + 1];
	  char *dp = (char *)cbdata->filesspace + cbdata->norq.elements[off];
	  if (!memcmp(dp, statdata, 16))
	    return cbdata->norq.elements[off + 1];
	}
      h = HASHCHAIN_NEXT(h, hh, cbdata->statmapn);
    }
  /* new stat result. work. */
  nspaceoff = addfilesspace(cbdata, 16);
  memcpy(cbdata->filesspace + nspaceoff, statdata, 16);
  queue_push2(&cbdata->norq, nspaceoff, nspaceoff);
  cbdata->statmap[2 * h] = hx;
  cbdata->statmap[2 * h + 1] = cbdata->norq.count - 2;
  if (++cbdata->statmapused * 2 > cbdata->statmapn)
    cbdata->statmap = growhash(cbdata->statmap, &cbdata->statmapn);
  return nspaceoff;
}

/* forward declaration */
static Id normalizedir(struct cbdata *cbdata, const char *dir, int dirl, Id hx, int create);

static Id
unifywithcanon(struct cbdata *cbdata, Id diroff, int dirl)
{
  Id dirnameid;
  int i, l, ll, lo;
  struct stat stb;

#if 0
  printf("UNIFY %.*s\n", dirl, (char *)cbdata->filesspace + diroff);
#endif
  if (!dirl || cbdata->filesspace[diroff] != '/')
    return diroff;
  /* strip / at end*/
  while (dirl && cbdata->filesspace[diroff + dirl - 1] == '/')
    dirl--;
  if (!dirl)
    return diroff;

  /* find dirname */
  for (i = dirl - 1; i > 0; i--)
    if (cbdata->filesspace[diroff + i] == '/')
      break;
  i++;				/* include trailing / */

  /* normalize dirname */
  dirnameid = normalizedir(cbdata, (char *)cbdata->filesspace + diroff, i, strnhash((char *)cbdata->filesspace + diroff, i), 1);
  if (dirnameid == -1)
    return diroff;		/* hit "in progress" marker, some cyclic link */

  /* sanity check result */
  if (cbdata->filesspace[dirnameid] != '/')
    return diroff;		/* hmm */
  l = strlen((char *)cbdata->filesspace + dirnameid);
  if (l && cbdata->filesspace[dirnameid + l - 1] != '/')
    return diroff;		/* hmm */

  /* special handling for "." and ".." basename */
  if (cbdata->filesspace[diroff + i] == '.')
    {
      if (dirl - i == 1)
	return dirnameid;
      if (dirl - i == 2 && cbdata->filesspace[diroff + i + 1] == '.')
	{
	  if (l <= 2)
	    return dirnameid;	/* we hit our root */
	  for (i = l - 2; i > 0; i--)
	    if (cbdata->filesspace[dirnameid + i] == '/')
	      break;
	  i++;	/* include trailing / */
	  dirnameid = normalizedir(cbdata, (char *)cbdata->filesspace + dirnameid, i, strnhash((char *)cbdata->filesspace + dirnameid, i), 1);
	  return dirnameid == -1 ? diroff : dirnameid;
	}
    }

  /* append basename to normalized dirname */
  if (cbdata->rootdirl + l + dirl - i + 1 > cbdata->canonspacen)
    {
      cbdata->canonspacen = cbdata->rootdirl + l + dirl - i + 20;
      cbdata->canonspace = solv_realloc(cbdata->canonspace, cbdata->canonspacen);
      strcpy(cbdata->canonspace, cbdata->rootdir);
    }
  strcpy(cbdata->canonspace + cbdata->rootdirl, (char *)cbdata->filesspace + dirnameid);
  strncpy(cbdata->canonspace + cbdata->rootdirl + l, (char *)cbdata->filesspace + diroff + i, dirl - i);
  cbdata->canonspace[cbdata->rootdirl + l + dirl - i] = 0;

#if 0
  printf("stat()ing %s\n", cbdata->canonspace);
#endif
  cbdata->statsmade++;
  if (lstat(cbdata->canonspace, &stb) != 0 || !S_ISLNK(stb.st_mode))
    {
      /* not a symlink or stat failed, have new canon entry */
      diroff = addfilesspace(cbdata, l + dirl - i + 2);
      strcpy((char *)cbdata->filesspace + diroff, cbdata->canonspace + cbdata->rootdirl);
      l += dirl - i;
      /* add trailing / */
      if (cbdata->filesspace[diroff + l - 1] != '/')
	{
	  cbdata->filesspace[diroff + l++] = '/';
	  cbdata->filesspace[diroff + l] = 0;
	}
      /* call normalizedir on new entry for unification purposes */
      dirnameid = normalizedir(cbdata, (char *)cbdata->filesspace + diroff, l, strnhash((char *)cbdata->filesspace + diroff, l), 1);
      return dirnameid == -1 ? diroff : dirnameid;
    }
  /* oh no, a symlink! follow */
  lo = cbdata->rootdirl + l + dirl - i + 1;
  if (lo + stb.st_size + 2 > cbdata->canonspacen)
    {
      cbdata->canonspacen = lo + stb.st_size + 20;
      cbdata->canonspace = solv_realloc(cbdata->canonspace, cbdata->canonspacen);
    }
  ll = readlink(cbdata->canonspace, cbdata->canonspace + lo, stb.st_size);
  if (ll < 0 || ll > stb.st_size)
    return diroff;		/* hmm */
  if (ll == 0)
    return dirnameid;		/* empty means current dir */
  if (cbdata->canonspace[lo + ll - 1] != '/')
    cbdata->canonspace[lo + ll++] = '/';	/* add trailing / */
  cbdata->canonspace[lo + ll] = 0;		/* zero terminate */
  if (cbdata->canonspace[lo] != '/')
    {
      /* relative link, concatenate to dirname */
      memmove(cbdata->canonspace + cbdata->rootdirl + l, cbdata->canonspace + lo, ll + 1);
      lo = cbdata->rootdirl;
      ll += l;
    }
  dirnameid = normalizedir(cbdata, cbdata->canonspace + lo, ll, strnhash(cbdata->canonspace + lo, ll), 1);
  return dirnameid == -1 ? diroff : dirnameid;
}

/*
 * map a directory (containing a trailing /) into a number.
 * for unifywithstat this is the offset to the 16 byte stat result.
 * for unifywithcanon this is the offset to the normailzed dir.
 */
static Id
normalizedir(struct cbdata *cbdata, const char *dir, int dirl, Id hx, int create)
{
  Hashval h, hh;
  Id qx;
  Id nspaceoff;
  int mycnt;

  if (!hx)
    hx = dirl + 1;
  h = hx & cbdata->normapn;
  hh = HASHCHAIN_START;
  for (;;)
    {
      qx = cbdata->normap[2 * h];
      if (!qx)
	break;
      if (qx == hx)
	{
	  Id off = cbdata->normap[2 * h + 1];
	  char *dp = (char *)cbdata->filesspace + cbdata->norq.elements[off];
	  if (!strncmp(dp, dir, dirl) && dp[dirl] == 0)
	    return cbdata->norq.elements[off + 1];
	}
      h = HASHCHAIN_NEXT(h, hh, cbdata->normapn);
    }
  if (!create)
    return 0;
  /* new dir. work. */
  if (dir >= (const char *)cbdata->filesspace && dir < (const char *)cbdata->filesspace + cbdata->filesspacen)
    {
      /* can happen when called from unifywithcanon */
      Id off = dir - (const char *)cbdata->filesspace;
      nspaceoff = addfilesspace(cbdata, dirl + 1);
      dir = (const char *)cbdata->filesspace + off;
    }
  else
    nspaceoff = addfilesspace(cbdata, dirl + 1);
  if (dirl)
    memcpy(cbdata->filesspace + nspaceoff, dir, dirl);
  cbdata->filesspace[nspaceoff + dirl] = 0;
  mycnt = cbdata->norq.count;
  queue_push2(&cbdata->norq, nspaceoff, -1);	/* -1: in progress */
  cbdata->normap[2 * h] = hx;
  cbdata->normap[2 * h + 1] = mycnt;
  if (++cbdata->normapused * 2 > cbdata->normapn)
    cbdata->normap = growhash(cbdata->normap, &cbdata->normapn);
  /* unify */
  if (cbdata->usestat)
    nspaceoff = unifywithstat(cbdata, nspaceoff, dirl);
  else
    nspaceoff = unifywithcanon(cbdata, nspaceoff, dirl);
  cbdata->norq.elements[mycnt + 1] = nspaceoff;	/* patch in result */
#if 0
  if (!cbdata->usestat)
    printf("%s normalized to %d: %s\n", cbdata->filesspace + cbdata->norq.elements[mycnt], nspaceoff, cbdata->filesspace + nspaceoff);
#endif
  return nspaceoff;
}

/* collect all candidates for cflmap entries marked as "multiple" */
static void
findfileconflicts_alias_cb(void *cbdatav, const char *fn, struct filelistinfo *info)
{
  int isdir = S_ISDIR(info->mode);
  struct cbdata *cbdata = cbdatav;
  const char *dp;
  Id idx, dirid;
  Id hx, qx;
  Hashval h, hh;

  idx = cbdata->idx;

  if (!info->dirlen)
    return;
  if (info->diridx != cbdata->lastdiridx)
    {
      cbdata->lastdiridx = info->diridx;
      cbdata->lastdirhash = 0;
    }
  dp = fn + info->dirlen;
  hx = strhash(dp);
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
  if (!qx || cbdata->cflmap[2 * h + 1] != -1)
    return;
  /* found entry marked as "multiple", recored as conflict candidate */
  if (!cbdata->lastdirhash)
    cbdata->lastdirhash = strnhash(fn, dp - fn);
  dirid = normalizedir(cbdata, fn, dp - fn, cbdata->lastdirhash, 1);
  queue_push2(&cbdata->lookat, hx, idx);
  queue_push2(&cbdata->lookat, cbdata->lastdirhash, isdir ? -dirid : dirid);
}

/* turn (hx, idx, dhx, dirid) entries into (hx, idx, off, dirid) entries,
 * where off is an offset into the filespace block */
static void
findfileconflicts_expand_cb(void *cbdatav, const char *fn, struct filelistinfo *info)
{
  struct cbdata *cbdata = cbdatav;
  Id hx, dhx;
  Hashval h, hh;
  const char *dp;
  char md5padded[34];
  Id off, dirid;
  int i;

  if (!info->dirlen)
    return;
  dp = fn + info->dirlen;
  if (info->diridx != cbdata->lastdiridx)
    {
      cbdata->lastdiridx = info->diridx;
      cbdata->lastdirhash = strnhash(fn, dp - fn);
      if (cbdata->aliases)
        cbdata->lastdiridxbad = MAPTST(&cbdata->fetchdirmap, cbdata->lastdirhash & cbdata->fetchdirmapn) ? 0 : 1;
    }
  if (cbdata->lastdiridxbad)
    return;
  if (cbdata->aliases)
    {
      hx = strhash(dp);
      dhx = cbdata->lastdirhash;
      dirid =  normalizedir(cbdata, fn, dp - fn, dhx, 0);
    }
  else
    {
      hx = cbdata->lastdirhash;
      hx = strhash_cont(dp, hx);
      dhx = dirid = 0;
    }
  if (!hx)
    hx = strlen(fn) + 1;

  h = (hx ^ (dirid * 37)) & cbdata->fetchmapn;
  hh = HASHCHAIN_START;
  for (;;)
    {
      i = cbdata->fetchmap[h];
      if (!i)
	break;
      if (cbdata->lookat.elements[i - 1] == hx && cbdata->lookat.elements[i + 2] == dirid && cbdata->lookat.elements[i + 1] == dhx)
	{
          /* printf("%d, hx %x dhx %x dirid %d -> %s   %d %s %d\n", cbdata->idx, hx, dhx, dirid, fn, info->mode, info->digest, info->color); */
	  strncpy(md5padded, info->digest, 32);
	  md5padded[32] = 0;
	  md5padded[33] = info->color;
	  off = addfilesspace(cbdata, strlen(fn) + (34 + 1));
	  memcpy(cbdata->filesspace + off, (unsigned char *)md5padded, 34);
          strcpy((char *)cbdata->filesspace + off + 34, fn);
	  queue_push2(&cbdata->lookat, hx, cbdata->idx);
	  queue_push2(&cbdata->lookat, off, dirid);
	}
      h = HASHCHAIN_NEXT(h, hh, cbdata->fetchmapn);
    }
}

static int
lookat_idx_cmp(const void *ap, const void *bp, void *dp)
{
  const Id *a = ap, *b = bp;
  unsigned int ahx, bhx;
  if (a[1] - b[1] != 0)		/* idx */
    return a[1] - b[1];
  if (a[3] - b[3] != 0)		/* dirid */
    return a[3] - b[3];
  ahx = (unsigned int)a[0];	/* can be < 0 */
  bhx = (unsigned int)b[0];
  if (ahx != bhx)
    return ahx < bhx ? -1 : 1;
  ahx = (unsigned int)a[2];	/* dhx */
  bhx = (unsigned int)b[2];
  if (ahx != bhx)
    return ahx < bhx ? -1 : 1;
  return 0;
}

static int
lookat_hx_cmp(const void *ap, const void *bp, void *dp)
{
  const Id *a = ap, *b = bp;
  unsigned int ahx, bhx;
  Id adirid, bdirid;
  ahx = (unsigned int)a[0];	/* can be < 0 */
  bhx = (unsigned int)b[0];
  if (ahx != bhx)
    return ahx < bhx ? -1 : 1;
  adirid = a[3] < 0 ? -a[3] : a[3];
  bdirid = b[3] < 0 ? -b[3] : b[3];
  if (adirid - bdirid != 0)	/* dirid */
    return adirid - bdirid;
  if (a[3] != b[3])
    return a[3] > 0 ? -1 : 1; 	/* bring positive dirids to front */
  if (a[1] - b[1] != 0)		/* idx */
    return a[1] - b[1];
  ahx = (unsigned int)a[2];	/* dhx */
  bhx = (unsigned int)b[2];
  if (ahx != bhx)
    return ahx < bhx ? -1 : 1;
  return 0;
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

static void
iterate_solvable_dirs(Pool *pool, Id p, void (*cb)(void *, const char *, struct filelistinfo *), void *cbdata)
{
  Repodata *lastdata = 0;
  Id lastdirid = -1;
  Dataiterator di;

  dataiterator_init(&di, pool, 0, p, SOLVABLE_FILELIST, 0, SEARCH_COMPLETE_FILELIST);
  while (dataiterator_step(&di))
    {
      if (di.data == lastdata && di.kv.id == lastdirid)
	continue;
      lastdata = di.data;
      lastdirid = di.kv.id;
      cb(cbdata, repodata_dir2str(di.data, di.kv.id, ""), 0);
    }
  dataiterator_free(&di);
}

/* before calling the expensive findfileconflicts_cb we check if any of
 * the files match. This only makes sense when cbdata->create is off.
 */
static int
precheck_solvable_files(struct cbdata *cbdata, Pool *pool, Id p)
{
  Dataiterator di;
  Id hx, qx;
  Hashval h, hh;
  int found = 0;
  int aliases = cbdata->aliases;
  unsigned int lastdirid = -1;
  Hashval lastdirhash = 0;
  int lastdirlen = 0;
  int checkthisdir = 0;
  Repodata *lastrepodata = 0;

  dataiterator_init(&di, pool, 0, p, SOLVABLE_FILELIST, 0, SEARCH_COMPLETE_FILELIST);
  while (dataiterator_step(&di))
    {
      if (aliases)
	{
	  /* hash just the basename */
	  hx = strhash(di.kv.str);
	  if (!hx)
	    hx = strlen(di.kv.str) + 1;
	}
      else
	{
	  /* hash the full path */
	  if (di.data != lastrepodata || di.kv.id != lastdirid)
	    {
	      const char *dir;
	      lastrepodata = di.data;
	      lastdirid = di.kv.id;
	      dir = repodata_dir2str(lastrepodata, lastdirid, "");
	      lastdirlen = strlen(dir);
	      lastdirhash = strhash(dir);
	      checkthisdir =  isindirmap(cbdata, lastdirhash ? lastdirhash : lastdirlen + 1);
	    }
	  if (!checkthisdir)
	    continue;
	  hx = strhash_cont(di.kv.str, lastdirhash);
	  if (!hx)
	    hx = lastdirlen + strlen(di.kv.str) + 1;
	}
      h = hx & cbdata->cflmapn;
      hh = HASHCHAIN_START;
      for (;;)
	{
	  qx = cbdata->cflmap[2 * h];
	  if (!qx)
	    break;
	  if (qx == hx)
	    {
	      found = 1;
	      break;
	    }
	  h = HASHCHAIN_NEXT(h, hh, cbdata->cflmapn);
	}
      if (found)
	break;
    }
  dataiterator_free(&di);
  return found;
}


/* pool_findfileconflicts: find file conflicts in a set of packages
 * input:
 *   - pkgs: list of packages to check
 *   - cutoff: packages after this are not checked against each other
 *             this is useful to ignore file conflicts in already installed packages
 *   - flags: see pool_fileconflicts.h
 *   - handle_cb, handle_cbdata: callback for rpm header fetches
 * output:
 *   - conflicts: list of conflicts
 *
 * This is designed for needing only little memory while still being reasonable fast.
 * We do this by hashing the file names and working with the 32bit hash values in the
 * first steps of the algorithm. A hash conflict is not a problem as it will just
 * lead to some unneeded extra work later on.
 */

int
pool_findfileconflicts(Pool *pool, Queue *pkgs, int cutoff, Queue *conflicts, int flags, void *(*handle_cb)(Pool *, Id, void *) , void *handle_cbdata)
{
  int i, j, idxmapset;
  struct cbdata cbdata;
  unsigned int now, start;
  void *handle;
  Repo *installed = pool->installed;
  Id p;
  int usefilecolors;
  int hdrfetches;
  int lookat_cnt;

  queue_empty(conflicts);
  if (!pkgs->count)
    return 0;

  now = start = solv_timems(0);
  /* Hmm, should we have a different flag for this? */
  usefilecolors = pool_get_flag(pool, POOL_FLAG_IMPLICITOBSOLETEUSESCOLORS);
  POOL_DEBUG(SOLV_DEBUG_STATS, "searching for file conflicts\n");
  POOL_DEBUG(SOLV_DEBUG_STATS, "packages: %d, cutoff %d, usefilecolors %d\n", pkgs->count, cutoff, usefilecolors);

  memset(&cbdata, 0, sizeof(cbdata));
  cbdata.aliases = flags & FINDFILECONFLICTS_CHECK_DIRALIASING;
  cbdata.pool = pool;
  if (cbdata.aliases && (flags & FINDFILECONFLICTS_USE_ROOTDIR) != 0)
    {
      cbdata.rootdir = pool_get_rootdir(pool);
      if (cbdata.rootdir && !strcmp(cbdata.rootdir, "/"))
	cbdata.rootdir = 0;
      if (cbdata.rootdir)
	cbdata.rootdirl = strlen(cbdata.rootdir);
      if (!cbdata.rootdir)
	cbdata.usestat = 1;
    }
  queue_init(&cbdata.lookat);
  queue_init(&cbdata.lookat_dir);
  map_init(&cbdata.idxmap, pkgs->count);

  if (cutoff <= 0)
    cutoff = pkgs->count;

  /* avarage file list size: 200 files per package */
  /* avarage dir count: 20 dirs per package */

  /* first pass: find dirs belonging to multiple packages */
  if (!cbdata.aliases)
    {
      hdrfetches = 0;
      cbdata.dirmapn = mkmask((cutoff + 3) * 16);
      cbdata.dirmap = solv_calloc(cbdata.dirmapn + 1, 2 * sizeof(Id));
      cbdata.create = 1;
      idxmapset = 0;
      for (i = 0; i < pkgs->count; i++)
	{
	  if (i == cutoff)
	    cbdata.create = 0;
	  cbdata.idx = i;
	  p = pkgs->elements[i];
	  if ((flags & FINDFILECONFLICTS_USE_SOLVABLEFILELIST) != 0 && installed)
	    {
	      if (p >= installed->start && p < installed->end && pool->solvables[p].repo == installed)
		{
		  iterate_solvable_dirs(pool, p, finddirs_cb, &cbdata);
		  if (MAPTST(&cbdata.idxmap, i))
		    idxmapset++;
		  continue;
		}
	    }
	  handle = (*handle_cb)(pool, p, handle_cbdata);
	  if (!handle)
	    continue;
	  hdrfetches++;
	  rpm_iterate_filelist(handle, RPM_ITERATE_FILELIST_ONLYDIRS, finddirs_cb, &cbdata);
	  if (MAPTST(&cbdata.idxmap, i))
	    idxmapset++;
	}
      POOL_DEBUG(SOLV_DEBUG_STATS, "dirmap size: %d, used %d\n", cbdata.dirmapn + 1, cbdata.dirmapused);
      POOL_DEBUG(SOLV_DEBUG_STATS, "dirmap memory usage: %d K\n", (cbdata.dirmapn + 1) * 2 * (int)sizeof(Id) / 1024);
      POOL_DEBUG(SOLV_DEBUG_STATS, "header fetches: %d\n", hdrfetches);
      POOL_DEBUG(SOLV_DEBUG_STATS, "dirmap creation took %d ms\n", solv_timems(now));
      POOL_DEBUG(SOLV_DEBUG_STATS, "dir conflicts found: %d, idxmap %d of %d\n", cbdata.dirconflicts, idxmapset, pkgs->count);
    }

  /* second pass: scan files in the directories found above */
  now = solv_timems(0);
  cbdata.cflmapn = mkmask((cutoff + 3) * 32);
  cbdata.cflmap = solv_calloc(cbdata.cflmapn + 1, 2 * sizeof(Id));
  cbdata.create = 1;
  hdrfetches = 0;
  for (i = 0; i < pkgs->count; i++)
    {
      if (i == cutoff)
	cbdata.create = 0;
      if (!cbdata.aliases && !MAPTST(&cbdata.idxmap, i))
	continue;
      cbdata.idx = i;
      p = pkgs->elements[i];
      if (!cbdata.create && (flags & FINDFILECONFLICTS_USE_SOLVABLEFILELIST) != 0 && installed)
	{
	  if (p >= installed->start && p < installed->end && pool->solvables[p].repo == installed)
	    if (!precheck_solvable_files(&cbdata, pool, p))
	      continue;
	}
      /* can't use FINDFILECONFLICTS_USE_SOLVABLEFILELIST because we have to know if
       * the file is a directory or not */
      handle = (*handle_cb)(pool, p, handle_cbdata);
      if (!handle)
	continue;
      hdrfetches++;
      cbdata.lastdiridx = -1;
      rpm_iterate_filelist(handle, RPM_ITERATE_FILELIST_NOGHOSTS, cbdata.aliases ? findfileconflicts_basename_cb : findfileconflicts_cb, &cbdata);
    }

  POOL_DEBUG(SOLV_DEBUG_STATS, "filemap size: %d, used %d\n", cbdata.cflmapn + 1, cbdata.cflmapused);
  POOL_DEBUG(SOLV_DEBUG_STATS, "filemap memory usage: %d K\n", (cbdata.cflmapn + 1) * 2 * (int)sizeof(Id) / 1024);
  POOL_DEBUG(SOLV_DEBUG_STATS, "header fetches: %d\n", hdrfetches);
  POOL_DEBUG(SOLV_DEBUG_STATS, "filemap creation took %d ms\n", solv_timems(now));
  POOL_DEBUG(SOLV_DEBUG_STATS, "lookat_dir size: %d\n", cbdata.lookat_dir.count);
  queue_free(&cbdata.lookat_dir);

  /* we need another pass for aliases to generate the normalized directory ids */
  queue_init(&cbdata.norq);
  if (cbdata.aliases)
    {
      now = solv_timems(0);
      addfilesspace(&cbdata, 1);	/* make sure the first offset is not zero */
      cbdata.normapn = mkmask((cutoff + 3) * 4);
      cbdata.normap = solv_calloc(cbdata.normapn + 1, 2 * sizeof(Id));
      if (cbdata.usestat)
	{
	  cbdata.statmapn = cbdata.normapn;
	  cbdata.statmap = solv_calloc(cbdata.statmapn + 1, 2 * sizeof(Id));
	}
      cbdata.create = 0;
      hdrfetches = 0;
      for (i = 0; i < pkgs->count; i++)
	{
	  if (!MAPTST(&cbdata.idxmap, i))
	    continue;
	  p = pkgs->elements[i];
	  cbdata.idx = i;
	  /* can't use FINDFILECONFLICTS_USE_SOLVABLEFILELIST because we have to know if
	   * the file is a directory or not */
	  handle = (*handle_cb)(pool, p, handle_cbdata);
	  if (!handle)
	    continue;
	  hdrfetches++;
	  cbdata.lastdiridx = -1;
	  rpm_iterate_filelist(handle, RPM_ITERATE_FILELIST_NOGHOSTS, findfileconflicts_alias_cb, &cbdata);
	}
      POOL_DEBUG(SOLV_DEBUG_STATS, "normap size: %d, used %d\n", cbdata.normapn + 1, cbdata.normapused);
      POOL_DEBUG(SOLV_DEBUG_STATS, "normap memory usage: %d K\n", (cbdata.normapn + 1) * 2 * (int)sizeof(Id) / 1024);
      POOL_DEBUG(SOLV_DEBUG_STATS, "header fetches: %d\n", hdrfetches);
      POOL_DEBUG(SOLV_DEBUG_STATS, "stats made: %d\n", cbdata.statsmade);
      if (cbdata.usestat)
	{
	  POOL_DEBUG(SOLV_DEBUG_STATS, "statmap size: %d, used %d\n", cbdata.statmapn + 1, cbdata.statmapused);
	  POOL_DEBUG(SOLV_DEBUG_STATS, "statmap memory usage: %d K\n", (cbdata.statmapn + 1) * 2 * (int)sizeof(Id) / 1024);
	}
      cbdata.statmap = solv_free(cbdata.statmap);
      cbdata.statmapn = 0;
      cbdata.canonspace = solv_free(cbdata.canonspace);
      cbdata.canonspacen = 0;
      POOL_DEBUG(SOLV_DEBUG_STATS, "alias processing took %d ms\n", solv_timems(now));
    }

  /* free no longer used stuff */
  cbdata.dirmap = solv_free(cbdata.dirmap);
  cbdata.dirmapn = 0;
  cbdata.dirmapused = 0;
  cbdata.cflmap = solv_free(cbdata.cflmap);
  cbdata.cflmapn = 0;
  cbdata.cflmapused = 0;
  map_free(&cbdata.idxmap);

  /* sort and unify/prune */
  /* this also makes all dirids positive as side effect */
  now = solv_timems(0);
  POOL_DEBUG(SOLV_DEBUG_STATS, "raw candidates: %d\n", cbdata.lookat.count / 4);
  solv_sort(cbdata.lookat.elements, cbdata.lookat.count / 4, sizeof(Id) * 4, &lookat_hx_cmp, pool);
  for (i = j = 0; i < cbdata.lookat.count; )
    {
      int first = 1, jstart = j;
      Id hx = cbdata.lookat.elements[i];
      Id idx = cbdata.lookat.elements[i + 1];
      Id dhx = cbdata.lookat.elements[i + 2];
      Id dirid = cbdata.lookat.elements[i + 3];
      i += 4;
      for (; i < cbdata.lookat.count && hx == cbdata.lookat.elements[i] && (dirid == cbdata.lookat.elements[i + 3] || dirid == -cbdata.lookat.elements[i + 3]); i += 4)
	{
	  if (idx == cbdata.lookat.elements[i + 1] && dhx == cbdata.lookat.elements[i + 2])
	    {
	      if (first && idx < cutoff && cbdata.aliases && dirid >= 0)
		first = 0;	/* special self-conflict case with dhx hash collision, e.g. /foo/xx and /fpf/xx */
	      else
	        continue;	/* ignore duplicates */
	    }
	  if (first)
	    {
	      if (dirid < 0)
		continue;	/* all have a neg dirid */
	      cbdata.lookat.elements[j++] = hx;
	      cbdata.lookat.elements[j++] = idx;
	      cbdata.lookat.elements[j++] = dhx;
	      cbdata.lookat.elements[j++] = dirid;
	      first = 0;
	      if (jstart >= 0 && idx < cutoff)
		jstart = -1;
	    }
	  idx = cbdata.lookat.elements[i + 1];
	  dhx = cbdata.lookat.elements[i + 2];
	  cbdata.lookat.elements[j++] = hx;
	  cbdata.lookat.elements[j++] = idx;
	  cbdata.lookat.elements[j++] = dhx;
	  cbdata.lookat.elements[j++] = dirid;
	  if (jstart >= 0 && idx < cutoff)
	    jstart = -1;
	}
      if (jstart >= 0)	/* we need at least one new candidate */
	j = jstart;
    }
  queue_truncate(&cbdata.lookat, j);
  POOL_DEBUG(SOLV_DEBUG_STATS, "pruned candidates: %d\n", cbdata.lookat.count / 4);
  POOL_DEBUG(SOLV_DEBUG_STATS, "pruning took %d ms\n", solv_timems(now));

  /* third pass: expand to real file names */
  now = solv_timems(0);
  /* sort by idx so we can do all files of a package in one go */
  solv_sort(cbdata.lookat.elements, cbdata.lookat.count / 4, sizeof(Id) * 4, &lookat_idx_cmp, pool);
  hdrfetches = 0;
  queue_init(&cbdata.newlookat);
  if (cbdata.lookat.count)
    {
      /* setup fetch map space */
      cbdata.fetchmapn = mkmask(cbdata.lookat.count + 3);
      if (cbdata.fetchmapn < 4095)
        cbdata.fetchmapn = 4095;
      cbdata.fetchmap = solv_calloc(cbdata.fetchmapn + 1, sizeof(Id));
      if (cbdata.aliases)
	{
	  cbdata.fetchdirmapn = ((cbdata.fetchmapn + 1) / 16) - 1;
	  map_init(&cbdata.fetchdirmap, cbdata.fetchdirmapn + 1);
	}
    }
  lookat_cnt = cbdata.lookat.count;
  while (lookat_cnt)
    {
      Id idx = cbdata.lookat.elements[1];
      int iterflags = RPM_ITERATE_FILELIST_WITHMD5 | RPM_ITERATE_FILELIST_NOGHOSTS;
      if (usefilecolors)
	iterflags |= RPM_ITERATE_FILELIST_WITHCOL;
      /* find end of idx block */
      for (j = 4; j < lookat_cnt; j += 4)
	if (cbdata.lookat.elements[j + 1] != idx)
	  break;
      p = pkgs->elements[idx];
      handle = (*handle_cb)(pool, p, handle_cbdata);
      if (!handle)
	{
	  queue_deleten(&cbdata.lookat, 0, j);
	  lookat_cnt -= j;
	  continue;
	}
      hdrfetches++;
      /* create hash which maps (hx, dirid) to lookat elements */
      /* also create map from dhx values for fast reject */
      for (i = 0; i < j; i += 4)
	{
	  Hashval h, hh;
	  h = (cbdata.lookat.elements[i] ^ (cbdata.lookat.elements[i + 3] * 37)) & cbdata.fetchmapn;
	  hh = HASHCHAIN_START;
	  while (cbdata.fetchmap[h])
	    h = HASHCHAIN_NEXT(h, hh, cbdata.fetchmapn);
	  cbdata.fetchmap[h] = i + 1;
	  cbdata.lookat.elements[i + 1] = (Id)h;	/* hack: misuse idx for easy hash cleanup */
	  if (cbdata.fetchdirmapn)
	    MAPSET(&cbdata.fetchdirmap, cbdata.lookat.elements[i + 2] & cbdata.fetchdirmapn);
	}
      cbdata.idx = idx;
      cbdata.lastdiridx = -1;
      cbdata.lastdiridxbad = 0;
      queue_prealloc(&cbdata.newlookat, j + 256);
      rpm_iterate_filelist(handle, iterflags, findfileconflicts_expand_cb, &cbdata);
      /* clear hash and map again */
      for (i = 0; i < j; i += 4)
	{
	  Hashval h = (Hashval)cbdata.lookat.elements[i + 1];
	  cbdata.fetchmap[h] = 0;
	  if (cbdata.fetchdirmapn)
	    MAPCLR_AT(&cbdata.fetchdirmap, cbdata.lookat.elements[i + 2] & cbdata.fetchdirmapn);
	}
      /* now delete old block and add new block to the end */
      queue_deleten(&cbdata.lookat, 0, j);
      queue_insertn(&cbdata.lookat, cbdata.lookat.count, cbdata.newlookat.count, cbdata.newlookat.elements);
      queue_empty(&cbdata.newlookat);
      lookat_cnt -= j;
    }
  queue_free(&cbdata.newlookat);
  POOL_DEBUG(SOLV_DEBUG_STATS, "header fetches: %d\n", hdrfetches);
  POOL_DEBUG(SOLV_DEBUG_STATS, "candidates now: %d\n", cbdata.lookat.count / 4);
  POOL_DEBUG(SOLV_DEBUG_STATS, "file expansion took %d ms\n", solv_timems(now));
  /* free memory */
  cbdata.fetchmap = solv_free(cbdata.fetchmap);
  cbdata.fetchmapn = 0;
  if (cbdata.fetchdirmapn)
    map_free(&cbdata.fetchdirmap);
  cbdata.fetchdirmapn = 0;
  cbdata.normap = solv_free(cbdata.normap);
  cbdata.normapn = 0;
  queue_free(&cbdata.norq);

  /* forth pass: for each (hx,dirid) we have, compare all matching files against all other matching files */
  now = solv_timems(0);
  solv_sort(cbdata.lookat.elements, cbdata.lookat.count / 4, sizeof(Id) * 4, &lookat_hx_cmp, pool);
  for (i = 0; i < cbdata.lookat.count - 4; i += 4)
    {
      Id hx = cbdata.lookat.elements[i];
      Id dirid = cbdata.lookat.elements[i + 3];
      Id idxi = cbdata.lookat.elements[i + 1];
      Id offi = cbdata.lookat.elements[i + 2];
      if (idxi >= cutoff)
	continue;	/* no conflicts between packages with idx >= cutoff */
      for (j = i + 4; j < cbdata.lookat.count && cbdata.lookat.elements[j] == hx && cbdata.lookat.elements[j + 3] == dirid; j += 4)
	{
	  Id idxj = cbdata.lookat.elements[j + 1];
	  Id offj = cbdata.lookat.elements[j + 2];
	  char *fsi = (char *)cbdata.filesspace + offi;
	  char *fsj = (char *)cbdata.filesspace + offj;
	  if (cbdata.aliases)
	    {
	      /* compare just the basenames, the dirs match because of the dirid */
	      char *bsi = strrchr(fsi + 34, '/');
	      char *bsj = strrchr(fsj + 34, '/');
	      if (!bsi || !bsj)
		continue;
	      if (strcmp(bsi, bsj))
		continue;	/* different base names */
	    }
	  else
	    {
	      if (strcmp(fsi + 34, fsj + 34))
		continue;	/* different file names */
	    }
	  if (!strcmp(fsi, fsj))
	    continue;	/* file digests match, no conflict */
	  if (usefilecolors && fsi[33] && fsj[33] && (fsi[33] & fsj[33]) == 0)
	    continue;	/* colors do not conflict */
	  queue_push(conflicts, pool_str2id(pool, fsi + 34, 1));
	  queue_push(conflicts, pkgs->elements[idxi]);
	  queue_push(conflicts, pool_str2id(pool, fsi, 1));
	  queue_push(conflicts, pool_str2id(pool, fsj + 34, 1));
	  queue_push(conflicts, pkgs->elements[idxj]);
	  queue_push(conflicts, pool_str2id(pool, fsj, 1));
	}
    }
  POOL_DEBUG(SOLV_DEBUG_STATS, "filespace size: %d K\n", cbdata.filesspacen / 1024);
  POOL_DEBUG(SOLV_DEBUG_STATS, "candidate check took %d ms\n", solv_timems(now));
  cbdata.filesspace = solv_free(cbdata.filesspace);
  cbdata.filesspacen = 0;
  queue_free(&cbdata.lookat);
  if (conflicts->count > 6)
    solv_sort(conflicts->elements, conflicts->count / 6, 6 * sizeof(Id), conflicts_cmp, pool);
  POOL_DEBUG(SOLV_DEBUG_STATS, "found %d file conflicts\n", conflicts->count / 6);
  POOL_DEBUG(SOLV_DEBUG_STATS, "file conflict detection took %d ms\n", solv_timems(start));

  return conflicts->count / 6;
}

