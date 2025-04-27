/*
 * Copyright (c) 2007-2011, Novell Inc.
 *
 * This program is licensed under the BSD license, read LICENSE.BSD
 * for further information
 */

/*
 * repo_write.c
 *
 * Write Repo data out to a file in solv format
 *
 * See doc/README.format for a description
 * of the binary file format
 *
 */

#include <sys/types.h>
#include <limits.h>
#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <assert.h>
#include <errno.h>

#include "pool.h"
#include "util.h"
#include "repo_write.h"
#include "repopage.h"

#undef USE_IDARRAYBLOCK
#define USE_REL_IDARRAY

/*------------------------------------------------------------------*/
/* Id map optimizations */

typedef struct needid {
  Id need;
  Id map;
} NeedId;


#define NEEDIDOFF(id) (ISRELDEP(id) ? (needid[0].map + GETRELID(id)) : id)

/*
 * increment need Id
 * idarray: array of Ids, ID_NULL terminated
 * needid: array of Id->NeedId
 *
 * return size of array (including trailing zero)
 *
 */

static inline void
incneedid(Id id, NeedId *needid)
{
  needid[NEEDIDOFF(id)].need++;
}

static int
incneedidarray(Id *idarray, NeedId *needid)
{
  Id id;
  int n = 0;

  while ((id = *idarray++) != 0)
    {
      n++;
      needid[NEEDIDOFF(id)].need++;
    }
  return n + 1;
}


/*
 *
 */

static int
needid_cmp_need(const void *ap, const void *bp, void *dp)
{
  const NeedId *a = ap;
  const NeedId *b = bp;
  int r;
  r = b->need - a->need;
  if (r)
    return r;
  return a->map - b->map;
}

static int
needid_cmp_need_s(const void *ap, const void *bp, void *dp)
{
  const NeedId *a = ap;
  const NeedId *b = bp;
  Stringpool *spool = dp;
  const char *as;
  const char *bs;

  int r;
  r = b->need - a->need;
  if (r)
    return r;
  as = spool->stringspace + spool->strings[a->map];
  bs = spool->stringspace + spool->strings[b->map];
  return strcmp(as, bs);
}


/*------------------------------------------------------------------*/
/* output helper routines, used for writing the header */
/* (the data itself is accumulated in memory and written with
 * write_blob) */

/*
 * unsigned 32-bit
 */

static void
write_u32(Repodata *data, unsigned int x)
{
  FILE *fp = data->fp;
  if (data->error)
    return;
  if (putc(x >> 24, fp) == EOF ||
      putc(x >> 16, fp) == EOF ||
      putc(x >> 8, fp) == EOF ||
      putc(x, fp) == EOF)
    {
      data->error = pool_error(data->repo->pool, -1, "write error u32: %s", strerror(errno));
    }
}


/*
 * unsigned 8-bit
 */

static void
write_u8(Repodata *data, unsigned int x)
{
  if (data->error)
    return;
  if (putc(x, data->fp) == EOF)
    {
      data->error = pool_error(data->repo->pool, -1, "write error u8: %s", strerror(errno));
    }
}

/*
 * data blob
 */

static void
write_blob(Repodata *data, void *blob, int len)
{
  if (data->error)
    return;
  if (len && fwrite(blob, len, 1, data->fp) != 1)
    {
      data->error = pool_error(data->repo->pool, -1, "write error blob: %s", strerror(errno));
    }
}

static void
write_compressed_blob(Repodata *data, void *blob, int len)
{
  unsigned char cpage[65536];
  if (data->error)
    return;
  while (len > 0)
    {
      int chunk = len > sizeof(cpage) ? sizeof(cpage) : len;
      int flag = (chunk == len ? 0x80 : 0x00);
      int clen = repopagestore_compress_page(blob, chunk, cpage, sizeof(cpage) - 1);
      if (!clen)
	{
	  write_u8(data, flag);
	  write_u8(data, chunk >> 8);
	  write_u8(data, chunk);
	  write_blob(data, blob, chunk);
	}
      else
	{
	  write_u8(data, flag | 0x40);
	  write_u8(data, clen >> 8);
	  write_u8(data, clen);
	  write_blob(data, cpage, clen);
	}
      blob = (char*) blob + chunk;
      len -= chunk;
    }
}

/*
 * Id
 */

static void
write_id(Repodata *data, Id x)
{
  FILE *fp = data->fp;
  if (data->error)
    return;
  if (x >= (1 << 14))
    {
      if (x >= (1 << 28))
	putc((x >> 28) | 128, fp);
      if (x >= (1 << 21))
	putc((x >> 21) | 128, fp);
      putc((x >> 14) | 128, fp);
    }
  if (x >= (1 << 7))
    putc((x >> 7) | 128, fp);
  if (putc(x & 127, fp) == EOF)
    {
      data->error = pool_error(data->repo->pool, -1, "write error id: %s", strerror(errno));
    }
}

static inline void
write_str(Repodata *data, const char *str)
{
  if (data->error)
    return;
  if (fputs(str, data->fp) == EOF || putc(0, data->fp) == EOF)
    {
      data->error = pool_error(data->repo->pool, -1, "write error str: %s", strerror(errno));
    }
}

/*
 * Array of Ids
 */

static void
write_idarray(Repodata *data, Pool *pool, NeedId *needid, Id *ids)
{
  Id id;
  if (!ids)
    return;
  if (!*ids)
    {
      write_u8(data, 0);
      return;
    }
  for (;;)
    {
      id = *ids++;
      if (needid)
        id = needid[NEEDIDOFF(id)].need;
      if (id >= 64)
	id = (id & 63) | ((id & ~63) << 1);
      if (!*ids)
	{
	  write_id(data, id);
	  return;
	}
      write_id(data, id | 64);
    }
}

struct extdata {
  unsigned char *buf;
  int len;
};

#define DIRIDCACHE_SIZE 1024

struct cbdata {
  Pool *pool;
  Repo *repo;
  Repodata *target;

  Stringpool *ownspool;
  Dirpool *owndirpool;
  int clonepool;	/* are the pool ids cloned into ownspool? */

  Id *keymap;		/* keymap for this repodata */

  NeedId *needid;

  Id *schema;		/* schema construction space */
  Id *sp;		/* pointer in above */

  Id *subschemata;
  int nsubschemata;
  int current_sub;

  struct extdata *extdata;

  Id *dirused;

  Id vstart;		/* offset of key in vertical data */

  Id maxdata;
  Id lastlen;

  int doingsolvables;	/* working on solvables data */
  int filelistmode;

  Id lastdirid;		/* last dir id seen in this repodata */
  Id lastdirid_own;	/* last dir id put in own pool */

  Id diridcache[3 * DIRIDCACHE_SIZE];
};

#define NEEDID_BLOCK 1023
#define SCHEMATA_BLOCK 31
#define EXTDATA_BLOCK 4095

static inline void
data_addid(struct extdata *xd, Id sx)
{
  unsigned int x = (unsigned int)sx;
  unsigned char *dp;

  xd->buf = solv_extend(xd->buf, xd->len, 5, 1, EXTDATA_BLOCK);
  dp = xd->buf + xd->len;

  if (x >= (1 << 14))
    {
      if (x >= (1 << 28))
	*dp++ = (x >> 28) | 128;
      if (x >= (1 << 21))
	*dp++ = (x >> 21) | 128;
      *dp++ = (x >> 14) | 128;
    }
  if (x >= (1 << 7))
    *dp++ = (x >> 7) | 128;
  *dp++ = x & 127;
  xd->len = dp - xd->buf;
}

static inline void
data_addideof(struct extdata *xd, Id sx, int eof)
{
  unsigned int x = (unsigned int)sx;
  unsigned char *dp;

  xd->buf = solv_extend(xd->buf, xd->len, 5, 1, EXTDATA_BLOCK);
  dp = xd->buf + xd->len;

  if (x >= (1 << 13))
    {
      if (x >= (1 << 27))
        *dp++ = (x >> 27) | 128;
      if (x >= (1 << 20))
        *dp++ = (x >> 20) | 128;
      *dp++ = (x >> 13) | 128;
    }
  if (x >= (1 << 6))
    *dp++ = (x >> 6) | 128;
  *dp++ = eof ? (x & 63) : (x & 63) | 64;
  xd->len = dp - xd->buf;
}

static inline int
data_addideof_len(Id sx)
{
  unsigned int x = (unsigned int)sx;
  if (x >= (1 << 13))
    {
      if (x >= (1 << 27))
	return 5;
      return x >= (1 << 20) ? 4 : 3;
    }
  return x >= (1 << 6) ? 2 : 1;
}

static void
data_addid64(struct extdata *xd, unsigned int x, unsigned int hx)
{
  if (hx)
    {
      if (hx > 7)
        {
          data_addid(xd, (Id)(hx >> 3));
          xd->buf[xd->len - 1] |= 128;
	  hx &= 7;
        }
      data_addid(xd, (Id)(x | 0x80000000));
      xd->buf[xd->len - 5] = (x >> 28) | (hx << 4) | 128;
    }
  else
    data_addid(xd, (Id)x);
}

#ifdef USE_REL_IDARRAY

static int
cmp_ids(const void *pa, const void *pb, void *dp)
{
  Id a = *(Id *)pa;
  Id b = *(Id *)pb;
  return a - b;
}

static void
data_adddepids(struct extdata *xd, Pool *pool, NeedId *needid, Id *ids, Id marker)
{
  int len, i;
  Id lids[64], *sids;
  Id id, old;

  if (!ids || !*ids)
    {
      data_addideof(xd, 0, 1);
      return;
    }
  for (len = 0; len < 64 && ids[len]; len++)
    {
      Id id = ids[len];
      if (needid)
        id = needid[NEEDIDOFF(id)].need;
      lids[len] = id;
    }
  if (ids[len])
    {
      for (i = len + 1; ids[i]; i++)
	;
      sids = solv_malloc2(i, sizeof(Id));
      memcpy(sids, lids, 64 * sizeof(Id));
      for (; ids[len]; len++)
	{
	  Id id = ids[len];
	  if (needid)
            id = needid[NEEDIDOFF(id)].need;
	  sids[len] = id;
	}
    }
  else
    sids = lids;

  /* That bloody solvable:prereqmarker needs to stay in position :-(  */
  if (needid)
    marker = needid[marker].need;
  for (i = 0; i < len; i++)
    if (sids[i] == marker)
      break;
  if (i > 1)
    solv_sort(sids, i, sizeof(Id), cmp_ids, 0);
  if ((len - i) > 2)
    solv_sort(sids + i + 1, len - i - 1, sizeof(Id), cmp_ids, 0);

  old = 0;

  /* The differencing above produces many runs of ones and twos.  I tried
     fairly elaborate schemes to RLE those, but they give only very mediocre
     improvements in compression, as coding the escapes costs quite some
     space.  Even if they are coded only as bits in IDs.  The best improvement
     was about 2.7% for the whole .solv file.  It's probably better to
     invest some complexity into sharing idarrays, than RLEing.  */
  for (i = 0; i < len - 1; i++)
    {
      id = sids[i];
    /* Ugly PREREQ handling.  A "difference" of 0 is the prereq marker,
       hence all real differences are offsetted by 1.  Otherwise we would
       have to handle negative differences, which would cost code space for
       the encoding of the sign.  We loose the exact mapping of prereq here,
       but we know the result, so we can recover from that in the reader.  */
      if (id == marker)
	id = old = 0;
      else
	{
          id = id - old + 1;
	  old = sids[i];
	}
      /* XXX If difference is zero we have multiple equal elements,
	 we might want to skip writing them out.  */
      data_addideof(xd, id, 0);
    }
  id = sids[i];
  if (id == marker)
    id = 0;
  else
    id = id - old + 1;
  data_addideof(xd, id, 1);
  if (sids != lids)
    solv_free(sids);
}

#else

#ifdef USE_IDARRAYBLOCK

static void
data_adddepids(struct extdata *xd, Pool *pool, NeedId *needid, Id *ids, Id marker)
{
  Id id;
  Id last = 0, tmp;
  if (!ids || !*ids)
    {
      data_addideof(xd, 0, 1);
      return;
    }
  while ((id = *ids++) != 0)
    {
      if (needid)
        id = needid[NEEDIDOFF(id)].need;
      tmp = id;
      if (id < last)
	id = (last - id) * 2 - 1;	/* [1, 2 * last - 1] odd */
      else if (id < 2 * last)
	id = (id - last) * 2;		/* [0, 2 * last - 2] even */
      last = tmp;
      data_addideof(xd, id, *ids ? 0 : 1);
    }
}

#else

static void
data_adddepids(struct extdata *xd, Pool *pool, NeedId *needid, Id *ids, Id marker)
{
  Id id;
  if (!ids || !*ids)
    {
      data_addideof(xd, 0, 1);
      return;
    }
  while ((id = *ids++) != 0)
    {
      if (needid)
        id = needid[NEEDIDOFF(id)].need;
      data_addideof(xd, id, *ids ? 0 : 1);
    }
}

#endif

#endif

static inline void
data_addblob(struct extdata *xd, unsigned char *blob, int len)
{
  xd->buf = solv_extend(xd->buf, xd->len, len, 1, EXTDATA_BLOCK);
  memcpy(xd->buf + xd->len, blob, len);
  xd->len += len;
}

/* grow needid array so that it contains the specified id */
static void
grow_needid(struct cbdata *cbdata, Id id)
{
  int oldoff = cbdata->needid[0].map;
  int newoff = (id + 1 + NEEDID_BLOCK) & ~NEEDID_BLOCK;
  int nrels = cbdata->pool->nrels;
  cbdata->needid = solv_realloc2(cbdata->needid, newoff + nrels, sizeof(NeedId));
  if (nrels)
    memmove(cbdata->needid + newoff, cbdata->needid + oldoff, nrels * sizeof(NeedId));
  memset(cbdata->needid + oldoff, 0, (newoff - oldoff) * sizeof(NeedId));
  cbdata->needid[0].map = newoff;
}

static Id
putinownpool(struct cbdata *cbdata, Repodata *data, Id id)
{
  Stringpool *ss = data->localpool ? &data->spool : &cbdata->pool->ss;
  const char *str = stringpool_id2str(ss, id);
  id = stringpool_str2id(cbdata->ownspool, str, 1);
  if (id >= cbdata->needid[0].map)
    grow_needid(cbdata, id);
  return id;
}

static Id
putinowndirpool_slow(struct cbdata *cbdata, Repodata *data, Dirpool *dp, Id dir)
{
  Id compid, parent, id;
  Id *cacheent;

  parent = dirpool_parent(dp, dir);
  if (parent)
    {
      /* put parent in own pool first */
      cacheent = cbdata->diridcache + (parent & (DIRIDCACHE_SIZE - 1));
      if (cacheent[0] == parent && cacheent[DIRIDCACHE_SIZE] == data->repodataid)
        parent = cacheent[2 * DIRIDCACHE_SIZE];
      else
        parent = putinowndirpool_slow(cbdata, data, dp, parent);
    }
  compid = dirpool_compid(dp, dir);
  if (cbdata->ownspool && compid > 1 && (!cbdata->clonepool || data->localpool))
    compid = putinownpool(cbdata, data, compid);
  id = dirpool_add_dir(cbdata->owndirpool, parent, compid, 1);
  /* cache result */
  cacheent = cbdata->diridcache + (dir & (DIRIDCACHE_SIZE - 1));
  cacheent[0] = dir;
  cacheent[DIRIDCACHE_SIZE] = data->repodataid;
  cacheent[2 * DIRIDCACHE_SIZE] = id;
  return id;
}

static inline Id
putinowndirpool(struct cbdata *cbdata, Repodata *data, Id dir)
{
  Id *cacheent;
  if (dir && dir == cbdata->lastdirid)
    return cbdata->lastdirid_own;
  cacheent = cbdata->diridcache + (dir & (DIRIDCACHE_SIZE - 1));
  if (dir && cacheent[0] == dir && cacheent[DIRIDCACHE_SIZE] == data->repodataid)
    return cacheent[2 * DIRIDCACHE_SIZE];
  cbdata->lastdirid = dir;
  cbdata->lastdirid_own = putinowndirpool_slow(cbdata, data, &data->dirpool, dir);
  return cbdata->lastdirid_own;
}

/*
 * pass 1 callback:
 * collect key/id/dirid usage information, create needed schemas
 */
static int
collect_needed_cb(void *vcbdata, Solvable *s, Repodata *data, Repokey *key, KeyValue *kv)
{
  struct cbdata *cbdata = vcbdata;
  Id id;
  int rm;

#if 0
    fprintf(stderr, "solvable %d (%s): key (%d)%s %d\n", s ? (int)(s - cbdata->pool->solvables) : 0, s ? pool_id2str(cbdata->pool, s->name) : "", key->name, pool_id2str(cbdata->pool, key->name), key->type);
#endif
  if (key->name == REPOSITORY_SOLVABLES)
    return SEARCH_NEXT_KEY;	/* we do not want this one */

  rm = cbdata->keymap[key - data->keys];
  if (!rm)
    return SEARCH_NEXT_KEY;	/* we do not want this one */

  /* record key in schema */
  if (cbdata->sp[-1] != rm)
    *cbdata->sp++ = rm;

  switch(key->type)
    {
      case REPOKEY_TYPE_ID:
      case REPOKEY_TYPE_IDARRAY:
	id = kv->id;
	if (!ISRELDEP(id) && cbdata->ownspool && id > 1 && (!cbdata->clonepool || data->localpool))
	  id = putinownpool(cbdata, data, id);
	incneedid(id, cbdata->needid);
	break;
      case REPOKEY_TYPE_DIR:
      case REPOKEY_TYPE_DIRNUMNUMARRAY:
      case REPOKEY_TYPE_DIRSTRARRAY:
	id = kv->id;
	if (cbdata->owndirpool)
	  putinowndirpool(cbdata, data, id);
	else
	  cbdata->dirused[id] = 1;
	break;
      case REPOKEY_TYPE_FIXARRAY:
      case REPOKEY_TYPE_FLEXARRAY:
	if (kv->entry)
	  {
	    /* finish schema, rewind to start */
	    Id *sp = cbdata->sp - 1;
	    *sp = 0;
	    while (sp[-1])
	      sp--;
	    if (sp[-2] >= 0)
	      cbdata->subschemata[sp[-2]] = repodata_schema2id(cbdata->target, sp, 1);
	    cbdata->sp = sp - 2;
	  }
	if (kv->eof != 2)
	  {
	    /* start new schema */
	    if (kv->entry == 0 || key->type == REPOKEY_TYPE_FLEXARRAY)
	      {
		cbdata->subschemata = solv_extend(cbdata->subschemata, cbdata->nsubschemata, 1, sizeof(Id), SCHEMATA_BLOCK);
		*cbdata->sp++ = cbdata->nsubschemata++;
	      }
	    else
	      *cbdata->sp++ = -1;
	    *cbdata->sp++ = 0;
          }
	break;
      default:
	break;
    }
  return 0;
}

static void
collect_needed_solvable(struct cbdata *cbdata, Solvable *s, Id *keymap)
{
  /* set schema info, keep in sync with collect_data_solvable */
  Repo *repo = s->repo;
  Id *sp = cbdata->sp;
  NeedId *needid = cbdata->needid;
  Repodata *target = cbdata->target;
  Id *idarraydata = repo->idarraydata;

  if (keymap[SOLVABLE_NAME])
    {
      *sp++ = keymap[SOLVABLE_NAME];
      needid[s->name].need++;
    }
  if (keymap[SOLVABLE_ARCH])
    {
      *sp++ = keymap[SOLVABLE_ARCH];
      needid[s->arch].need++;
    }
  if (keymap[SOLVABLE_EVR])
    {
      *sp++ = keymap[SOLVABLE_EVR];
      needid[s->evr].need++;
    }
  if (s->vendor && keymap[SOLVABLE_VENDOR])
    {
      *sp++ = keymap[SOLVABLE_VENDOR];
      needid[s->vendor].need++;
    }
  if (s->provides && keymap[SOLVABLE_PROVIDES])
    {
      *sp++ = keymap[SOLVABLE_PROVIDES];
      target->keys[keymap[SOLVABLE_PROVIDES]].size += incneedidarray(idarraydata + s->provides, needid);
    }
  if (s->obsoletes && keymap[SOLVABLE_OBSOLETES])
    {
      *sp++ = keymap[SOLVABLE_OBSOLETES];
      target->keys[keymap[SOLVABLE_OBSOLETES]].size += incneedidarray(idarraydata + s->obsoletes, needid);
    }
  if (s->conflicts && keymap[SOLVABLE_CONFLICTS])
    {
      *sp++ = keymap[SOLVABLE_CONFLICTS];
      target->keys[keymap[SOLVABLE_CONFLICTS]].size += incneedidarray(idarraydata + s->conflicts, needid);
    }
  if (s->requires && keymap[SOLVABLE_REQUIRES])
    {
      *sp++ = keymap[SOLVABLE_REQUIRES];
      target->keys[keymap[SOLVABLE_REQUIRES]].size += incneedidarray(idarraydata + s->requires, needid);
    }
  if (s->recommends && keymap[SOLVABLE_RECOMMENDS])
    {
      *sp++ = keymap[SOLVABLE_RECOMMENDS];
      target->keys[keymap[SOLVABLE_RECOMMENDS]].size += incneedidarray(idarraydata + s->recommends, needid);
    }
  if (s->suggests && keymap[SOLVABLE_SUGGESTS])
    {
      *sp++ = keymap[SOLVABLE_SUGGESTS];
      target->keys[keymap[SOLVABLE_SUGGESTS]].size += incneedidarray(idarraydata + s->suggests, needid);
    }
  if (s->supplements && keymap[SOLVABLE_SUPPLEMENTS])
    {
      *sp++ = keymap[SOLVABLE_SUPPLEMENTS];
      target->keys[keymap[SOLVABLE_SUPPLEMENTS]].size += incneedidarray(idarraydata + s->supplements, needid);
    }
  if (s->enhances && keymap[SOLVABLE_ENHANCES])
    {
      *sp++ = keymap[SOLVABLE_ENHANCES];
      target->keys[keymap[SOLVABLE_ENHANCES]].size += incneedidarray(idarraydata + s->enhances, needid);
    }
  if (repo->rpmdbid && keymap[RPM_RPMDBID])
    {
      *sp++ = keymap[RPM_RPMDBID];
      target->keys[keymap[RPM_RPMDBID]].size++;
    }
  cbdata->sp = sp;
}


/*
 * pass 2 callback:
 * encode all of the data into the correct buffers
 */
static int
collect_data_cb(void *vcbdata, Solvable *s, Repodata *data, Repokey *key, KeyValue *kv)
{
  struct cbdata *cbdata = vcbdata;
  int rm;
  Id id, storage;
  struct extdata *xd;
  NeedId *needid;

  if (key->name == REPOSITORY_SOLVABLES)
    return SEARCH_NEXT_KEY;

  rm = cbdata->keymap[key - data->keys];
  if (!rm)
    return SEARCH_NEXT_KEY;	/* we do not want this one */
  storage = cbdata->target->keys[rm].storage;

  xd = cbdata->extdata + 0;		/* incore buffer */
  if (storage == KEY_STORAGE_VERTICAL_OFFSET)
    {
      xd += rm;		/* vertical buffer */
      if (cbdata->vstart == -1)
        cbdata->vstart = xd->len;
    }
  switch(key->type)
    {
      case REPOKEY_TYPE_DELETED:
      case REPOKEY_TYPE_VOID:
      case REPOKEY_TYPE_CONSTANT:
      case REPOKEY_TYPE_CONSTANTID:
	break;
      case REPOKEY_TYPE_ID:
	id = kv->id;
	if (!ISRELDEP(id) && cbdata->ownspool && id > 1 && (!cbdata->clonepool || data->localpool))
	  id = putinownpool(cbdata, data, id);
        needid = cbdata->needid;
	id = needid[NEEDIDOFF(id)].need;
	data_addid(xd, id);
	break;
      case REPOKEY_TYPE_IDARRAY:
	id = kv->id;
	if (!ISRELDEP(id) && cbdata->ownspool && id > 1 && (!cbdata->clonepool || data->localpool))
	  id = putinownpool(cbdata, data, id);
        needid = cbdata->needid;
	id = needid[NEEDIDOFF(id)].need;
	data_addideof(xd, id, kv->eof);
	break;
      case REPOKEY_TYPE_STR:
	data_addblob(xd, (unsigned char *)kv->str, strlen(kv->str) + 1);
	break;
      case REPOKEY_TYPE_MD5:
	data_addblob(xd, (unsigned char *)kv->str, SIZEOF_MD5);
	break;
      case REPOKEY_TYPE_SHA1:
	data_addblob(xd, (unsigned char *)kv->str, SIZEOF_SHA1);
	break;
      case REPOKEY_TYPE_SHA224:
	data_addblob(xd, (unsigned char *)kv->str, SIZEOF_SHA224);
	break;
      case REPOKEY_TYPE_SHA256:
	data_addblob(xd, (unsigned char *)kv->str, SIZEOF_SHA256);
	break;
      case REPOKEY_TYPE_SHA384:
	data_addblob(xd, (unsigned char *)kv->str, SIZEOF_SHA384);
	break;
      case REPOKEY_TYPE_SHA512:
	data_addblob(xd, (unsigned char *)kv->str, SIZEOF_SHA512);
	break;
	break;
      case REPOKEY_TYPE_NUM:
	data_addid64(xd, kv->num, kv->num2);
	break;
      case REPOKEY_TYPE_DIR:
	id = kv->id;
	if (cbdata->owndirpool)
	  id = putinowndirpool(cbdata, data, id);
	id = cbdata->dirused[id];
	data_addid(xd, id);
	break;
      case REPOKEY_TYPE_BINARY:
	data_addid(xd, kv->num);
	if (kv->num)
	  data_addblob(xd, (unsigned char *)kv->str, kv->num);
	break;
      case REPOKEY_TYPE_DIRNUMNUMARRAY:
	id = kv->id;
	if (cbdata->owndirpool)
	  id = putinowndirpool(cbdata, data, id);
	id = cbdata->dirused[id];
	data_addid(xd, id);
	data_addid(xd, kv->num);
	data_addideof(xd, kv->num2, kv->eof);
	break;
      case REPOKEY_TYPE_DIRSTRARRAY:
	id = kv->id;
	if (cbdata->owndirpool)
	  id = putinowndirpool(cbdata, data, id);
	id = cbdata->dirused[id];
	if (rm == cbdata->filelistmode)
	  {
	    /* postpone adding to xd, just update len to get the correct offsets into the incore data*/
	    xd->len += data_addideof_len(id) + strlen(kv->str) + 1;
	    break;
	  }
	data_addideof(xd, id, kv->eof);
	data_addblob(xd, (unsigned char *)kv->str, strlen(kv->str) + 1);
	break;
      case REPOKEY_TYPE_FIXARRAY:
      case REPOKEY_TYPE_FLEXARRAY:
	if (!kv->entry)
	  data_addid(xd, kv->num);
	if (kv->eof != 2 && (!kv->entry || key->type == REPOKEY_TYPE_FLEXARRAY))
	  data_addid(xd, cbdata->subschemata[cbdata->current_sub++]);
	if (xd == cbdata->extdata + 0 && !kv->parent && !cbdata->doingsolvables)
	  {
	    if (xd->len - cbdata->lastlen > cbdata->maxdata)
	      cbdata->maxdata = xd->len - cbdata->lastlen;
	    cbdata->lastlen = xd->len;
	  }
	break;
      default:
	cbdata->target->error = pool_error(cbdata->pool, -1, "unknown type for %d: %d\n", key->name, key->type);
	break;
    }
  if (storage == KEY_STORAGE_VERTICAL_OFFSET && kv->eof)
    {
      /* we can re-use old data in the blob here! */
      data_addid(cbdata->extdata + 0, cbdata->vstart);			/* add offset into incore data */
      data_addid(cbdata->extdata + 0, xd->len - cbdata->vstart);	/* add length into incore data */
      cbdata->vstart = -1;
    }
  return 0;
}

/* special version of collect_data_cb that collects just one single REPOKEY_TYPE_DIRSTRARRAY vertical data */
static int
collect_filelist_cb(void *vcbdata, Solvable *s, Repodata *data, Repokey *key, KeyValue *kv)
{
  struct cbdata *cbdata = vcbdata;
  int rm;
  Id id;
  struct extdata *xd;

  rm = cbdata->keymap[key - data->keys];
  if (rm != cbdata->filelistmode)
    return SEARCH_NEXT_KEY;	/* we do not want this one */
  id = kv->id;
  if (cbdata->owndirpool)
    id = putinowndirpool(cbdata, data, id);
  id = cbdata->dirused[id];
  xd = cbdata->extdata + rm;	/* vertical buffer */
  data_addideof(xd, id, kv->eof);
  data_addblob(xd, (unsigned char *)kv->str, strlen(kv->str) + 1);
  return 0;
}

static void
collect_data_solvable(struct cbdata *cbdata, Solvable *s, Id *keymap)
{
  Repo *repo = s->repo;
  Pool *pool = repo->pool;
  struct extdata *xd = cbdata->extdata;
#ifdef USE_IDARRAYBLOCK
  struct extdata *xda = xd + cbdata->target->nkeys;	/* idarray block */
#else
  struct extdata *xda = xd;
#endif

  NeedId *needid = cbdata->needid;
  Id *idarraydata = repo->idarraydata;

  if (keymap[SOLVABLE_NAME])
    data_addid(xd, needid[s->name].need);
  if (keymap[SOLVABLE_ARCH])
    data_addid(xd, needid[s->arch].need);
  if (keymap[SOLVABLE_EVR])
    data_addid(xd, needid[s->evr].need);
  if (s->vendor && keymap[SOLVABLE_VENDOR])
    data_addid(xd, needid[s->vendor].need);
  if (s->provides && keymap[SOLVABLE_PROVIDES])
    data_adddepids(xda, pool, needid, idarraydata + s->provides, SOLVABLE_FILEMARKER);
  if (s->obsoletes && keymap[SOLVABLE_OBSOLETES])
    data_adddepids(xda, pool, needid, idarraydata + s->obsoletes, 0);
  if (s->conflicts && keymap[SOLVABLE_CONFLICTS])
    data_adddepids(xda, pool, needid, idarraydata + s->conflicts, 0);
  if (s->requires && keymap[SOLVABLE_REQUIRES])
    data_adddepids(xda, pool, needid, idarraydata + s->requires, SOLVABLE_PREREQMARKER);
  if (s->recommends && keymap[SOLVABLE_RECOMMENDS])
    data_adddepids(xda, pool, needid, idarraydata + s->recommends, 0);
  if (s->suggests && keymap[SOLVABLE_SUGGESTS])
    data_adddepids(xda, pool, needid, idarraydata + s->suggests, 0);
  if (s->supplements && keymap[SOLVABLE_SUPPLEMENTS])
    data_adddepids(xda, pool, needid, idarraydata + s->supplements, 0);
  if (s->enhances && keymap[SOLVABLE_ENHANCES])
    data_adddepids(xda, pool, needid, idarraydata + s->enhances, 0);
  if (repo->rpmdbid && keymap[RPM_RPMDBID])
    data_addid(xd, repo->rpmdbid[(s - pool->solvables) - repo->start]);
}

/* traverse through directory with first child "dir" */
static int
traverse_dirs(Dirpool *dp, Id *dirmap, Id n, Id dir, Id *used)
{
  Id sib, child;
  Id parent, lastn;

  parent = n;
  /* special case for '/', which has to come first */
  if (parent == 1)
    dirmap[n++] = 1;
  for (sib = dir; sib; sib = dirpool_sibling(dp, sib))
    {
      if (used && !used[sib])
	continue;
      if (sib == 1 && parent == 1)
	continue;	/* already did that one above */
      dirmap[n++] = sib;
    }

  /* check if our block has some content */
  if (parent == n)
    return n - 1;	/* nope, drop parent id again */

  /* now go through all the siblings we just added and
   * do recursive calls on them */
  lastn = n;
  for (; parent < lastn; parent++)
    {
      sib = dirmap[parent];
      if (used && used[sib] != 2)	/* 2: used as parent */
	continue;
      child = dirpool_child(dp, sib);
      if (child)
	{
	  dirmap[n++] = -parent;	/* start new block */
	  n = traverse_dirs(dp, dirmap, n, child, used);
	}
    }
  return n;
}

static void
write_compressed_page(Repodata *data, unsigned char *page, int len)
{
  int clen;
  unsigned char cpage[REPOPAGE_BLOBSIZE];

  clen = repopagestore_compress_page(page, len, cpage, len - 1);
  if (!clen)
    {
      write_u32(data, len * 2);
      write_blob(data, page, len);
    }
  else
    {
      write_u32(data, clen * 2 + 1);
      write_blob(data, cpage, clen);
    }
}

static Id verticals[] = {
  SOLVABLE_AUTHORS,
  SOLVABLE_DESCRIPTION,
  SOLVABLE_MESSAGEDEL,
  SOLVABLE_MESSAGEINS,
  SOLVABLE_EULA,
  SOLVABLE_DISKUSAGE,
  SOLVABLE_FILELIST,
  SOLVABLE_CHECKSUM,
  DELTA_CHECKSUM,
  DELTA_SEQ_NUM,
  SOLVABLE_PKGID,
  SOLVABLE_HDRID,
  SOLVABLE_LEADSIGID,
  SOLVABLE_CHANGELOG_AUTHOR,
  SOLVABLE_CHANGELOG_TEXT,
  SOLVABLE_SIGNATUREDATA,
  0
};

static char *languagetags[] = {
  "solvable:summary:",
  "solvable:description:",
  "solvable:messageins:",
  "solvable:messagedel:",
  "solvable:eula:",
  0
};

int
repo_write_stdkeyfilter(Repo *repo, Repokey *key, void *kfdata)
{
  const char *keyname;
  int i;

  for (i = 0; verticals[i]; i++)
    if (key->name == verticals[i])
      return KEY_STORAGE_VERTICAL_OFFSET;
  keyname = pool_id2str(repo->pool, key->name);
  for (i = 0; languagetags[i] != 0; i++)
    if (!strncmp(keyname, languagetags[i], strlen(languagetags[i])))
      return KEY_STORAGE_VERTICAL_OFFSET;
  return KEY_STORAGE_INCORE;
}

static int
write_compressed_extdata(Repodata *target, struct extdata *xd, unsigned char *vpage, int lpage)
{
  unsigned char *dp = xd->buf;
  int l = xd->len;
  while (l)
    {
      int ll = REPOPAGE_BLOBSIZE - lpage;
      if (l < ll)
	ll = l;
      memcpy(vpage + lpage, dp, ll);
      dp += ll;
      lpage += ll;
      l -= ll;
      if (lpage == REPOPAGE_BLOBSIZE)
	{
	  write_compressed_page(target, vpage, lpage);
	  lpage = 0;
	}
    }
  return lpage;
}


static Id *
create_keyskip(Repo *repo, Id entry, unsigned char *repodataused, Id **oldkeyskip)
{
  Repodata *data, *last = 0;
  Id *keyskip;
  int rdid, cnt = 0;

  if (repo->nrepodata <= 2)
    return 0;
  keyskip = *oldkeyskip;
  if (keyskip)
    {
      if (keyskip[1] >= 0x10000000)
	keyskip = solv_free(keyskip);
      else
        keyskip[1] = keyskip[2];
    }
  FOR_REPODATAS(repo, rdid, data)
    {
      if (!repodataused[rdid])
        continue;
      if (entry != SOLVID_META)
	{
	  if (entry < data->start || entry >= data->end)
	    continue;
	  /* if repodataused is set we know that the state is AVAILABLE */
	  if (!data->incoreoffset[entry - data->start])
	    continue;
	}
      if (last)
        keyskip = repodata_fill_keyskip(last, entry, keyskip);
      last = data;
      cnt++;
    }
  if (cnt <= 1)		/* just one repodata means we don't need a keyskip */
    {
      *oldkeyskip = keyskip;
      return 0;
    }
  keyskip = repodata_fill_keyskip(last, entry, keyskip);
  if (keyskip)
    keyskip[2] = keyskip[1] + repo->nrepodata;
  *oldkeyskip = keyskip;
  return keyskip;
}

/*
 * Repo
 */

Repowriter *
repowriter_create(Repo *repo)
{
  Repowriter *writer = solv_calloc(1, sizeof(*writer));
  writer->repo = repo;
  writer->keyfilter = repo_write_stdkeyfilter;
  writer->repodatastart = 1;
  writer->repodataend = repo->nrepodata;
  writer->solvablestart = repo->start;
  writer->solvableend = repo->end;
  return writer;
}

Repowriter *
repowriter_free(Repowriter *writer)
{
  solv_free(writer->userdata);
  return solv_free(writer);
}

void
repowriter_set_flags(Repowriter *writer, int flags)
{
  writer->flags = flags;
}

void
repowriter_set_keyfilter(Repowriter *writer, int (*keyfilter)(Repo *repo, Repokey *key, void *kfdata), void *kfdata)
{
  writer->keyfilter = keyfilter;
  writer->kfdata = kfdata;
}

void
repowriter_set_keyqueue(Repowriter *writer, Queue *keyq)
{
  writer->keyq = keyq;
}

void
repowriter_set_repodatarange(Repowriter *writer, int repodatastart, int repodataend)
{
  writer->repodatastart = repodatastart;
  writer->repodataend = repodataend;
}

void
repowriter_set_solvablerange(Repowriter *writer, int solvablestart, int solvableend)
{
  writer->solvablestart = solvablestart;
  writer->solvableend = solvableend;
}

void
repowriter_set_userdata(Repowriter *writer, const void *data, int len)
{
  writer->userdata = solv_free(writer->userdata);
  writer->userdatalen = 0;
  if (len <= 0)
    return;
  writer->userdata = solv_memdup(data, len);
  writer->userdatalen = len;
}

/*
 * the code works the following way:
 *
 * 1) find which keys should be written
 * 2) collect usage information for keys/ids/dirids, create schema
 *    data
 * 3) use usage information to create mapping tables, so that often
 *    used ids get a lower number
 * 4) encode data into buffers using the mapping tables
 * 5) write everything to disk
 */
int
repowriter_write(Repowriter *writer, FILE *fp)
{
  Repo *repo = writer->repo;
  Pool *pool = repo->pool;
  int i, j, n;
  Solvable *s;
  NeedId *needid, *needidp;
  int nstrings, nrels;
  unsigned int sizeid;
  unsigned int solv_flags;
  Id *oldkeyskip = 0;
  Id *keyskip = 0;
  int searchflags = 0;

  Id id, *sp;

  Id *keymap;	/* maps repo key to my key, 0 -> not used */
  int nkeymap;
  int *keymapstart;	/* maps repo number to keymap offset */

  Id *dirmap;
  int ndirmap;
  Id *keyused;

  unsigned char *repodataused;
  int anyrepodataused = 0;

  int solvablestart, solvableend;
  Id *solvschemata;
  int anysolvableused = 0;
  int nsolvables;

  struct cbdata cbdata;

  int clonepool;
  Repokey *key;
  int poolusage, dirpoolusage;
  int reloff;

  Repodata *data, *dirpooldata;

  Repodata target;

  Stringpool *spool;
  Dirpool *dirpool;

  Id mainschema, *mainschemakeys;

  struct extdata *xd;

  Id type_constantid = 0;

  /* sanity checks */
  if (writer->userdatalen < 0 || writer->userdatalen >= 65536)
    return pool_error(pool, -1, "illegal userdata length: %d", writer->userdatalen);

  memset(&cbdata, 0, sizeof(cbdata));
  cbdata.pool = pool;
  cbdata.repo = repo;
  cbdata.target = &target;

  repodata_initdata(&target, repo, 1);

  /* go through all repodata and find the keys we need */
  /* also unify keys */

  /* start with all KEY_STORAGE_SOLVABLE ids */

  n = ID_NUM_INTERNAL;
  FOR_REPODATAS(repo, i, data)
    n += data->nkeys;
  nkeymap = n;
  keymap = solv_calloc(nkeymap, sizeof(Id));
  keymapstart = solv_calloc(repo->nrepodata, sizeof(Id));
  repodataused = solv_calloc(repo->nrepodata, 1);

  clonepool = 0;
  poolusage = 0;

  if (!(writer->flags & REPOWRITER_NO_STORAGE_SOLVABLE))
    {
      /* add keys for STORAGE_SOLVABLE */
      for (i = SOLVABLE_NAME; i <= RPM_RPMDBID; i++)
	{
	  Repokey keyd;
	  keyd.name = i;
	  if (i < SOLVABLE_PROVIDES)
	    keyd.type = REPOKEY_TYPE_ID;
	  else if (i < RPM_RPMDBID)
	    keyd.type = REPOKEY_TYPE_IDARRAY;
	  else
	    keyd.type = REPOKEY_TYPE_NUM;
#ifdef USE_REL_IDARRAY
	  if (keyd.type == REPOKEY_TYPE_IDARRAY)
	    keyd.type = REPOKEY_TYPE_REL_IDARRAY;
#endif
	  keyd.size = 0;
	  keyd.storage = KEY_STORAGE_SOLVABLE;
	  if (writer->keyfilter)
	    {
	      keyd.storage = writer->keyfilter(repo, &keyd, writer->kfdata);
	      if (keyd.storage == KEY_STORAGE_DROPPED)
		continue;
	      keyd.storage = KEY_STORAGE_SOLVABLE;
	    }
#ifdef USE_IDARRAYBLOCK
	  if (keyd.type == REPOKEY_TYPE_IDARRAY)
	    keyd.storage = KEY_STORAGE_IDARRAYBLOCK;
#endif
	  poolusage = 1;
	  clonepool = 1;
	  keymap[keyd.name] = repodata_key2id(&target, &keyd, 1);
	}
    }

  if (repo->nsolvables)
    {
      Repokey keyd;
      keyd.name = REPOSITORY_SOLVABLES;
      keyd.type = REPOKEY_TYPE_FLEXARRAY;
      keyd.size = 0;
      keyd.storage = KEY_STORAGE_INCORE;
      keymap[keyd.name] = repodata_key2id(&target, &keyd, 1);
    }

  dirpoolusage = 0;

  spool = 0;
  dirpool = 0;
  dirpooldata = 0;
  n = ID_NUM_INTERNAL;
  FOR_REPODATAS(repo, i, data)
    {
      int idused, dirused;
      if (i < writer->repodatastart || i >= writer->repodataend)
	continue;
      if (writer->keyfilter && (writer->flags & REPOWRITER_LEGACY) != 0)
	{
	  /* ask keyfilter if we want this repodata */
	  Repokey keyd;
	  /* check if we want this repodata */
	  memset(&keyd, 0, sizeof(keyd));
	  keyd.name = 1;
	  keyd.type = 1;
	  keyd.size = i;
	  if (writer->keyfilter(repo, &keyd, writer->kfdata) == -1)
	    continue;
	}
      keymapstart[i] = n;
      keymap[n++] = 0;	/* key 0 */
      idused = dirused = 0;
      for (j = 1; j < data->nkeys; j++, n++)
	{
	  key = data->keys + j;
	  if (key->name == REPOSITORY_SOLVABLES && key->type == REPOKEY_TYPE_FLEXARRAY)
	    {
	      keymap[n] = keymap[key->name];
	      continue;
	    }
	  if (key->type == REPOKEY_TYPE_DELETED && (writer->flags & REPOWRITER_KEEP_TYPE_DELETED) == 0)
	    {
	      keymap[n] = 0;
	      continue;
	    }
	  if (key->type == REPOKEY_TYPE_CONSTANTID && data->localpool)
	    {
	      Repokey keyd = *key;
	      keyd.size = repodata_globalize_id(data, key->size, 1);
	      id = repodata_key2id(&target, &keyd, 0);
	    }
	  else
	    id = repodata_key2id(&target, key, 0);
	  if (!id)
	    {
	      /* a new key. ask keyfilter if we want it before creating it */
	      Repokey keyd = *key;
	      keyd.storage = KEY_STORAGE_INCORE;
	      if (keyd.type == REPOKEY_TYPE_CONSTANTID)
		keyd.size = repodata_globalize_id(data, key->size, 1);
	      else if (keyd.type != REPOKEY_TYPE_CONSTANT)
		keyd.size = 0;
	      if (writer->keyfilter)
		{
		  keyd.storage = writer->keyfilter(repo, &keyd, writer->kfdata);
		  if (keyd.storage == KEY_STORAGE_DROPPED)
		    {
		      keymap[n] = 0;
		      continue;
		    }
		  if (keyd.storage != KEY_STORAGE_VERTICAL_OFFSET)
		    keyd.storage = KEY_STORAGE_INCORE;		/* do not mess with us */
		}
	      if (data->state != REPODATA_STUB)
	        id = repodata_key2id(&target, &keyd, 1);
	    }
	  keymap[n] = id;
	  /* load repodata if not already loaded */
	  if (data->state == REPODATA_STUB)
	    {
	      int oldnkeys = data->nkeys;
	      repodata_load(data);
	      if (oldnkeys != data->nkeys)
		{
		  nkeymap += data->nkeys - oldnkeys;		/* grow/shrink keymap */
		  keymap = solv_realloc2(keymap, nkeymap, sizeof(Id));
		}
	      if (data->state == REPODATA_AVAILABLE)
		{
		  /* redo this repodata! */
		  j = 0;
		  n = keymapstart[i];
		  continue;
		}
	    }
	  if (data->state != REPODATA_AVAILABLE && data->state != REPODATA_LOADING)
	    {
	      /* too bad! */
	      keymap[n] = 0;
	      continue;
	    }

	  repodataused[i] = 1;
	  anyrepodataused = 1;
	  if (key->type == REPOKEY_TYPE_CONSTANTID || key->type == REPOKEY_TYPE_ID ||
              key->type == REPOKEY_TYPE_IDARRAY || key->type == REPOKEY_TYPE_REL_IDARRAY)
	    idused = 1;
	  else if (key->type == REPOKEY_TYPE_DIR || key->type == REPOKEY_TYPE_DIRNUMNUMARRAY || key->type == REPOKEY_TYPE_DIRSTRARRAY)
	    {
	      idused = 1;	/* dirs also use ids */
	      dirused = 1;
	    }
	}
      if (idused)
	{
	  if (data->localpool)
	    {
	      if (poolusage)
		poolusage = 3;	/* need own pool */
	      else
		{
		  poolusage = 2;
		  spool = &data->spool;
		}
	    }
	  else
	    {
	      if (poolusage == 0)
		poolusage = 1;
	      else if (poolusage != 1)
		poolusage = 3;	/* need own pool */
	    }
	}
      if (dirused)
	{
	  if (dirpoolusage)
	    dirpoolusage = 3;	/* need own dirpool */
	  else
	    {
	      dirpoolusage = 2;
	      dirpool = &data->dirpool;
	      dirpooldata = data;
	    }
	}
    }
  nkeymap = n;		/* update */

  /* 0: no pool needed at all */
  /* 1: use global pool */
  /* 2: use repodata local pool */
  /* 3: need own pool */
  if (poolusage != 3)
    clonepool = 0;
  if (poolusage == 3)
    {
      spool = &target.spool;
      target.localpool = 1;	/* so we can use repodata_translate */
      /* hack: reuse global pool data so we don't have to map pool ids */
      if (clonepool)
	{
	  stringpool_free(spool);
	  stringpool_clone(spool, &pool->ss);
	  cbdata.clonepool = 1;
	}
      cbdata.ownspool = spool;
    }
  else if (poolusage == 0 || poolusage == 1)
    {
      poolusage = 1;
      spool = &pool->ss;
    }

  if (dirpoolusage == 3)
    {
      /* dirpoolusage == 3 means that at least two repodata
       * areas have dir keys. This means that two areas have
       * idused set to 1, which results in poolusage being
       * either 1 (global pool) or 3 (own pool) */
      dirpool = &target.dirpool;
      dirpooldata = 0;
      cbdata.owndirpool = dirpool;
    }
  else if (dirpool)
    cbdata.dirused = solv_calloc(dirpool->ndirs, sizeof(Id));


/********************************************************************/
#if 0
fprintf(stderr, "poolusage: %d\n", poolusage);
fprintf(stderr, "dirpoolusage: %d\n", dirpoolusage);
fprintf(stderr, "clonepool: %d\n", clonepool);
fprintf(stderr, "nkeys: %d\n", target.nkeys);
for (i = 1; i < target.nkeys; i++)
  fprintf(stderr, "  %2d: %s[%d] %d %d %d\n", i, pool_id2str(pool, target.keys[i].name), target.keys[i].name, target.keys[i].type, target.keys[i].size, target.keys[i].storage);
#endif

/********************************************************************/

  searchflags = SEARCH_SUB|SEARCH_ARRAYSENTINEL;
  if ((writer->flags & REPOWRITER_KEEP_TYPE_DELETED) != 0)
    searchflags |= SEARCH_KEEP_TYPE_DELETED;

  /* set needed count of all strings and rels,
   * find which keys are used in the solvables
   * put all strings in own spool
   */

  reloff = spool->nstrings;
  if (cbdata.ownspool)
    reloff = (reloff + NEEDID_BLOCK) & ~NEEDID_BLOCK;
  else if (poolusage == 2)
    {
      /* we'll need to put the key data into the spool,
       * so leave some room. 3 * nkeys is an upper bound */
      reloff += 3 * target.nkeys;
    }

  needid = solv_calloc(reloff + pool->nrels, sizeof(*needid));
  needid[0].map = reloff;	/* remember size in case we need to grow */

  cbdata.needid = needid;
  cbdata.schema = solv_calloc(target.nkeys + 2, sizeof(Id));

  /* create main schema */
  cbdata.sp = cbdata.schema + 1;

  /* collect meta data from all repodatas */
  /* XXX: merge arrays of equal keys? */
  keyskip = create_keyskip(repo, SOLVID_META, repodataused, &oldkeyskip);
  FOR_REPODATAS(repo, j, data)
    {
      if (!repodataused[j])
	continue;
      cbdata.keymap = keymap + keymapstart[j];
      cbdata.lastdirid = 0;		/* clear dir mapping cache */
      repodata_search_keyskip(data, SOLVID_META, 0, searchflags, keyskip, collect_needed_cb, &cbdata);
    }
  needid = cbdata.needid;		/* maybe relocated */
  sp = cbdata.sp;
  /* add solvables if needed (may revert later) */
  if (repo->nsolvables)
    {
      *sp++ = keymap[REPOSITORY_SOLVABLES];
      target.keys[keymap[REPOSITORY_SOLVABLES]].size++;
    }
  *sp = 0;
  /* stash away main schema (including terminating zero) */
  mainschemakeys = solv_memdup2(cbdata.schema + 1, sp - cbdata.schema, sizeof(Id));

  /* collect data for all solvables */
  solvschemata = solv_calloc(repo->nsolvables, sizeof(Id));	/* allocate upper bound */
  solvablestart = writer->solvablestart < repo->start ? repo->start : writer->solvablestart;
  solvableend = writer->solvableend > repo->end ? repo->end : writer->solvableend;
  anysolvableused = 0;
  nsolvables = 0;		/* solvables we are going to write, will be <= repo->nsolvables */
  cbdata.doingsolvables = 1;
  for (i = solvablestart, s = pool->solvables + i; i < solvableend; i++, s++)
    {
      if (s->repo != repo)
	continue;

      cbdata.sp = cbdata.schema + 1;
      collect_needed_solvable(&cbdata, s, keymap);

      if (anyrepodataused)
	{
	  keyskip = create_keyskip(repo, i, repodataused, &oldkeyskip);
	  FOR_REPODATAS(repo, j, data)
	    {
	      if (!repodataused[j] || i < data->start || i >= data->end)
		continue;
	      cbdata.keymap = keymap + keymapstart[j];
	      cbdata.lastdirid = 0;
	      repodata_search_keyskip(data, i, 0, searchflags, keyskip, collect_needed_cb, &cbdata);
	    }
	  needid = cbdata.needid;		/* maybe relocated */
	}
      *cbdata.sp = 0;
      solvschemata[nsolvables] = repodata_schema2id(cbdata.target, cbdata.schema + 1, 1);
      if (solvschemata[nsolvables])
	anysolvableused = 1;
      nsolvables++;
    }
  cbdata.doingsolvables = 0;

  if (repo->nsolvables && !anysolvableused)
    {
      /* strip off REPOSITORY_SOLVABLES from the main schema */
      for (sp = mainschemakeys; *sp; sp++)
	;
      sp[-1] = 0;	/* strip last entry */
    }
  mainschema = repodata_schema2id(cbdata.target, mainschemakeys, 1);
  mainschemakeys = solv_free(mainschemakeys);

/********************************************************************/

  /* remove unused keys */
  keyused = solv_calloc(target.nkeys, sizeof(Id));
  for (i = 1; i < (int)target.schemadatalen; i++)
    keyused[target.schemadata[i]] = 1;
  keyused[0] = 0;
  for (n = i = 1; i < target.nkeys; i++)
    {
      if (!keyused[i])
	continue;
      if (i != n)
	target.keys[n] = target.keys[i];
      keyused[i] = n++;
    }
  target.nkeys = n;

  /* update schema data to the new key ids */
  for (i = 1; i < (int)target.schemadatalen; i++)
    target.schemadata[i] = keyused[target.schemadata[i]];
  /* update keymap to the new key ids */
  for (i = 0; i < nkeymap; i++)
    keymap[i] = keyused[keymap[i]];
  keyused = solv_free(keyused);

  /* copy keys if requested */
  if (writer->keyq)
    {
      queue_empty(writer->keyq);
      for (i = 1; i < target.nkeys; i++)
	queue_push2(writer->keyq, target.keys[i].name, target.keys[i].type);
    }

/********************************************************************/

  /* check if we can do the special filelist memory optimization
   * we do the check before the keys are mapped.
   * The optimization is done if there is just one vertical key and
   * it is of type REPOKEY_TYPE_DIRSTRARRAY */
  if (anysolvableused && anyrepodataused)
    {
      for (i = 1; i < target.nkeys; i++)
	{
	  if (target.keys[i].storage != KEY_STORAGE_VERTICAL_OFFSET)
	    continue;
	  if (target.keys[i].type != REPOKEY_TYPE_DIRSTRARRAY || cbdata.filelistmode != 0)
	    {
	      cbdata.filelistmode = 0;
	      break;
	    }
	  cbdata.filelistmode = i;
	}
    }

/********************************************************************/

  if (poolusage > 1)
    {
      /* put all the keys in our string pool */
      /* put mapped ids right into target.keys */
      for (i = 1, key = target.keys + i; i < target.nkeys; i++, key++)
	{
	  key->name = stringpool_str2id(spool, pool_id2str(pool, key->name), 1);
	  id = stringpool_str2id(spool, pool_id2str(pool, key->type), 1);
	  if (key->type == REPOKEY_TYPE_CONSTANTID)
	    {
	      type_constantid = id;
	      key->size = stringpool_str2id(spool, pool_id2str(pool, key->size), 1);
	    }
	  key->type = id;
	}
      if (poolusage == 2)
	stringpool_freehash(spool);	/* free some mem */
      if (cbdata.ownspool && spool->nstrings > needid[0].map)
	{
	  grow_needid(&cbdata, spool->nstrings - 1);
	  needid = cbdata.needid;		/* we relocated */
	}
    }
  else
    type_constantid = REPOKEY_TYPE_CONSTANTID;

  /* increment needid of the keys */
  for (i = 1; i < target.nkeys; i++)
    {
      if (target.keys[i].type == type_constantid)
	needid[target.keys[i].size].need++;
      needid[target.keys[i].name].need++;
      needid[target.keys[i].type].need++;
    }

/********************************************************************/

  /* increment need id of all relations
   * if we refer to another relation, make sure that the
   * need value is it is bigger than our value so that
   * ordering works.
   */
  reloff = needid[0].map;
  for (i = pool->nrels - 1, needidp = needid + (reloff + i); i > 0; i--, needidp--)
    if (needidp->need)
      break;
  if (i)
    {
      /* we have some relations with a non-zero need */
      Reldep *rd;

      for (rd = pool->rels + i; i > 0; i--, rd--)
	{
	  int need = needid[reloff + i].need;
	  if (!need)
	    continue;
	  id = rd->name;
	  if (ISRELDEP(id))
	    {
	      id = GETRELID(id);
	      if (needid[reloff + id].need < need + 1)
		needid[reloff + id].need = need + 1;
	    }
	  else
	    {
	      if (cbdata.ownspool && id > 1 && !cbdata.clonepool)
		{
		  id = stringpool_str2id(cbdata.ownspool, pool_id2str(pool, id), 1);
		  if (id >= cbdata.needid[0].map)
		    {
		      grow_needid(&cbdata, id);
		      needid = cbdata.needid;		/* we relocated */
		      reloff = needid[0].map;		/* we have a new offset */
		    }
		}
	      needid[id].need++;
	    }

	  id = rd->evr;
	  if (ISRELDEP(id))
	    {
	      id = GETRELID(id);
	      if (needid[reloff + id].need < need + 1)
		needid[reloff + id].need = need + 1;
	    }
	  else
	    {
	      if (cbdata.ownspool && id > 1 && !cbdata.clonepool)
		{
		  id = stringpool_str2id(cbdata.ownspool, pool_id2str(pool, id), 1);
		  if (id >= cbdata.needid[0].map)
		    {
		      grow_needid(&cbdata, id);
		      needid = cbdata.needid;		/* we relocated */
		      reloff = needid[0].map;		/* we have a new offset */
		    }
		}
	      needid[id].need++;
	    }
	}
  }

/********************************************************************/

  /* increment need id for used dir components */
  if (cbdata.owndirpool)
    {
      /* if we have own dirpool, all entries in it are used.
	 also, all comp ids are already mapped by putinowndirpool(),
	 so we can simply increment needid.
	 (owndirpool != 0, dirused == 0, dirpooldata == 0) */
      for (i = 1; i < dirpool->ndirs; i++)
	{
	  id = dirpool->dirs[i];
	  if (id <= 0)
	    continue;
	  needid[id].need++;
	}
    }
  else if (dirpool)
    {
      Id parent;
      /* else we re-use a dirpool of repodata "dirpooldata".
	 dirused tells us which of the ids are used.
	 we need to map comp ids if we generate a new pool.
	 (owndirpool == 0, dirused != 0, dirpooldata != 0) */
      for (i = dirpool->ndirs - 1; i > 0; i--)
	{
	  if (!cbdata.dirused[i])
	    continue;
	  parent = dirpool_parent(dirpool, i);	/* always < i */
	  cbdata.dirused[parent] = 2;		/* 2: used as parent */
	  id = dirpool->dirs[i];
	  if (id <= 0)
	    continue;
	  if (cbdata.ownspool && id > 1 && (!cbdata.clonepool || dirpooldata->localpool))
	    {
	      id = putinownpool(&cbdata, dirpooldata, id);
	      needid = cbdata.needid;
	    }
	  needid[id].need++;
	}
      if (!cbdata.dirused[0])
	{
          cbdata.dirused = solv_free(cbdata.dirused);
          dirpool = 0;
	}
    }


/********************************************************************/

  /*
   * create mapping table, new keys are sorted by needid[].need
   *
   * needid[key].need : old key -> new key
   * needid[key].map  : new key -> old key
   */

  /* zero out id 0 and rel 0 just in case */
  reloff = needid[0].map;
  needid[0].need = 0;
  needid[reloff].need = 0;

  for (i = 1; i < reloff + pool->nrels; i++)
    needid[i].map = i;

  /* make first entry '' */
  needid[1].need = 1;
  solv_sort(needid + 2, spool->nstrings - 2, sizeof(*needid), needid_cmp_need_s, spool);
  solv_sort(needid + reloff, pool->nrels, sizeof(*needid), needid_cmp_need, 0);
  /* now needid is in new order, needid[newid].map -> oldid */

  /* calculate string space size, also zero out needid[].need */
  sizeid = 0;
  for (i = 1; i < reloff; i++)
    {
      if (!needid[i].need)
        break;	/* as we have sorted, every entry after this also has need == 0 */
      needid[i].need = 0;
      sizeid += strlen(spool->stringspace + spool->strings[needid[i].map]) + 1;
    }
  nstrings = i;	/* our new string id end */

  /* make needid[oldid].need point to newid */
  for (i = 1; i < nstrings; i++)
    needid[needid[i].map].need = i;

  /* same as above for relations */
  for (i = 0; i < pool->nrels; i++)
    {
      if (!needid[reloff + i].need)
        break;
      needid[reloff + i].need = 0;
    }
  nrels = i;	/* our new rel id end */

  for (i = 0; i < nrels; i++)
    needid[needid[reloff + i].map].need = nstrings + i;

  /* now we have: needid[oldid].need -> newid
                  needid[newid].map  -> oldid
     both for strings and relations  */


/********************************************************************/

  ndirmap = 0;
  dirmap = 0;
  if (dirpool && dirpool->ndirs)
    {
      /* create our new target directory structure by traversing through all
       * used dirs. This will concatenate blocks with the same parent
       * directory into single blocks.
       * Instead of components, traverse_dirs stores the old dirids,
       * we will change this in the second step below */
      /* (dirpooldata and dirused are 0 if we have our own dirpool) */
      if (cbdata.dirused && !cbdata.dirused[1])
	{
	  cbdata.dirused[1] = 1;	/* always want / entry */
	  cbdata.dirused[0] = 2;	/* always want / entry */
	}
      dirmap = solv_calloc(dirpool->ndirs, sizeof(Id));
      dirmap[0] = 0;
      ndirmap = traverse_dirs(dirpool, dirmap, 1, dirpool_child(dirpool, 0), cbdata.dirused);

      /* (re)create dirused, so that it maps from "old dirid" to "new dirid" */
      /* change dirmap so that it maps from "new dirid" to "new compid" */
      if (!cbdata.dirused)
	cbdata.dirused = solv_malloc2(dirpool->ndirs, sizeof(Id));
      memset(cbdata.dirused, 0, dirpool->ndirs * sizeof(Id));
      for (i = 1; i < ndirmap; i++)
	{
	  if (dirmap[i] <= 0)
	    continue;
	  cbdata.dirused[dirmap[i]] = i;
	  id = dirpool->dirs[dirmap[i]];
	  if (dirpooldata && cbdata.ownspool && id > 1)
	    id = putinownpool(&cbdata, dirpooldata, id);
	  dirmap[i] = needid[id].need;
	}
      /* now the new target directory structure is complete (dirmap), and we have
       * dirused[olddirid] -> newdirid */
    }

/********************************************************************/

  /* collect all data
   * we use extdata[0] for incore data and extdata[keyid] for vertical data
   * we use extdata[nkeys] for the idarray_block data
   *
   * this must match the code above that creates the schema data!
   */

  cbdata.extdata = solv_calloc(target.nkeys + 1, sizeof(struct extdata));

  xd = cbdata.extdata;
  cbdata.current_sub = 0;
  /* add main schema */
  cbdata.lastlen = 0;
  data_addid(xd, mainschema);

  keyskip = create_keyskip(repo, SOLVID_META, repodataused, &oldkeyskip);
  FOR_REPODATAS(repo, j, data)
    {
      if (!repodataused[j])
	continue;
      cbdata.keymap = keymap + keymapstart[j];
      cbdata.lastdirid = 0;
      repodata_search_keyskip(data, SOLVID_META, 0, searchflags, keyskip, collect_data_cb, &cbdata);
    }
  if (xd->len - cbdata.lastlen > cbdata.maxdata)
    cbdata.maxdata = xd->len - cbdata.lastlen;
  cbdata.lastlen = xd->len;

  if (anysolvableused)
    {
      data_addid(xd, nsolvables);	/* FLEXARRAY nentries */
      cbdata.doingsolvables = 1;

      for (i = solvablestart, s = pool->solvables + i, n = 0; i < solvableend; i++, s++)
	{
	  if (s->repo != repo)
	    continue;
	  data_addid(xd, solvschemata[n]);
          collect_data_solvable(&cbdata, s, keymap);
	  if (anyrepodataused)
	    {
	      keyskip = create_keyskip(repo, i, repodataused, &oldkeyskip);
	      cbdata.vstart = -1;
	      FOR_REPODATAS(repo, j, data)
		{
		  if (!repodataused[j] || i < data->start || i >= data->end)
		    continue;
		  cbdata.keymap = keymap + keymapstart[j];
		  cbdata.lastdirid = 0;
		  repodata_search_keyskip(data, i, 0, searchflags, keyskip, collect_data_cb, &cbdata);
		}
	    }
	  if (xd->len - cbdata.lastlen > cbdata.maxdata)
	    cbdata.maxdata = xd->len - cbdata.lastlen;
	  cbdata.lastlen = xd->len;
	  n++;
	}
      cbdata.doingsolvables = 0;
    }

  assert(cbdata.current_sub == cbdata.nsubschemata);
  cbdata.subschemata = solv_free(cbdata.subschemata);
  cbdata.nsubschemata = 0;

/********************************************************************/

  target.fp = fp;

  /* write header */
  solv_flags = 0;
  solv_flags |= SOLV_FLAG_PREFIX_POOL;
  solv_flags |= SOLV_FLAG_SIZE_BYTES;
  if (writer->userdatalen)
    solv_flags |= SOLV_FLAG_USERDATA;
  if (cbdata.extdata[target.nkeys].len)
    solv_flags |= SOLV_FLAG_IDARRAYBLOCK;

  /* write file header */
  write_u32(&target, 'S' << 24 | 'O' << 16 | 'L' << 8 | 'V');
  if ((solv_flags & (SOLV_FLAG_USERDATA | SOLV_FLAG_IDARRAYBLOCK)) != 0)
    write_u32(&target, SOLV_VERSION_9);
  else
    write_u32(&target, SOLV_VERSION_8);

  /* write counts */
  write_u32(&target, nstrings);
  write_u32(&target, nrels);
  write_u32(&target, ndirmap);
  write_u32(&target, anysolvableused ? nsolvables : 0);
  write_u32(&target, target.nkeys);
  write_u32(&target, target.nschemata);
  write_u32(&target, solv_flags);

  /* write userdata */
  if ((solv_flags & SOLV_FLAG_USERDATA) != 0)
    {
      write_u32(&target, writer->userdatalen);
      write_blob(&target, writer->userdata, writer->userdatalen);
    }

  if (nstrings)
    {
      /*
       * calculate prefix encoding of the strings
       */
      unsigned char *prefixcomp = solv_malloc(nstrings);
      unsigned int compsum = 0;
      char *old_str = "";

      prefixcomp[0] = 0;
      for (i = 1; i < nstrings; i++)
	{
	  char *str = spool->stringspace + spool->strings[needid[i].map];
	  int same;
	  for (same = 0; same < 255; same++)
	    if (!old_str[same] || old_str[same] != str[same])
	      break;
	  prefixcomp[i] = same;
	  compsum += same;
	  old_str = str;
	}

      /*
       * write strings
       */
      write_u32(&target, sizeid);
      /* we save compsum bytes but need 1 extra byte for every string */
      write_u32(&target, sizeid + nstrings - 1 - compsum);
      for (i = 1; i < nstrings; i++)
	{
	  char *str = spool->stringspace + spool->strings[needid[i].map];
	  write_u8(&target, prefixcomp[i]);
	  write_str(&target, str + prefixcomp[i]);
	}
      solv_free(prefixcomp);
    }
  else
    {
      write_u32(&target, 0);	/* unpacked size */
      write_u32(&target, 0);	/* compressed size */
    }

  /*
   * write RelDeps
   */
  for (i = 0; i < nrels; i++)
    {
      Reldep *ran = pool->rels + (needid[reloff + i].map - reloff);
      write_id(&target, needid[NEEDIDOFF(ran->name)].need);
      write_id(&target, needid[NEEDIDOFF(ran->evr)].need);
      write_u8(&target, ran->flags);
    }

  /*
   * write dirs (skip both root and / entry)
   */
  for (i = 2; i < ndirmap; i++)
    {
      if (dirmap[i] > 0)
        write_id(&target, dirmap[i]);
      else
        write_id(&target, nstrings - dirmap[i]);
    }
  solv_free(dirmap);

  /*
   * write keys
   */
  for (i = 1; i < target.nkeys; i++)
    {
      write_id(&target, needid[target.keys[i].name].need);
      write_id(&target, needid[target.keys[i].type].need);
      if (target.keys[i].storage != KEY_STORAGE_VERTICAL_OFFSET)
	{
	  if (target.keys[i].type == type_constantid)
            write_id(&target, needid[target.keys[i].size].need);
	  else
            write_id(&target, target.keys[i].size);
	}
      else
        write_id(&target, cbdata.extdata[i].len);
      write_id(&target, target.keys[i].storage);
    }

  /*
   * write schemata
   */
  write_id(&target, target.schemadatalen);	/* XXX -1? */
  for (i = 1; i < target.nschemata; i++)
    write_idarray(&target, pool, 0, repodata_id2schema(&target, i));

  /* write idarray_block data if not empty */
  if (cbdata.extdata[target.nkeys].len)
    {
      unsigned int cnt = 0;
      unsigned char *b;
      unsigned int l;
	
      xd = cbdata.extdata + target.nkeys;
      /* calculate number of entries */
      for (l = xd->len, b = xd->buf; l--;)
	{
	  unsigned char x = *b++;
	  if ((x & 0x80) == 0)
	    cnt += (x & 0x40) ? 1 : 2;
	}
      write_id(&target, cnt);
      if (cnt)
        write_compressed_blob(&target, xd->buf, xd->len);
      solv_free(xd->buf);
    }

  /*
   * write incore data
   */
  xd = cbdata.extdata;
  write_id(&target, cbdata.maxdata);
  write_id(&target, xd->len);
  if (xd->len)
    write_blob(&target, xd->buf, xd->len);
  solv_free(xd->buf);

  /*
   * write vertical data if we have any
   */
  for (i = 1; i < target.nkeys; i++)
    if (cbdata.extdata[i].len)
      break;
  if (i < target.nkeys)
    {
      /* have vertical data, write it in pages */
      unsigned char vpage[REPOPAGE_BLOBSIZE];
      int lpage = 0;

      write_u32(&target, REPOPAGE_BLOBSIZE);
      if (!cbdata.filelistmode)
	{
	  for (i = 1; i < target.nkeys; i++)
	    if (cbdata.extdata[i].len)
	      lpage = write_compressed_extdata(&target, cbdata.extdata + i, vpage, lpage);
	}
      else
	{
	  /* ok, just one single extdata which is of type REPOKEY_TYPE_DIRSTRARRAY */
	  xd = cbdata.extdata + i;
	  xd->len = 0;
	  keyskip = create_keyskip(repo, SOLVID_META, repodataused, &oldkeyskip);
	  FOR_REPODATAS(repo, j, data)
	    {
	      if (!repodataused[j])
		continue;
	      cbdata.keymap = keymap + keymapstart[j];
	      cbdata.lastdirid = 0;
	      repodata_search_keyskip(data, SOLVID_META, 0, searchflags, keyskip, collect_filelist_cb, &cbdata);
	    }
	  for (i = solvablestart, s = pool->solvables + i; i < solvableend; i++, s++)
	    {
	      if (s->repo != repo)
		continue;
	      keyskip = create_keyskip(repo, i, repodataused, &oldkeyskip);
	      FOR_REPODATAS(repo, j, data)
		{
		  if (!repodataused[j] || i < data->start || i >= data->end)
		    continue;
		  cbdata.keymap = keymap + keymapstart[j];
		  cbdata.lastdirid = 0;
		  repodata_search_keyskip(data, i, 0, searchflags, keyskip, collect_filelist_cb, &cbdata);
		}
	      if (xd->len > 1024 * 1024)
		{
		  lpage = write_compressed_extdata(&target, xd, vpage, lpage);
		  xd->len = 0;
		}
	    }
	  if (xd->len)
	    lpage = write_compressed_extdata(&target, xd, vpage, lpage);
	}
      if (lpage)
	write_compressed_page(&target, vpage, lpage);
    }

  for (i = 1; i < target.nkeys; i++)
    solv_free(cbdata.extdata[i].buf);
  solv_free(cbdata.extdata);

  target.fp = 0;
  repodata_freedata(&target);

  solv_free(needid);
  solv_free(solvschemata);
  solv_free(cbdata.schema);

  solv_free(keymap);
  solv_free(keymapstart);
  solv_free(cbdata.dirused);
  solv_free(repodataused);
  solv_free(oldkeyskip);
  return target.error;
}

int
repo_write(Repo *repo, FILE *fp)
{
  int res;
  Repowriter *writer = repowriter_create(repo);
  res = repowriter_write(writer, fp);
  repowriter_free(writer);
  return res;
}

int
repodata_write(Repodata *data, FILE *fp)
{
  int res;
  Repowriter *writer = repowriter_create(data->repo);
  repowriter_set_repodatarange(writer, data->repodataid, data->repodataid + 1);
  repowriter_set_flags(writer, REPOWRITER_NO_STORAGE_SOLVABLE);
  res = repowriter_write(writer, fp);
  repowriter_free(writer);
  return res;
}

/* deprecated functions, do not use in new code! */
int
repo_write_filtered(Repo *repo, FILE *fp, int (*keyfilter)(Repo *repo, Repokey *key, void *kfdata), void *kfdata, Queue *keyq)
{
  int res;
  Repowriter *writer = repowriter_create(repo);
  repowriter_set_flags(writer, REPOWRITER_LEGACY);
  repowriter_set_keyfilter(writer, keyfilter, kfdata);
  repowriter_set_keyqueue(writer, keyq);
  res = repowriter_write(writer, fp);
  repowriter_free(writer);
  return res;
}

int
repodata_write_filtered(Repodata *data, FILE *fp, int (*keyfilter)(Repo *repo, Repokey *key, void *kfdata), void *kfdata, Queue *keyq)
{
  int res;
  Repowriter *writer = repowriter_create(data->repo);
  repowriter_set_repodatarange(writer, data->repodataid, data->repodataid + 1);
  repowriter_set_flags(writer, REPOWRITER_NO_STORAGE_SOLVABLE | REPOWRITER_LEGACY);
  repowriter_set_keyfilter(writer, keyfilter, kfdata);
  repowriter_set_keyqueue(writer, keyq);
  res = repowriter_write(writer, fp);
  repowriter_free(writer);
  return res;
}

