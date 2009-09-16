/*
 * Copyright (c) 2007, Novell Inc.
 *
 * This program is licensed under the BSD license, read LICENSE.BSD
 * for further information
 */

/*
 * repo_write.c
 * 
 * Write Repo data out to binary file
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

#include "pool.h"
#include "util.h"
#include "repo_write.h"
#include "repopage.h"

/*------------------------------------------------------------------*/
/* Id map optimizations */

typedef struct needid {
  Id need;
  Id map;
} NeedId;


#define RELOFF(id) (needid[0].map + GETRELID(id))

/*
 * increment need Id
 * idarray: array of Ids, ID_NULL terminated
 * needid: array of Id->NeedId
 * 
 * return count
 * 
 */

static void
incneedid(Pool *pool, Id id, NeedId *needid)
{
  while (ISRELDEP(id))
    {
      Reldep *rd = GETRELDEP(pool, id);
      needid[RELOFF(id)].need++;
      if (ISRELDEP(rd->evr))
	incneedid(pool, rd->evr, needid);
      else
	needid[rd->evr].need++;
      id = rd->name;
    }
  needid[id].need++;
}

static int
incneedidarray(Pool *pool, Id *idarray, NeedId *needid)
{
  Id id;
  int n = 0;

  if (!idarray)
    return 0;
  while ((id = *idarray++) != 0)
    {
      n++;
      while (ISRELDEP(id))
	{
	  Reldep *rd = GETRELDEP(pool, id);
	  needid[RELOFF(id)].need++;
	  if (ISRELDEP(rd->evr))
	    incneedid(pool, rd->evr, needid);
	  else
	    needid[rd->evr].need++;
	  id = rd->name;
	}
      needid[id].need++;
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

  int r;
  r = b->need - a->need;
  if (r)
    return r;
  const char *as = spool->stringspace + spool->strings[a->map];
  const char *bs = spool->stringspace + spool->strings[b->map];
  return strcmp(as, bs);
}


/*------------------------------------------------------------------*/
/* output helper routines */

/*
 * unsigned 32-bit
 */

static void
write_u32(FILE *fp, unsigned int x)
{
  if (putc(x >> 24, fp) == EOF ||
      putc(x >> 16, fp) == EOF ||
      putc(x >> 8, fp) == EOF ||
      putc(x, fp) == EOF)
    {
      perror("write error u32");
      exit(1);
    }
}


/*
 * unsigned 8-bit
 */

static void
write_u8(FILE *fp, unsigned int x)
{
  if (putc(x, fp) == EOF)
    {
      perror("write error u8");
      exit(1);
    }
}

/*
 * data blob
 */

static void
write_blob(FILE *fp, void *data, int len)
{
  if (len && fwrite(data, len, 1, fp) != 1)
    {
      perror("write error blob");
      exit(1);
    }
}

/*
 * Id
 */

static void
write_id(FILE *fp, Id x)
{
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
      perror("write error id");
      exit(1);
    }
}

static inline void
write_id_eof(FILE *fp, Id x, int eof)
{
  if (x >= 64)
    x = (x & 63) | ((x & ~63) << 1);
  write_id(fp, x | (eof ? 0 : 64));
}



static inline void
write_str(FILE *fp, const char *str)
{
  if (fputs(str, fp) == EOF || putc(0, fp) == EOF)
    {
      perror("write error str");
      exit(1);
    }
}

/*
 * Array of Ids
 */

static void
write_idarray(FILE *fp, Pool *pool, NeedId *needid, Id *ids)
{
  Id id;
  if (!ids)
    return;
  if (!*ids)
    {
      write_u8(fp, 0);
      return;
    }
  for (;;)
    {
      id = *ids++;
      if (needid)
        id = needid[ISRELDEP(id) ? RELOFF(id) : id].need;
      if (id >= 64)
	id = (id & 63) | ((id & ~63) << 1);
      if (!*ids)
	{
	  write_id(fp, id);
	  return;
	}
      write_id(fp, id | 64);
    }
}

static int
cmp_ids(const void *pa, const void *pb, void *dp)
{
  Id a = *(Id *)pa;
  Id b = *(Id *)pb;
  return a - b;
}

#if 0
static void
write_idarray_sort(FILE *fp, Pool *pool, NeedId *needid, Id *ids, Id marker)
{
  int len, i;
  Id lids[64], *sids;

  if (!ids)
    return;
  if (!*ids)
    {
      write_u8(fp, 0);
      return;
    }
  for (len = 0; len < 64 && ids[len]; len++)
    {
      Id id = ids[len];
      if (needid)
        id = needid[ISRELDEP(id) ? RELOFF(id) : id].need;
      lids[len] = id;
    }
  if (ids[len])
    {
      for (i = len + 1; ids[i]; i++)
	;
      sids = sat_malloc2(i, sizeof(Id));
      memcpy(sids, lids, 64 * sizeof(Id));
      for (; ids[len]; len++)
	{
	  Id id = ids[len];
	  if (needid)
            id = needid[ISRELDEP(id) ? RELOFF(id) : id].need;
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
    sat_sort(sids, i, sizeof(Id), cmp_ids, 0);
  if ((len - i) > 2)
    sat_sort(sids + i + 1, len - i - 1, sizeof(Id), cmp_ids, 0);

  Id id, old = 0;

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
      if (id >= 64)
	id = (id & 63) | ((id & ~63) << 1);
      write_id(fp, id | 64);
    }
  id = sids[i];
  if (id == marker)
    id = 0;
  else
    id = id - old + 1;
  if (id >= 64)
    id = (id & 63) | ((id & ~63) << 1);
  write_id(fp, id);
  if (sids != lids)
    sat_free(sids);
}
#endif


struct extdata {
  unsigned char *buf;
  int len;
};

struct cbdata {
  Repo *repo;

  Stringpool *ownspool;
  Dirpool *owndirpool;

  Repokey *mykeys;
  int nmykeys;

  Id *keymap;
  int nkeymap;
  Id *keymapstart;

  NeedId *needid;

  Id *schema;		/* schema construction space */
  Id *sp;		/* pointer in above */
  Id *oldschema, *oldsp;

  Id *myschemata;
  int nmyschemata;

  Id *myschemadata;
  int myschemadatalen;

  Id schematacache[256];

  Id *solvschemata;
  Id *extraschemata;
  Id *subschemata;
  int nsubschemata;
  int current_sub;

  struct extdata *extdata;

  Id *dirused;

  Id vstart;

  Id maxdata;
  Id lastlen;

  int doingsolvables;	/* working on solvables data */
};

#define NEEDED_BLOCK 1023
#define SCHEMATA_BLOCK 31
#define SCHEMATADATA_BLOCK 255
#define EXTDATA_BLOCK 4095

static inline void
data_addid(struct extdata *xd, Id x)
{
  unsigned char *dp;
  xd->buf = sat_extend(xd->buf, xd->len, 5, 1, EXTDATA_BLOCK);
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
data_addideof(struct extdata *xd, Id x, int eof)
{
  if (x >= 64)
    x = (x & 63) | ((x & ~63) << 1);
  data_addid(xd, (eof ? x: x | 64));
}

static void
data_addidarray_sort(struct extdata *xd, Pool *pool, NeedId *needid, Id *ids, Id marker)
{
  int len, i;
  Id lids[64], *sids;

  if (!ids)
    return;
  if (!*ids)
    {
      data_addid(xd, 0);
      return;
    }
  for (len = 0; len < 64 && ids[len]; len++)
    {
      Id id = ids[len];
      if (needid)
        id = needid[ISRELDEP(id) ? RELOFF(id) : id].need;
      lids[len] = id;
    }
  if (ids[len])
    {
      for (i = len + 1; ids[i]; i++)
	;
      sids = sat_malloc2(i, sizeof(Id));
      memcpy(sids, lids, 64 * sizeof(Id));
      for (; ids[len]; len++)
	{
	  Id id = ids[len];
	  if (needid)
            id = needid[ISRELDEP(id) ? RELOFF(id) : id].need;
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
    sat_sort(sids, i, sizeof(Id), cmp_ids, 0);
  if ((len - i) > 2)
    sat_sort(sids + i + 1, len - i - 1, sizeof(Id), cmp_ids, 0);

  Id id, old = 0;

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
      if (id >= 64)
	id = (id & 63) | ((id & ~63) << 1);
      data_addid(xd, id | 64);
    }
  id = sids[i];
  if (id == marker)
    id = 0;
  else
    id = id - old + 1;
  if (id >= 64)
    id = (id & 63) | ((id & ~63) << 1);
  data_addid(xd, id);
  if (sids != lids)
    sat_free(sids);
}

static inline void
data_addblob(struct extdata *xd, unsigned char *blob, int len)
{
  xd->buf = sat_extend(xd->buf, xd->len, len, 1, EXTDATA_BLOCK);
  memcpy(xd->buf + xd->len, blob, len);
  xd->len += len;
}

static inline void
data_addu32(struct extdata *xd, unsigned int num)
{
  unsigned char d[4];
  d[0] = num >> 24;
  d[1] = num >> 16;
  d[2] = num >> 8;
  d[3] = num;
  data_addblob(xd, d, 4);
}

static Id
addschema(struct cbdata *cbdata, Id *schema)
{
  int h, len;
  Id *sp, cid;

  for (sp = schema, len = 0, h = 0; *sp; len++)
    h = h * 7 + *sp++;
  h &= 255;
  len++;

  cid = cbdata->schematacache[h];
  if (cid)
    {
      if (!memcmp(cbdata->myschemadata + cbdata->myschemata[cid], schema, len * sizeof(Id)))
	return cid;
      /* cache conflict */
      for (cid = 1; cid < cbdata->nmyschemata; cid++)
	if (!memcmp(cbdata->myschemadata + cbdata->myschemata[cid], schema, len * sizeof(Id)))
	  return cid;
    }
  /* a new one. make room. */
  if (!cbdata->nmyschemata)
    {
      /* allocate schema 0, it is always empty */
      cbdata->myschemadata = sat_extend(cbdata->myschemadata, cbdata->myschemadatalen, 1, sizeof(Id), SCHEMATADATA_BLOCK);
      cbdata->myschemata = sat_extend(cbdata->myschemata, cbdata->nmyschemata, 1, sizeof(Id), SCHEMATA_BLOCK);
      cbdata->myschemata[0] = 0;
      cbdata->myschemadata[0] = 0;
      cbdata->nmyschemata = 1;
      cbdata->myschemadatalen = 1;
    }
  /* add schema */
  cbdata->myschemadata = sat_extend(cbdata->myschemadata, cbdata->myschemadatalen, len, sizeof(Id), SCHEMATADATA_BLOCK);
  cbdata->myschemata = sat_extend(cbdata->myschemata, cbdata->nmyschemata, 1, sizeof(Id), SCHEMATA_BLOCK);
  memcpy(cbdata->myschemadata + cbdata->myschemadatalen, schema, len * sizeof(Id));
  cbdata->myschemata[cbdata->nmyschemata] = cbdata->myschemadatalen;
  cbdata->myschemadatalen += len;
  cbdata->schematacache[h] = cbdata->nmyschemata;
  return cbdata->nmyschemata++;
}

static Id
putinownpool(struct cbdata *cbdata, Stringpool *ss, Id id)
{
  const char *str = stringpool_id2str(ss, id);
  id = stringpool_str2id(cbdata->ownspool, str, 1);
  if (id >= cbdata->needid[0].map)
    {
      int oldoff = cbdata->needid[0].map;
      int newoff = (id + 1 + NEEDED_BLOCK) & ~NEEDED_BLOCK;
      int nrels = cbdata->repo->pool->nrels;
      cbdata->needid = sat_realloc2(cbdata->needid, newoff + nrels, sizeof(NeedId));
      if (nrels)
	memmove(cbdata->needid + newoff, cbdata->needid + oldoff, nrels * sizeof(NeedId));
      memset(cbdata->needid + oldoff, 0, (newoff - oldoff) * sizeof(NeedId));
      cbdata->needid[0].map = newoff;
    }
  return id;
}

static Id
putinowndirpool(struct cbdata *cbdata, Repodata *data, Dirpool *dp, Id dir)
{
  Id compid, parent;

  parent = dirpool_parent(dp, dir);
  if (parent)
    parent = putinowndirpool(cbdata, data, dp, parent);
  compid = dp->dirs[dir];
  if (cbdata->ownspool && compid > 1)
    compid = putinownpool(cbdata, data->localpool ? &data->spool : &data->repo->pool->ss, compid);
  return dirpool_add_dir(cbdata->owndirpool, parent, compid, 1);
}

static inline void
setdirused(struct cbdata *cbdata, Dirpool *dp, Id dir)
{
  if (cbdata->dirused[dir])
    return;
  cbdata->dirused[dir] = 1;
  while ((dir = dirpool_parent(dp, dir)) != 0)
    {
      if (cbdata->dirused[dir] == 2)
	return;
      if (cbdata->dirused[dir])
        {
	  cbdata->dirused[dir] = 2;
	  return;
        }
      cbdata->dirused[dir] = 2;
    }
  cbdata->dirused[0] = 2;
}

static int
repo_write_collect_needed(struct cbdata *cbdata, Repo *repo, Repodata *data, Repokey *key, KeyValue *kv)
{
  Id id;
  int rm;

  if (key->name == REPOSITORY_SOLVABLES)
    return SEARCH_NEXT_KEY;	/* we do not want this one */
  if (data != data->repo->repodata + data->repo->nrepodata - 1)
    if (key->name == REPOSITORY_ADDEDFILEPROVIDES || key->name == REPOSITORY_EXTERNAL || key->name == REPOSITORY_LOCATION || key->name == REPOSITORY_KEYS)
      return SEARCH_NEXT_KEY;

  rm = cbdata->keymap[cbdata->keymapstart[data - data->repo->repodata] + (key - data->keys)];
  if (!rm)
    return SEARCH_NEXT_KEY;	/* we do not want this one */
  /* record key in schema */
  if ((key->type != REPOKEY_TYPE_FIXARRAY || kv->eof == 0)
      && (cbdata->sp == cbdata->schema || cbdata->sp[-1] != rm))
    *cbdata->sp++ = rm;
  switch(key->type)
    {
      case REPOKEY_TYPE_ID:
      case REPOKEY_TYPE_IDARRAY:
	id = kv->id;
	if (!ISRELDEP(id) && cbdata->ownspool && id > 1)
	  id = putinownpool(cbdata, data->localpool ? &data->spool : &repo->pool->ss, id);
	incneedid(repo->pool, id, cbdata->needid);
	break;
      case REPOKEY_TYPE_DIR:
      case REPOKEY_TYPE_DIRNUMNUMARRAY:
      case REPOKEY_TYPE_DIRSTRARRAY:
	id = kv->id;
	if (cbdata->owndirpool)
	  putinowndirpool(cbdata, data, &data->dirpool, id);
	else
	  setdirused(cbdata, &data->dirpool, id);
	break;
      case REPOKEY_TYPE_FIXARRAY:
	if (kv->eof == 0)
	  {
	    if (cbdata->oldschema)
	      {
		fprintf(stderr, "nested structs not yet implemented\n");
		exit(1);
	      }
	    cbdata->oldschema = cbdata->schema;
	    cbdata->oldsp = cbdata->sp;
	    cbdata->schema = sat_calloc(cbdata->nmykeys, sizeof(Id));
	    cbdata->sp = cbdata->schema;
	  }
	else if (kv->eof == 1)
	  {
	    cbdata->current_sub++;
	    *cbdata->sp = 0;
	    cbdata->subschemata = sat_extend(cbdata->subschemata, cbdata->nsubschemata, 1, sizeof(Id), SCHEMATA_BLOCK);
	    cbdata->subschemata[cbdata->nsubschemata++] = addschema(cbdata, cbdata->schema);
#if 0
	    fprintf(stderr, "Have schema %d\n", cbdata->subschemata[cbdata->nsubschemata-1]);
#endif
	    cbdata->sp = cbdata->schema;
	  }
	else
	  {
	    sat_free(cbdata->schema);
	    cbdata->schema = cbdata->oldschema;
	    cbdata->sp = cbdata->oldsp;
	    cbdata->oldsp = cbdata->oldschema = 0;
	  }
	break;
      case REPOKEY_TYPE_FLEXARRAY:
	if (kv->entry == 0)
	  {
	    if (kv->eof != 2)
	      *cbdata->sp++ = 0;	/* mark start */
	  }
	else
	  {
	    /* just finished a schema, rewind */
	    Id *sp = cbdata->sp - 1;
	    *sp = 0;
	    while (sp[-1])
	      sp--;
	    cbdata->subschemata = sat_extend(cbdata->subschemata, cbdata->nsubschemata, 1, sizeof(Id), SCHEMATA_BLOCK);
	    cbdata->subschemata[cbdata->nsubschemata++] = addschema(cbdata, sp);
	    cbdata->sp = kv->eof == 2 ? sp - 1: sp;
	  }
	break;
      default:
	break;
    }
  return 0;
}

static int
repo_write_cb_needed(void *vcbdata, Solvable *s, Repodata *data, Repokey *key, KeyValue *kv)
{
  struct cbdata *cbdata = vcbdata;
  Repo *repo = data->repo;

#if 0
  if (s)
    fprintf(stderr, "solvable %d (%s): key (%d)%s %d\n", s ? s - repo->pool->solvables : 0, s ? id2str(repo->pool, s->name) : "", key->name, id2str(repo->pool, key->name), key->type);
#endif
  return repo_write_collect_needed(cbdata, repo, data, key, kv);
}

static int
repo_write_adddata(struct cbdata *cbdata, Repodata *data, Repokey *key, KeyValue *kv)
{
  int rm;
  Id id;
  unsigned int u32;
  unsigned char v[4];
  struct extdata *xd;

  if (key->name == REPOSITORY_SOLVABLES)
    return SEARCH_NEXT_KEY;
  if (data != data->repo->repodata + data->repo->nrepodata - 1)
    if (key->name == REPOSITORY_ADDEDFILEPROVIDES || key->name == REPOSITORY_EXTERNAL || key->name == REPOSITORY_LOCATION || key->name == REPOSITORY_KEYS)
      return SEARCH_NEXT_KEY;

  rm = cbdata->keymap[cbdata->keymapstart[data - data->repo->repodata] + (key - data->keys)];
  if (!rm)
    return SEARCH_NEXT_KEY;	/* we do not want this one */
  
  if (cbdata->mykeys[rm].storage == KEY_STORAGE_VERTICAL_OFFSET)
    {
      xd = cbdata->extdata + rm;	/* vertical buffer */
      if (cbdata->vstart == -1)
        cbdata->vstart = xd->len;
    }
  else
    xd = cbdata->extdata + 0;		/* incore buffer */
  switch(key->type)
    {
      case REPOKEY_TYPE_VOID:
      case REPOKEY_TYPE_CONSTANT:
      case REPOKEY_TYPE_CONSTANTID:
	break;
      case REPOKEY_TYPE_ID:
	id = kv->id;
	if (!ISRELDEP(id) && cbdata->ownspool && id > 1)
	  id = putinownpool(cbdata, data->localpool ? &data->spool : &data->repo->pool->ss, id);
	id = cbdata->needid[id].need;
	data_addid(xd, id);
	break;
      case REPOKEY_TYPE_IDARRAY:
	id = kv->id;
	if (cbdata->ownspool && id > 1)
	  id = putinownpool(cbdata, data->localpool ? &data->spool : &data->repo->pool->ss, id);
	id = cbdata->needid[id].need;
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
      case REPOKEY_TYPE_SHA256:
	data_addblob(xd, (unsigned char *)kv->str, SIZEOF_SHA256);
	break;
      case REPOKEY_TYPE_U32:
	u32 = kv->num;
	v[0] = u32 >> 24;
	v[1] = u32 >> 16;
	v[2] = u32 >> 8;
	v[3] = u32;
	data_addblob(xd, v, 4);
	break;
      case REPOKEY_TYPE_NUM:
	data_addid(xd, kv->num);
	break;
      case REPOKEY_TYPE_DIR:
	id = kv->id;
	if (cbdata->owndirpool)
	  id = putinowndirpool(cbdata, data, &data->dirpool, id);
	id = cbdata->dirused[id];
	data_addid(xd, id);
	break;
      case REPOKEY_TYPE_DIRNUMNUMARRAY:
	id = kv->id;
	if (cbdata->owndirpool)
	  id = putinowndirpool(cbdata, data, &data->dirpool, id);
	id = cbdata->dirused[id];
	data_addid(xd, id);
	data_addid(xd, kv->num);
	data_addideof(xd, kv->num2, kv->eof);
	break;
      case REPOKEY_TYPE_DIRSTRARRAY:
	id = kv->id;
	if (cbdata->owndirpool)
	  id = putinowndirpool(cbdata, data, &data->dirpool, id);
	id = cbdata->dirused[id];
	data_addideof(xd, id, kv->eof);
	data_addblob(xd, (unsigned char *)kv->str, strlen(kv->str) + 1);
	break;
      case REPOKEY_TYPE_FIXARRAY:
	if (kv->eof == 0)
	  {
	    if (kv->num)
	      {
		data_addid(xd, kv->num);
		data_addid(xd, cbdata->subschemata[cbdata->current_sub]);
#if 0
		fprintf(stderr, "writing %d %d\n", kv->num, cbdata->subschemata[cbdata->current_sub]);
#endif
	      }
	  }
	else if (kv->eof == 1)
	  {
	    cbdata->current_sub++;
	  }
	else
	  {
	  }
	break;
      case REPOKEY_TYPE_FLEXARRAY:
	if (!kv->entry)
	  data_addid(xd, kv->num);
	if (kv->eof != 2)
	  data_addid(xd, cbdata->subschemata[cbdata->current_sub++]);
	if (xd == cbdata->extdata + 0 && !kv->parent && !cbdata->doingsolvables)
	  {
	    if (xd->len - cbdata->lastlen > cbdata->maxdata)
	      cbdata->maxdata = xd->len - cbdata->lastlen;
	    cbdata->lastlen = xd->len;
	  }
	break;
      default:
	fprintf(stderr, "unknown type for %d: %d\n", key->name, key->type);
	exit(1);
    }
  if (cbdata->mykeys[rm].storage == KEY_STORAGE_VERTICAL_OFFSET && kv->eof)
    {
      /* we can re-use old data in the blob here! */
      data_addid(cbdata->extdata + 0, cbdata->vstart);			/* add offset into incore data */
      data_addid(cbdata->extdata + 0, xd->len - cbdata->vstart);	/* add length into incore data */
      cbdata->vstart = -1;
    }
  return 0;
}

static int
repo_write_cb_adddata(void *vcbdata, Solvable *s, Repodata *data, Repokey *key, KeyValue *kv)
{
  struct cbdata *cbdata = vcbdata;
  return repo_write_adddata(cbdata, data, key, kv);
}

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
  lastn = n;
  for (; parent < lastn; parent++)
    {
      sib = dirmap[parent];
      if (used && used[sib] != 2)
	continue;
      child = dirpool_child(dp, sib);
      if (child)
	{
	  dirmap[n++] = -parent;
	  n = traverse_dirs(dp, dirmap, n, child, used);
	}
    }
  return n;
}

static void
write_compressed_page(FILE *fp, unsigned char *page, int len)
{
  int clen;
  unsigned char cpage[BLOB_PAGESIZE];

  clen = repopagestore_compress_page(page, len, cpage, len - 1);
  if (!clen)
    {
      write_u32(fp, len * 2);
      write_blob(fp, page, len);
    }
  else
    {
      write_u32(fp, clen * 2 + 1);
      write_blob(fp, cpage, clen);
    }
}


#if 0
static Id subfilekeys[] = {
  REPODATA_INFO, REPOKEY_TYPE_VOID,
  REPODATA_EXTERNAL, REPOKEY_TYPE_VOID,
  REPODATA_KEYS, REPOKEY_TYPE_IDARRAY,
  REPODATA_LOCATION, REPOKEY_TYPE_STR,
  REPODATA_ADDEDFILEPROVIDES, REPOKEY_TYPE_REL_IDARRAY,
  REPODATA_RPMDBCOOKIE, REPOKEY_TYPE_SHA256,
  0,
};
#endif

static Id verticals[] = {
  SOLVABLE_AUTHORS,
  SOLVABLE_DESCRIPTION,
  SOLVABLE_MESSAGEDEL,
  SOLVABLE_MESSAGEINS,
  SOLVABLE_EULA,
  SOLVABLE_DISKUSAGE,
  SOLVABLE_FILELIST,
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
  keyname = id2str(repo->pool, key->name);
  for (i = 0; languagetags[i] != 0; i++)
    if (!strncmp(keyname, languagetags[i], strlen(languagetags[i])))
      return KEY_STORAGE_VERTICAL_OFFSET;
  return KEY_STORAGE_INCORE;
}

/*
 * Repo
 */

void
repo_write(Repo *repo, FILE *fp, int (*keyfilter)(Repo *repo, Repokey *key, void *kfdata), void *kfdata, Id **keyarrayp)
{
  Pool *pool = repo->pool;
  int i, j, k, n;
  Solvable *s;
  NeedId *needid;
  int nstrings, nrels;
  unsigned int sizeid;
  unsigned int solv_flags;
  Reldep *ran;
  Id *idarraydata;

  Id id, *sp;

  Id *dirmap;
  int ndirmap;
  Id *keyused;
  unsigned char *repodataused;
  int anyrepodataused;
  
  struct cbdata cbdata;
  int needrels;
  Repokey *key;
  int poolusage, dirpoolusage, idused, dirused;
  int reloff;

  Repodata *data, *dirpooldata = 0;
  Stringpool ownspool, *spool;
  Dirpool owndirpool, *dirpool;

  Id *repodataschemata = 0;
  Id mainschema;

  struct extdata *xd;

  Id type_constantid = 0;

  memset(&cbdata, 0, sizeof(cbdata));
  cbdata.repo = repo;

  /* go through all repodata and find the keys we need */
  /* also unify keys */
  /* creates: mykeys      - key array, still has global pool ids */
  /*          keymapstart - maps repo number to keymap offset */
  /*          keymap      - maps repo key to my key, 0 -> not used */

  /* start with all KEY_STORAGE_SOLVABLE ids */

  n = ID_NUM_INTERNAL;
  for (i = 0; i < repo->nrepodata; i++)
    n += repo->repodata[i].nkeys;
  cbdata.mykeys = sat_calloc(n, sizeof(Repokey));
  cbdata.keymap = sat_calloc(n, sizeof(Id));
  cbdata.keymapstart = sat_calloc(repo->nrepodata, sizeof(Id));
  repodataused = sat_calloc(repo->nrepodata, 1);

  cbdata.nmykeys = 1;
  needrels = 0;
  poolusage = 0;
  for (i = SOLVABLE_NAME; i <= RPM_RPMDBID; i++)
    {
      key = cbdata.mykeys + i;
      key->name = i;
      if (i < SOLVABLE_PROVIDES)
        key->type = REPOKEY_TYPE_ID;
      else if (i < RPM_RPMDBID)
        key->type = REPOKEY_TYPE_REL_IDARRAY;
      else
        key->type = REPOKEY_TYPE_U32;
      key->size = 0;
      key->storage = KEY_STORAGE_SOLVABLE;
      if (keyfilter)
	{
	  key->storage = keyfilter(repo, key, kfdata);
	  if (key->storage == KEY_STORAGE_DROPPED)
	    continue;
	  key->storage = KEY_STORAGE_SOLVABLE;
	}
      poolusage = 1;
      if (key->type == REPOKEY_TYPE_IDARRAY || key->type == REPOKEY_TYPE_REL_IDARRAY)
	needrels = 1;
      cbdata.keymap[i] = i;
    }
  cbdata.nmykeys = i;

  if (repo->nsolvables)
    {
      key = cbdata.mykeys + cbdata.nmykeys;
      key->name = REPOSITORY_SOLVABLES;
      key->type = REPOKEY_TYPE_FLEXARRAY;
      key->size = 0;
      key->storage = KEY_STORAGE_INCORE;
      cbdata.keymap[key->name] = cbdata.nmykeys++;
    }

#if 0
  /* If we store subfile info, generate the necessary keys.  */
  if (nsubfiles)
    {
      for (i = 0; subfilekeys[i]; i += 2)
	{
	  key = cbdata.mykeys + cbdata.nmykeys;
	  key->name = subfilekeys[i];
	  key->type = subfilekeys[i + 1];
	  key->size = 0;
	  key->storage = KEY_STORAGE_SOLVABLE;
	  cbdata.keymap[key->name] = cbdata.nmykeys++;
	}
    }
#endif

  dirpoolusage = 0;

  spool = 0;
  dirpool = 0;
  n = ID_NUM_INTERNAL;
  for (i = 0; i < repo->nrepodata; i++)
    {
      data = repo->repodata + i;
      cbdata.keymapstart[i] = n;
      cbdata.keymap[n++] = 0;	/* key 0 */
      idused = 0;
      dirused = 0;
      if (keyfilter)
	{
	  Repokey zerokey;
	  /* check if we want this repodata */
	  memset(&zerokey, 0, sizeof(zerokey));
	  zerokey.name = 1;
	  zerokey.type = 1;
	  zerokey.size = i;
	  if (keyfilter(repo, &zerokey, kfdata) == -1)
	    continue;
	}
      for (j = 1; j < data->nkeys; j++, n++)
	{
	  key = data->keys + j;
	  if (key->name == REPOSITORY_SOLVABLES && key->type == REPOKEY_TYPE_FLEXARRAY)
	    {
	      cbdata.keymap[n] = cbdata.keymap[key->name];
	      continue;
	    }
	  /* see if we already had this one, should use hash for fast miss */
	  for (k = 0; k < cbdata.nmykeys; k++)
	    {
	      if (key->name == cbdata.mykeys[k].name && key->type == cbdata.mykeys[k].type)
		{
		  if ((key->type == REPOKEY_TYPE_CONSTANT || key->type == REPOKEY_TYPE_CONSTANTID) && key->size != cbdata.mykeys[k].size)
		    continue;
		  break;
		}
	    }
	  if (k < cbdata.nmykeys)
	    cbdata.keymap[n] = k;
          else
	    {
	      /* found a new key! */
	      cbdata.mykeys[cbdata.nmykeys] = *key;
	      key = cbdata.mykeys + cbdata.nmykeys;
	      key->storage = KEY_STORAGE_INCORE;
	      if (key->type != REPOKEY_TYPE_CONSTANT && key->type != REPOKEY_TYPE_CONSTANTID)
		key->size = 0;
	      if (keyfilter)
		{
		  key->storage = keyfilter(repo, key, kfdata);
		  if (key->storage == KEY_STORAGE_DROPPED)
		    {
		      cbdata.keymap[n] = 0;
		      continue;
		    }
		}
	      cbdata.keymap[n] = cbdata.nmykeys++;
	    }
	  /* load repodata if not already loaded */
	  if (data->state == REPODATA_STUB)
	    {
	      if (data->loadcallback)
		data->loadcallback(data);
	      else
		data->state = REPODATA_ERROR;
	      if (data->state != REPODATA_ERROR)
		{
		  /* redo this repodata! */
		  j = 0;
		  n = cbdata.keymapstart[i];
		  continue;
		}
	    }
	  if (data->state == REPODATA_ERROR)
	    {
	      /* too bad! */
	      cbdata.keymap[n] = 0;
	      continue;
	    }

	  repodataused[i] = 1;
	  anyrepodataused = 1;
	  if (key->type != REPOKEY_TYPE_STR
	      && key->type != REPOKEY_TYPE_U32
	      && key->type != REPOKEY_TYPE_MD5
	      && key->type != REPOKEY_TYPE_SHA1)
	    idused = 1;
	  if (key->type == REPOKEY_TYPE_DIR || key->type == REPOKEY_TYPE_DIRNUMNUMARRAY || key->type == REPOKEY_TYPE_DIRSTRARRAY)
	    dirused = 1;
	  /* make sure we know that key */
	  if (data->localpool)
	    {
	      stringpool_str2id(&data->spool, id2str(pool, key->name), 1);
	      stringpool_str2id(&data->spool, id2str(pool, key->type), 1);
	      if (key->type == REPOKEY_TYPE_CONSTANTID)
	        stringpool_str2id(&data->spool, id2str(pool, key->size), 1);
	    }
	}
      if (idused)
	{
	  if (data->localpool)
	    {
	      if (poolusage)
		poolusage = 3;	/* need local pool */
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
		poolusage = 3;	/* need local pool */
	    }
	}
      if (dirused)
	{
	  if (dirpoolusage)
	    dirpoolusage = 3;	/* need local dirpool */
	  else
	    {
	      dirpoolusage = 2;
	      dirpool = &data->dirpool;
	      dirpooldata = data;
	    }
	}
    }
  cbdata.nkeymap = n;

  /* 0: no pool needed at all */
  /* 1: use global pool */
  /* 2: use repodata local pool */
  /* 3: need own pool */
  if (poolusage == 3)
    {
      spool = &ownspool;
      if (needrels)
	{
	  /* hack: reuse global pool so we don't have to map rel ids */
	  stringpool_clone(spool, &repo->pool->ss);
	}
      else
	stringpool_init_empty(spool);
      cbdata.ownspool = spool;
    }
  else if (poolusage == 0 || poolusage == 1)
    {
      poolusage = 1;
      spool = &repo->pool->ss;
    }
  if (dirpoolusage == 3)
    {
      dirpool = &owndirpool;
      dirpooldata = 0;
      dirpool_init(dirpool);
      cbdata.owndirpool = dirpool;
    }
  else if (dirpool)
    cbdata.dirused = sat_calloc(dirpool->ndirs, sizeof(Id));


/********************************************************************/
#if 0
fprintf(stderr, "poolusage: %d\n", poolusage);
fprintf(stderr, "dirpoolusage: %d\n", dirpoolusage);
fprintf(stderr, "nmykeys: %d\n", cbdata.nmykeys);
for (i = 1; i < cbdata.nmykeys; i++)
  fprintf(stderr, "  %2d: %s[%d] %d %d %d\n", i, id2str(pool, cbdata.mykeys[i].name), cbdata.mykeys[i].name, cbdata.mykeys[i].type, cbdata.mykeys[i].size, cbdata.mykeys[i].storage);
#endif

/********************************************************************/

  /* set needed count of all strings and rels,
   * find which keys are used in the solvables
   * put all strings in own spool
   */

  reloff = spool->nstrings;
  if (poolusage == 3)
    reloff = (reloff + NEEDED_BLOCK) & ~NEEDED_BLOCK;

  needid = calloc(reloff + pool->nrels, sizeof(*needid));
  needid[0].map = reloff;

  cbdata.needid = needid;
  cbdata.schema = sat_calloc(cbdata.nmykeys, sizeof(Id));
  cbdata.sp = cbdata.schema;
  cbdata.solvschemata = sat_calloc(repo->nsolvables, sizeof(Id));
#if 0
  cbdata.extraschemata = sat_calloc(repo->nextra, sizeof(Id));
#endif

  /* create main schema */
  cbdata.sp = cbdata.schema;
  /* collect all other data from all repodatas */
  /* XXX: merge arrays of equal keys? */
  for (j = 0, data = repo->repodata; j < repo->nrepodata; j++, data++)
    {
      if (!repodataused[j])
	continue;
      repodata_search(data, SOLVID_META, 0, SEARCH_SUB|SEARCH_ARRAYSENTINEL, repo_write_cb_needed, &cbdata);
    }
  sp = cbdata.sp;
  /* add solvables if needed */
  if (repo->nsolvables)
    {
      *sp++ = cbdata.keymap[REPOSITORY_SOLVABLES];
      cbdata.mykeys[cbdata.keymap[REPOSITORY_SOLVABLES]].size++;
    }
  *sp = 0;
  mainschema = addschema(&cbdata, cbdata.schema);


  idarraydata = repo->idarraydata;

  cbdata.doingsolvables = 1;
  for (i = repo->start, s = pool->solvables + i, n = 0; i < repo->end; i++, s++)
    {
      if (s->repo != repo)
	continue;

      /* set schema info, keep in sync with further down */
      sp = cbdata.schema;
      if (cbdata.keymap[SOLVABLE_NAME])
	{
          *sp++ = SOLVABLE_NAME;
	  needid[s->name].need++;
	}
      if (cbdata.keymap[SOLVABLE_ARCH])
	{
          *sp++ = SOLVABLE_ARCH;
	  needid[s->arch].need++;
	}
      if (cbdata.keymap[SOLVABLE_EVR])
	{
          *sp++ = SOLVABLE_EVR;
	  needid[s->evr].need++;
	}
      if (s->vendor && cbdata.keymap[SOLVABLE_VENDOR])
	{
          *sp++ = SOLVABLE_VENDOR;
	  needid[s->vendor].need++;
	}
      if (s->provides && cbdata.keymap[SOLVABLE_PROVIDES])
        {
          *sp++ = SOLVABLE_PROVIDES;
	  cbdata.mykeys[SOLVABLE_PROVIDES].size += incneedidarray(pool, idarraydata + s->provides, needid);
	}
      if (s->obsoletes && cbdata.keymap[SOLVABLE_OBSOLETES])
	{
          *sp++ = SOLVABLE_OBSOLETES;
	  cbdata.mykeys[SOLVABLE_OBSOLETES].size += incneedidarray(pool, idarraydata + s->obsoletes, needid);
	}
      if (s->conflicts && cbdata.keymap[SOLVABLE_CONFLICTS])
	{
          *sp++ = SOLVABLE_CONFLICTS;
	  cbdata.mykeys[SOLVABLE_CONFLICTS].size += incneedidarray(pool, idarraydata + s->conflicts, needid);
	}
      if (s->requires && cbdata.keymap[SOLVABLE_REQUIRES])
	{
          *sp++ = SOLVABLE_REQUIRES;
	  cbdata.mykeys[SOLVABLE_REQUIRES].size += incneedidarray(pool, idarraydata + s->requires, needid);
	}
      if (s->recommends && cbdata.keymap[SOLVABLE_RECOMMENDS])
	{
          *sp++ = SOLVABLE_RECOMMENDS;
	  cbdata.mykeys[SOLVABLE_RECOMMENDS].size += incneedidarray(pool, idarraydata + s->recommends, needid);
	}
      if (s->suggests && cbdata.keymap[SOLVABLE_SUGGESTS])
	{
          *sp++ = SOLVABLE_SUGGESTS;
	  cbdata.mykeys[SOLVABLE_SUGGESTS].size += incneedidarray(pool, idarraydata + s->suggests, needid);
	}
      if (s->supplements && cbdata.keymap[SOLVABLE_SUPPLEMENTS])
	{
          *sp++ = SOLVABLE_SUPPLEMENTS;
	  cbdata.mykeys[SOLVABLE_SUPPLEMENTS].size += incneedidarray(pool, idarraydata + s->supplements, needid);
	}
      if (s->enhances && cbdata.keymap[SOLVABLE_ENHANCES])
	{
          *sp++ = SOLVABLE_ENHANCES;
	  cbdata.mykeys[SOLVABLE_ENHANCES].size += incneedidarray(pool, idarraydata + s->enhances, needid);
	}
      if (repo->rpmdbid && cbdata.keymap[RPM_RPMDBID])
	{
          *sp++ = RPM_RPMDBID;
	  cbdata.mykeys[RPM_RPMDBID].size++;
	}
      cbdata.sp = sp;

      if (anyrepodataused)
	{
	  for (j = 0, data = repo->repodata; j < repo->nrepodata; j++, data++)
	    {
	      if (!repodataused[j])
		continue;
	      if (i < data->start || i >= data->end)
		continue;
	      repodata_search(data, i, 0, SEARCH_SUB|SEARCH_ARRAYSENTINEL, repo_write_cb_needed, &cbdata);
	      needid = cbdata.needid;
	    }
	}
      *cbdata.sp = 0;
      cbdata.solvschemata[n] = addschema(&cbdata, cbdata.schema);
      n++;
    }
  cbdata.doingsolvables = 0;
  assert(n == repo->nsolvables);

#if 0
  if (repo->nextra && anyrepodataused)
    for (i = -1; i >= -repo->nextra; i--)
      {
	Dataiterator di;
	dataiterator_init(&di, repo, i, 0, 0, SEARCH_EXTRA | SEARCH_NO_STORAGE_SOLVABLE);
	cbdata.sp = cbdata.schema;
	while (dataiterator_step(&di))
	  repo_write_collect_needed(&cbdata, repo, di.data, di.key, &di.kv);
	*cbdata.sp = 0;
	cbdata.extraschemata[-1 - i] = addschema(&cbdata, cbdata.schema);
      }

  /* If we have fileinfos to write, setup schemas and increment needid[]
     of the right strings.  */
  for (i = 0; i < nsubfiles; i++)
    {
      int j;
      Id schema[4], *sp;

      sp = schema;
      if (fileinfo[i].addedfileprovides || fileinfo[i].rpmdbcookie)
	{
	  /* extra info about this file */
	  *sp++ = cbdata.keymap[REPODATA_INFO];
	  if (fileinfo[i].addedfileprovides)
	    {
	      *sp++ = cbdata.keymap[REPODATA_ADDEDFILEPROVIDES];
	      for (j = 0; fileinfo[i].addedfileprovides[j]; j++)
	        ;
	      cbdata.mykeys[cbdata.keymap[REPODATA_ADDEDFILEPROVIDES]].size += j + 1;
	    }
	  if (fileinfo[i].rpmdbcookie)
	    *sp++ = cbdata.keymap[REPODATA_RPMDBCOOKIE];
	}
      else
	{
	  *sp++ = cbdata.keymap[REPODATA_EXTERNAL];
	  *sp++ = cbdata.keymap[REPODATA_KEYS];
	  if (fileinfo[i].location)
	    *sp++ = cbdata.keymap[REPODATA_LOCATION];
	}
      *sp = 0;
      repodataschemata[i] = addschema(&cbdata, schema);
      cbdata.mykeys[cbdata.keymap[REPODATA_KEYS]].size += 2 * fileinfo[i].nkeys + 1;
      for (j = 1; j < fileinfo[i].nkeys; j++)
	{
	  needid[fileinfo[i].keys[j].type].need++;
	  needid[fileinfo[i].keys[j].name].need++;
	}
    }
#endif

/********************************************************************/

  /* remove unused keys, convert ids to local ids and increment their needid */
  keyused = sat_calloc(cbdata.nmykeys, sizeof(Id));
  for (i = 0; i < cbdata.myschemadatalen; i++)
    keyused[cbdata.myschemadata[i]] = 1;
  keyused[0] = 0;
  for (n = i = 1; i < cbdata.nmykeys; i++)
    {
      if (!keyused[i])
	continue;
      keyused[i] = n;
      if (i != n)
	cbdata.mykeys[n] = cbdata.mykeys[i];
      if (cbdata.mykeys[n].type == REPOKEY_TYPE_CONSTANTID)
	{
	  if (!type_constantid)
	    type_constantid = poolusage > 1 ? stringpool_str2id(spool, id2str(repo->pool, cbdata.mykeys[n].type), 1) : REPOKEY_TYPE_CONSTANTID;
	  if (poolusage > 1)
	    cbdata.mykeys[n].size = stringpool_str2id(spool, id2str(repo->pool, cbdata.mykeys[n].size), 1);
	  needid[cbdata.mykeys[n].size].need++;
	}
      if (poolusage > 1)
	{
	  cbdata.mykeys[n].name = stringpool_str2id(spool, id2str(repo->pool, cbdata.mykeys[n].name), 1);
	  cbdata.mykeys[n].type = stringpool_str2id(spool, id2str(repo->pool, cbdata.mykeys[n].type), 1);
	}
      needid[cbdata.mykeys[n].name].need++;
      needid[cbdata.mykeys[n].type].need++;
      n++;
    }
  cbdata.nmykeys = n;
  for (i = 0; i < cbdata.myschemadatalen; i++)
    cbdata.myschemadata[i] = keyused[cbdata.myschemadata[i]];
  for (i = 0; i < cbdata.nkeymap; i++)
    cbdata.keymap[i] = keyused[cbdata.keymap[i]];
  keyused = sat_free(keyused);

/********************************************************************/

  /* increment need id for used dir components */
  if (cbdata.dirused && !cbdata.dirused[0])
    {
      /* no dirs used at all */
      cbdata.dirused = sat_free(cbdata.dirused);
      dirpool = 0;
    }
  if (dirpool)
    {
      for (i = 1; i < dirpool->ndirs; i++)
	{
#if 0
fprintf(stderr, "dir %d used %d\n", i, cbdata.dirused ? cbdata.dirused[i] : 1);
#endif
	  id = dirpool->dirs[i];
	  if (id <= 0)
	    continue;
	  if (cbdata.dirused && !cbdata.dirused[i])
	    continue;
	  if (cbdata.ownspool && dirpooldata && id > 1)
	    {
	      id = putinownpool(&cbdata, dirpooldata->localpool ? &dirpooldata->spool : &pool->ss, id);
	      needid = cbdata.needid;
	    }
	  needid[id].need++;
	}
    }

  reloff = needid[0].map;


/********************************************************************/

  /*
   * create mapping table, new keys are sorted by needid[].need
   *
   * needid[key].need : old key -> new key
   * needid[key].map  : new key -> old key
   */

  /* zero out id 0 and rel 0 just in case */

  needid[0].need = 0;
  needid[reloff].need = 0;

  for (i = 1; i < reloff + pool->nrels; i++)
    needid[i].map = i;

#if 0
  sat_sort(needid + 1, spool->nstrings - 1, sizeof(*needid), needid_cmp_need_s, spool);
#else
  /* make first entry '' */
  needid[1].need = 1;
  sat_sort(needid + 2, spool->nstrings - 2, sizeof(*needid), needid_cmp_need_s, spool);
#endif
  sat_sort(needid + reloff, pool->nrels, sizeof(*needid), needid_cmp_need, 0);

  sizeid = 0;
  for (i = 1; i < reloff; i++)
    {
      if (!needid[i].need)
        break;
      needid[i].need = 0;
      sizeid += strlen(spool->stringspace + spool->strings[needid[i].map]) + 1;
    }

  nstrings = i;
  for (i = 1; i < nstrings; i++)
    needid[needid[i].map].need = i;

  for (i = 0; i < pool->nrels; i++)
    {
      if (!needid[reloff + i].need)
        break;
      else
        needid[reloff + i].need = 0;
    }

  nrels = i;
  for (i = 0; i < nrels; i++)
    needid[needid[reloff + i].map].need = nstrings + i;


/********************************************************************/

  /* create dir map */
  ndirmap = 0;
  dirmap = 0;
  if (dirpool)
    {
      if (cbdata.dirused && !cbdata.dirused[1])
	cbdata.dirused[1] = 1;	/* always want / entry */
      dirmap = sat_calloc(dirpool->ndirs, sizeof(Id));
      dirmap[0] = 0;
      ndirmap = traverse_dirs(dirpool, dirmap, 1, dirpool_child(dirpool, 0), cbdata.dirused);
      if (!cbdata.dirused)
	cbdata.dirused = sat_malloc2(dirpool->ndirs, sizeof(Id));
      memset(cbdata.dirused, 0, dirpool->ndirs * sizeof(Id));
      for (i = 1; i < ndirmap; i++)
	{
	  if (dirmap[i] <= 0)
	    continue;
	  cbdata.dirused[dirmap[i]] = i;
	  id = dirpool->dirs[dirmap[i]];
	  if (cbdata.ownspool && dirpooldata && id > 1)
	    id = putinownpool(&cbdata, dirpooldata->localpool ? &dirpooldata->spool : &pool->ss, id);
	  dirmap[i] = needid[id].need;
	}
    }

/********************************************************************/
  cbdata.extdata = sat_calloc(cbdata.nmykeys, sizeof(struct extdata));

  xd = cbdata.extdata;
  cbdata.current_sub = 0;
  /* write main schema */
  cbdata.lastlen = 0;
  data_addid(xd, mainschema);

#if 1
  for (j = 0, data = repo->repodata; j < repo->nrepodata; j++, data++)
    {
      if (!repodataused[j])
	continue;
      repodata_search(data, SOLVID_META, 0, SEARCH_SUB|SEARCH_ARRAYSENTINEL, repo_write_cb_adddata, &cbdata);
    }
#endif

  if (xd->len - cbdata.lastlen > cbdata.maxdata)
    cbdata.maxdata = xd->len - cbdata.lastlen;
  cbdata.lastlen = xd->len;

  if (repo->nsolvables)
    data_addid(xd, repo->nsolvables);	/* FLEXARRAY nentries */
  cbdata.doingsolvables = 1;
  for (i = repo->start, s = pool->solvables + i, n = 0; i < repo->end; i++, s++)
    {
      if (s->repo != repo)
	continue;
      data_addid(xd, cbdata.solvschemata[n]);
      if (cbdata.keymap[SOLVABLE_NAME])
        data_addid(xd, needid[s->name].need);
      if (cbdata.keymap[SOLVABLE_ARCH])
        data_addid(xd, needid[s->arch].need);
      if (cbdata.keymap[SOLVABLE_EVR])
        data_addid(xd, needid[s->evr].need);
      if (s->vendor && cbdata.keymap[SOLVABLE_VENDOR])
        data_addid(xd, needid[s->vendor].need);
      if (s->provides && cbdata.keymap[SOLVABLE_PROVIDES])
        data_addidarray_sort(xd, pool, needid, idarraydata + s->provides, SOLVABLE_FILEMARKER);
      if (s->obsoletes && cbdata.keymap[SOLVABLE_OBSOLETES])
        data_addidarray_sort(xd, pool, needid, idarraydata + s->obsoletes, 0);
      if (s->conflicts && cbdata.keymap[SOLVABLE_CONFLICTS])
        data_addidarray_sort(xd, pool, needid, idarraydata + s->conflicts, 0);
      if (s->requires && cbdata.keymap[SOLVABLE_REQUIRES])
        data_addidarray_sort(xd, pool, needid, idarraydata + s->requires, SOLVABLE_PREREQMARKER);
      if (s->recommends && cbdata.keymap[SOLVABLE_RECOMMENDS])
        data_addidarray_sort(xd, pool, needid, idarraydata + s->recommends, 0);
      if (s->suggests && cbdata.keymap[SOLVABLE_SUGGESTS])
        data_addidarray_sort(xd, pool, needid, idarraydata + s->suggests, 0);
      if (s->supplements && cbdata.keymap[SOLVABLE_SUPPLEMENTS])
        data_addidarray_sort(xd, pool, needid, idarraydata + s->supplements, 0);
      if (s->enhances && cbdata.keymap[SOLVABLE_ENHANCES])
        data_addidarray_sort(xd, pool, needid, idarraydata + s->enhances, 0);
      if (repo->rpmdbid && cbdata.keymap[RPM_RPMDBID])
        data_addu32(xd, repo->rpmdbid[i - repo->start]);
      if (anyrepodataused)
	{
	  cbdata.vstart = -1;
	  for (j = 0, data = repo->repodata; j < repo->nrepodata; j++, data++)
	    {
	      if (!repodataused[j])
		continue;
	      if (i < data->start || i >= data->end)
		continue;
	      repodata_search(data, i, 0, SEARCH_SUB|SEARCH_ARRAYSENTINEL, repo_write_cb_adddata, &cbdata);
	    }
	}
      if (xd->len - cbdata.lastlen > cbdata.maxdata)
	cbdata.maxdata = xd->len - cbdata.lastlen;
      cbdata.lastlen = xd->len;
      n++;
    }
  cbdata.doingsolvables = 0;

  assert(cbdata.current_sub == cbdata.nsubschemata);
  if (cbdata.subschemata)
    {
      cbdata.subschemata = sat_free(cbdata.subschemata);
      cbdata.nsubschemata = 0;
    }

#if 0
  if (repo->nextra && anyrepodataused)
    for (i = -1; i >= -repo->nextra; i--)
      {
	Dataiterator di;
	dataiterator_init(&di, repo, i, 0, 0, SEARCH_EXTRA | SEARCH_NO_STORAGE_SOLVABLE);
	entrysize = xd->len;
	data_addid(xd, cbdata.extraschemata[-1 - i]);
	cbdata.vstart = -1;
	while (dataiterator_step(&di))
	  repo_write_adddata(&cbdata, di.data, di.key, &di.kv);
	entrysize = xd->len - entrysize;
	if (entrysize > maxentrysize)
	  maxentrysize = entrysize;
      }
#endif

/********************************************************************/

  /* write header */

  /* write file header */
  write_u32(fp, 'S' << 24 | 'O' << 16 | 'L' << 8 | 'V');
  write_u32(fp, SOLV_VERSION_8);


  /* write counts */
  write_u32(fp, nstrings);
  write_u32(fp, nrels);
  write_u32(fp, ndirmap);
  write_u32(fp, repo->nsolvables);
  write_u32(fp, cbdata.nmykeys);
  write_u32(fp, cbdata.nmyschemata);
  solv_flags = 0;
  solv_flags |= SOLV_FLAG_PREFIX_POOL;
  write_u32(fp, solv_flags);

  /*
   * calculate prefix encoding of the strings
   */
  unsigned char *prefixcomp = sat_malloc(nstrings);
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
  write_u32(fp, sizeid);
  /* we save compsum bytes but need 1 extra byte for every string */
  write_u32(fp, sizeid + (nstrings ? nstrings - 1 : 0) - compsum);
  if (sizeid + (nstrings ? nstrings - 1 : 0) != compsum)
    {
      for (i = 1; i < nstrings; i++)
	{
	  char *str = spool->stringspace + spool->strings[needid[i].map];
	  write_u8(fp, prefixcomp[i]);
	  write_str(fp, str + prefixcomp[i]);
	}
    }
  sat_free(prefixcomp);

#if 0
  /* Build the prefix-encoding of the string pool.  We need to know
     the size of that before writing it to the file, so we have to
     build a separate buffer for that.  As it's temporarily possible
     that this actually is an expansion we can't easily reuse the 
     stringspace for this.  The max expansion per string is 1 byte,
     so it will fit into sizeid+nstrings bytes.  */
  char *prefix = sat_malloc(sizeid + nstrings);
  char *pp = prefix;
  char *old_str = "";
  for (i = 1; i < nstrings; i++)
    {
      char *str = spool->stringspace + spool->strings[needid[i].map];
      int same;
      size_t len;
      for (same = 0; same < 255; same++)
	if (!old_str[same] || !str[same] || old_str[same] != str[same])
	  break;
      *pp++ = same;
      len = strlen(str + same) + 1;
      memcpy(pp, str + same, len);
      pp += len;
      old_str = str;
    }

  /*
   * write strings
   */
  write_u32(fp, sizeid);
  write_u32(fp, pp - prefix);
  if (pp != prefix)
    {
      if (fwrite(prefix, pp - prefix, 1, fp) != 1)
	{
	  perror("write error prefix");
	  exit(1);
	}
    }
  sat_free(prefix);
#endif

  /*
   * write RelDeps
   */
  for (i = 0; i < nrels; i++)
    {
      ran = pool->rels + (needid[reloff + i].map - reloff);
      write_id(fp, needid[ISRELDEP(ran->name) ? RELOFF(ran->name) : ran->name].need);
      write_id(fp, needid[ISRELDEP(ran->evr) ? RELOFF(ran->evr) : ran->evr].need);
      write_u8(fp, ran->flags);
    }

  /*
   * write dirs (skip both root and / entry)
   */
  for (i = 2; i < ndirmap; i++)
    {
      if (dirmap[i] > 0)
        write_id(fp, dirmap[i]);
      else
        write_id(fp, nstrings - dirmap[i]);
    }
  sat_free(dirmap);

  /*
   * write keys
   */
  if (keyarrayp)
    *keyarrayp = sat_calloc(2 * cbdata.nmykeys + 1, sizeof(Id));
  for (i = 1; i < cbdata.nmykeys; i++)
    {
      write_id(fp, needid[cbdata.mykeys[i].name].need);
      write_id(fp, needid[cbdata.mykeys[i].type].need);
      if (cbdata.mykeys[i].storage != KEY_STORAGE_VERTICAL_OFFSET)
	{
	  if (cbdata.mykeys[i].type == type_constantid)
            write_id(fp, needid[cbdata.mykeys[i].size].need);
	  else
            write_id(fp, cbdata.mykeys[i].size);
	}
      else
        write_id(fp, cbdata.extdata[i].len);
      write_id(fp, cbdata.mykeys[i].storage);
      if (keyarrayp)
	{
          (*keyarrayp)[2 * i - 2] = cbdata.mykeys[i].name;
          (*keyarrayp)[2 * i - 1] = cbdata.mykeys[i].type;
	}
    }

  /*
   * write schemata
   */
  write_id(fp, cbdata.myschemadatalen);
  if (cbdata.nmyschemata)
    {
      for (i = 1; i < cbdata.nmyschemata; i++)
	write_idarray(fp, pool, 0, cbdata.myschemadata + cbdata.myschemata[i]);
    }

#if 0
  /*
   * write info block
   */
  if (nsubfiles)
    {
      struct extdata xd;
      xd.buf = 0;
      xd.len = 0;
      int max = 0;
      int cur;

      for (i = 0; i < nsubfiles; i++)
	{
	  int j;

	  cur = xd.len;
	  data_addid(&xd, repodataschemata[i]);
	  if (fileinfo[i].addedfileprovides || fileinfo[i].rpmdbcookie)
	    {
	      if (fileinfo[i].addedfileprovides)
	        data_addidarray_sort(&xd, pool, needid, fileinfo[i].addedfileprovides, 0);
	      if (fileinfo[i].rpmdbcookie)
	        data_addblob(&xd, fileinfo[i].rpmdbcookie, 32);
	    }
	  else
	    {
	      /* key,type array + location, write idarray */
	      for (j = 1; j < fileinfo[i].nkeys; j++)
		{
		  data_addideof(&xd, needid[fileinfo[i].keys[j].name].need, 0);
		  data_addideof(&xd, needid[fileinfo[i].keys[j].type].need, j == fileinfo[i].nkeys - 1);
		}
	      if (fileinfo[i].location)
		data_addblob(&xd, (unsigned char *)fileinfo[i].location, strlen(fileinfo[i].location) + 1);
	    }
	  cur = xd.len - cur;
	  if (cur > max)
	    max = cur;
	}
      write_id(fp, max);
      write_id(fp, xd.len);
      write_blob(fp, xd.buf, xd.len);
      sat_free(xd.buf);
    }
#endif

/********************************************************************/

  write_id(fp, cbdata.maxdata);
  write_id(fp, cbdata.extdata[0].len);
  if (cbdata.extdata[0].len)
    write_blob(fp, cbdata.extdata[0].buf, cbdata.extdata[0].len);
  sat_free(cbdata.extdata[0].buf);

#if 0
  /*
   * write Solvable data
   */
  if (repo->nsolvables || repo->nextra)
    {
      write_id(fp, maxentrysize);
      write_id(fp, cbdata.extdata[0].len);
      write_blob(fp, cbdata.extdata[0].buf, cbdata.extdata[0].len);
    }
  sat_free(cbdata.extdata[0].buf);
#endif

  /* write vertical data */
  for (i = 1; i < cbdata.nmykeys; i++)
    if (cbdata.extdata[i].len)
      break;
  if (i < cbdata.nmykeys)
    {
      unsigned char *dp, vpage[BLOB_PAGESIZE];
      int l, ll, lpage = 0;

      write_u32(fp, BLOB_PAGESIZE);
      for (i = 1; i < cbdata.nmykeys; i++)
	{
	  if (!cbdata.extdata[i].len)
	    continue;
	  l = cbdata.extdata[i].len;
	  dp = cbdata.extdata[i].buf;
	  while (l)
	    {
	      ll = BLOB_PAGESIZE - lpage;
	      if (l < ll)
		ll = l;
	      memcpy(vpage + lpage, dp, ll);
	      dp += ll;
	      lpage += ll;
	      l -= ll;
	      if (lpage == BLOB_PAGESIZE)
		{
		  write_compressed_page(fp, vpage, lpage);
		  lpage = 0;
		}
	    }
	}
      if (lpage)
	write_compressed_page(fp, vpage, lpage);
    }

#if 0
  /* write vertical_offset entries */
  write_u32(fp, 0);	/* no paging */
  for (i = 1; i < cbdata.nmykeys; i++)
    if (cbdata.extdata[i].len)
      write_blob(fp, cbdata.extdata[i].buf, cbdata.extdata[i].len);

  /* Fill fileinfo for our caller.  */
  if (setfileinfo)
    {
      fileinfo->checksum = 0;
      fileinfo->nchecksum = 0;
      fileinfo->checksumtype = 0;
      fileinfo->location = 0;
    }
#endif

  for (i = 1; i < cbdata.nmykeys; i++)
    sat_free(cbdata.extdata[i].buf);
  sat_free(cbdata.extdata);

  if (cbdata.ownspool)
    {
      sat_free(cbdata.ownspool->strings);
      sat_free(cbdata.ownspool->stringspace);
      sat_free(cbdata.ownspool->stringhashtbl);
    }
  if (cbdata.owndirpool)
    {
      sat_free(cbdata.owndirpool->dirs);
      sat_free(cbdata.owndirpool->dirtraverse);
    }
  sat_free(needid);
  sat_free(cbdata.extraschemata);
  sat_free(cbdata.solvschemata);
  sat_free(cbdata.myschemadata);
  sat_free(cbdata.myschemata);
  sat_free(cbdata.schema);

  sat_free(cbdata.mykeys);
  sat_free(cbdata.keymap);
  sat_free(cbdata.keymapstart);
  sat_free(cbdata.dirused);
  sat_free(repodataused);
  sat_free(repodataschemata);
}

struct repodata_write_data {
  int (*keyfilter)(Repo *repo, Repokey *key, void *kfdata);
  void *kfdata;
  int repodataid;
};

static int
repodata_write_keyfilter(Repo *repo, Repokey *key, void *kfdata)
{
  struct repodata_write_data *wd = kfdata;

  /* XXX: special repodata selection hack */
  if (key->name == 1 && key->size != wd->repodataid)
    return -1;
  if (key->storage == KEY_STORAGE_SOLVABLE)
    return KEY_STORAGE_DROPPED;	/* not part of this repodata */
  if (wd->keyfilter)
    return (*wd->keyfilter)(repo, key, wd->kfdata);
  return key->storage;
}

void
repodata_write(Repodata *data, FILE *fp, int (*keyfilter)(Repo *repo, Repokey *key, void *kfdata), void *kfdata)
{
  struct repodata_write_data wd;

  wd.keyfilter = keyfilter;
  wd.kfdata = kfdata;
  wd.repodataid = data - data->repo->repodata;
  repo_write(data->repo, fp, repodata_write_keyfilter, &wd, 0);
}
