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
needid_cmp_need(const void *ap, const void *bp)
{
  const NeedId *a = ap;
  const NeedId *b = bp;
  int r;
  r = b->need - a->need;
  if (r)
    return r;
  return a->map - b->map;
}

static Pool *cmp_pool;

static int
needid_cmp_need_s(const void *ap, const void *bp)
{
  const NeedId *a = ap;
  const NeedId *b = bp;
  int r;
  r = b->need - a->need;
  if (r)
    return r;
  const char *as = cmp_pool->ss.stringspace + cmp_pool->ss.strings[a->map];
  const char *bs = cmp_pool->ss.stringspace + cmp_pool->ss.strings[b->map];
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
  if (fwrite(data, len, 1, fp) != 1)
    {
      perror("write error blob");
      exit(1);
    }
}

static unsigned id_bytes;

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

#if 1
static void
write_str(FILE *fp, const char *str)
{
  if (fputs (str, fp) == EOF || putc (0, fp) == EOF)
    {
      perror("write error");
      exit(1);
    }
}
#endif

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
cmp_ids (const void *pa, const void *pb)
{
  Id a = *(Id *)pa;
  Id b = *(Id *)pb;
  return a - b;
}

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
    qsort(sids, i, sizeof (Id), cmp_ids);
  if ((len - i) > 2)
    qsort(sids + i + 1, len - i - 1, sizeof(Id), cmp_ids);

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

static inline void
write_id_value(FILE *fp, Id id, Id value, int eof)
{
  if (id >= 64)
    id = (id & 63) | ((id & ~63) << 1);
  write_id(fp, id | 64);
  if (value >= 64)
    value = (value & 63) | ((value & ~63) << 1);
  write_id(fp, value | (eof ? 0 : 64));
}


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

  Id *myschemata;
  int nmyschemata;

  Id *myschemadata;
  int myschemadatalen;

  Id schematacache[256];

  Id *solvschemata;
  Id *incorelen;

  struct extdata *extdata;

  Id *dirused;

  Id vstart;
};

#define NEEDED_BLOCK 1023
#define SCHEMATA_BLOCK 31
#define SCHEMATADATA_BLOCK 255
#define EXTDATA_BLOCK 1023

static void
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

static void
data_addideof(struct extdata *xd, Id x, int eof)
{
  if (x >= 64)
    x = (x & 63) | ((x & ~63) << 1);
  data_addid(xd, (eof ? x: x | 64));
}

static void
data_addblob(struct extdata *xd, unsigned char *blob, int len)
{
  xd->buf = sat_extend(xd->buf, xd->len, len, 1, EXTDATA_BLOCK);
  memcpy(xd->buf + xd->len, blob, len);
  xd->len += len;
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
  cbdata->myschemadata = sat_extend(cbdata->myschemadata, cbdata->myschemadatalen, len, sizeof(Id), SCHEMATADATA_BLOCK);
  cbdata->myschemata = sat_extend(cbdata->myschemata, cbdata->nmyschemata, 1, sizeof(Id), SCHEMATA_BLOCK);
  if (!cbdata->nmyschemata)
    {
      cbdata->myschemata[0] = 0;
      cbdata->myschemadata[0] = 0;
      cbdata->nmyschemata = 1;
      cbdata->myschemadatalen = 1;
    }
  /* add schema */
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
      fprintf(stderr, "growing needid...\n");
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
repo_write_cb_needed(void *vcbdata, Solvable *s, Repodata *data, Repokey *key, KeyValue *kv)
{
  struct cbdata *cbdata = vcbdata;
  Repo *repo = s ? s->repo : 0;
  Id id;
  int rm;

#if 0
  fprintf(stderr, "solvable %d (%s): key (%d)%s %d\n", s ? s - s->repo->pool->solvables : 0, s ? id2str(s->repo->pool, s->name) : "", key->name, id2str(repo->pool, key->name), key->type);
#endif
  rm = cbdata->keymap[cbdata->keymapstart[data - data->repo->repodata] + (key - data->keys)];
  if (!rm)
    return SEARCH_NEXT_KEY;	/* we do not want this one */
  if (cbdata->sp == cbdata->schema || cbdata->sp[-1] != rm)
    *cbdata->sp++ = rm;
  switch(key->type)
    {
      case TYPE_ID:
      case TYPE_IDARRAY:
	id = kv->id;
	if (!ISRELDEP(id) && cbdata->ownspool && id > 1)
	  id = putinownpool(cbdata, data->localpool ? &data->spool : &repo->pool->ss, id);
	incneedid(repo->pool, id, cbdata->needid);
	break;
      case TYPE_DIR:
      case TYPE_DIRNUMNUMARRAY:
      case TYPE_DIRSTRARRAY:
	id = kv->id;
	if (cbdata->owndirpool)
	  putinowndirpool(cbdata, data, &data->dirpool, id);
	else
	  setdirused(cbdata, &data->dirpool, id);
	break;
      default:
	break;
    }
  return 0;
}

static int
repo_write_cb_sizes(void *vcbdata, Solvable *s, Repodata *data, Repokey *key, KeyValue *kv)
{
  struct cbdata *cbdata = vcbdata;
  int rm;
  Id id;
  unsigned int u32;
  unsigned char v[4];
  struct extdata *xd;

  rm = cbdata->keymap[cbdata->keymapstart[data - data->repo->repodata] + (key - data->keys)];
  if (!rm)
    return 0;	/* we do not want this one */
  
  if (cbdata->mykeys[rm].storage == KEY_STORAGE_VERTICAL_OFFSET)
    {
      xd = cbdata->extdata + rm;	/* vertical buffer */
      if (!cbdata->vstart)
        cbdata->vstart = xd->len;
    }
  else
    xd = cbdata->extdata + 0;		/* incore buffer */
  switch(key->type)
    {
      case TYPE_VOID:
      case TYPE_CONSTANT:
	break;
      case TYPE_ID:
	id = kv->id;
	if (!ISRELDEP(id) && cbdata->ownspool && id > 1)
	  id = putinownpool(cbdata, data->localpool ? &data->spool : &data->repo->pool->ss, id);
	id = cbdata->needid[id].need;
	data_addid(xd, id);
	break;
      case TYPE_IDARRAY:
	id = kv->id;
	if (cbdata->ownspool && id > 1)
	  id = putinownpool(cbdata, data->localpool ? &data->spool : &data->repo->pool->ss, id);
	id = cbdata->needid[id].need;
	data_addideof(xd, id, kv->eof);
	break;
      case TYPE_STR:
	data_addblob(xd, (unsigned char *)kv->str, strlen(kv->str) + 1);
	break;
      case TYPE_U32:
	u32 = kv->num;
	v[0] = u32 >> 24;
	v[1] = u32 >> 16;
	v[2] = u32 >> 8;
	v[3] = u32;
	data_addblob(xd, v, 4);
	break;
      case TYPE_DIR:
	id = kv->id;
	id = cbdata->dirused[id];
	data_addid(xd, id);
	break;
      case TYPE_NUM:
	data_addid(xd, kv->num);
	break;
      case TYPE_DIRNUMNUMARRAY:
	id = kv->id;
	id = cbdata->dirused[id];
	data_addid(xd, id);
	data_addid(xd, kv->num);
	data_addideof(xd, kv->num2, kv->eof);
	break;
      case TYPE_DIRSTRARRAY:
	id = kv->id;
	id = cbdata->dirused[id];
	data_addideof(xd, id, kv->eof);
	data_addblob(xd, (unsigned char *)kv->str, strlen(kv->str) + 1);
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
      cbdata->vstart = 0;
    }
  return 0;
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
assert (sib < dp->ndirs);
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

#define BLOB_PAGEBITS 15
#define BLOB_PAGESIZE (1 << BLOB_PAGEBITS)

static void
write_compressed_page(FILE *fp, unsigned char *page, int len)
{
  int clen;
  unsigned char cpage[BLOB_PAGESIZE];

  clen = repodata_compress_page(page, len, cpage, len - 1);
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

/*
 * Repo
 */

void
repo_write(Repo *repo, FILE *fp, int (*keyfilter)(Repo *repo, Repokey *key, void *kfdata), void *kfdata, Repodatafile *fileinfo, int nsubfiles)
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
  int ext0len;
  unsigned char *repodataused;
  
  struct cbdata cbdata;
  int needrels;
  Repokey *key;
  int poolusage, dirpoolusage, idused, dirused;
  int reloff;
  unsigned char *incoredata;

  Repodata *data;
  Stringpool ownspool, *spool;
  Dirpool owndirpool, *dirpool;

  int setfileinfo = 0;
  Id repodataschema = 0;
  Id repodataschema_internal = 0;

  /* If we're given a fileinfo structure, but have no subfiles, then we're
     writing a subfile and our callers wants info about it.  */
  if (fileinfo && nsubfiles == 0)
    setfileinfo = 1;

  memset(&cbdata, 0, sizeof(cbdata));

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
        key->type = TYPE_ID;
      else if (i < RPM_RPMDBID)
        key->type = TYPE_REL_IDARRAY;
      else
        key->type = TYPE_U32;
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
      if (key->type == TYPE_IDARRAY || key->type == TYPE_REL_IDARRAY)
	needrels = 1;
      cbdata.keymap[i] = i;
    }
  cbdata.nmykeys = i;

  /* If we store subfile info, generate the three necessary keys.  */
  if (nsubfiles)
    {
      key = cbdata.mykeys + cbdata.nmykeys;
      key->name = REPODATA_EXTERNAL;
      key->type = TYPE_VOID;
      key->size = 0;
      key->storage = KEY_STORAGE_SOLVABLE;
      cbdata.keymap[key->name] = key - cbdata.mykeys;
      key++;

      key->name = REPODATA_KEYS;
      key->type = TYPE_IDVALUEARRAY;
      key->size = 0;
      key->storage = KEY_STORAGE_SOLVABLE;
      cbdata.keymap[key->name] = key - cbdata.mykeys;
      key++;

      key->name = REPODATA_LOCATION;
      key->type = TYPE_STR;
      key->size = 0;
      key->storage = KEY_STORAGE_SOLVABLE;
      cbdata.keymap[key->name] = key - cbdata.mykeys;
      key++;

      cbdata.nmykeys = key - cbdata.mykeys;
    }

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
      for (j = 1; j < data->nkeys; j++)
	{
	  key = data->keys + j;
	  /* see if we already had this one, should use hash for fast miss */
	  for (k = 0; k < cbdata.nmykeys; k++)
	    {
	      if (key->name == cbdata.mykeys[k].name && key->type == cbdata.mykeys[k].type)
		{
		  if (key->type == TYPE_CONSTANT && key->size != cbdata.mykeys[k].size)
		    continue;
		  break;
		}
	    }
	  if (k < cbdata.nmykeys)
	    {
	      repodataused[i] = 1;
	      cbdata.keymap[n++] = k;
	      continue;
	    }
	  cbdata.mykeys[cbdata.nmykeys] = *key;
	  key = cbdata.mykeys + cbdata.nmykeys;
	  key->storage = KEY_STORAGE_INCORE;
	  if (key->type != TYPE_CONSTANT)
	    key->size = 0;
	  if (keyfilter)
	    {
	      key->storage = keyfilter(repo, key, kfdata);
	      if (key->storage == KEY_STORAGE_DROPPED)
		{
		  cbdata.keymap[n++] = 0;
		  continue;
		}
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
		  n = cbdata.keymapstart[i] + 1;
		  continue;
		}
	    }
	  if (data->state == REPODATA_ERROR)
	    {
	      /* too bad! */
	      cbdata.keymap[n++] = 0;
	      continue;
	    }
	  cbdata.keymap[n++] = cbdata.nmykeys++;
	  repodataused[i] = 1;
	  if (key->type != TYPE_STR && key->type != TYPE_U32)
	    idused = 1;
	  if (key->type == TYPE_DIR || key->type == TYPE_DIRNUMNUMARRAY || key->type == TYPE_DIRSTRARRAY)
	    dirused = 1;
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
      dirpool_create(dirpool);
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
  fprintf(stderr, "  %2d: %d %d\n", i, cbdata.mykeys[i].name, cbdata.mykeys[i].type);
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

  idarraydata = repo->idarraydata;

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
      if (s->freshens && cbdata.keymap[SOLVABLE_FRESHENS])
	{
          *sp++ = SOLVABLE_FRESHENS;
	  cbdata.mykeys[SOLVABLE_FRESHENS].size += incneedidarray(pool, idarraydata + s->freshens, needid);
	}
      if (repo->rpmdbid && cbdata.keymap[RPM_RPMDBID])
	{
          *sp++ = RPM_RPMDBID;
	  cbdata.mykeys[RPM_RPMDBID].size++;
	}
      cbdata.sp = sp;

      for (j = 0, data = repo->repodata; j < repo->nrepodata; j++, data++)
	{
	  if (!repodataused[j])
	    continue;
	  if (i < data->start || i >= data->end)
	    continue;
	  repodata_search(data, i - data->start, 0, repo_write_cb_needed, &cbdata);
	  needid = cbdata.needid;
	}
      *cbdata.sp = 0;
      cbdata.solvschemata[n] = addschema(&cbdata, cbdata.schema);
      n++;
    }

  reloff = needid[0].map;

  /* If we have fileinfos to write, setup schemas and increment needid[]
     of the right strings.  */
  for (i = 0; i < nsubfiles; i++)
    {
      int j;

      if (fileinfo[i].location && !repodataschema)
        {
	  Id schema[4];
	  schema[0] = cbdata.keymap[REPODATA_EXTERNAL];
	  schema[1] = cbdata.keymap[REPODATA_KEYS];
	  schema[2] = cbdata.keymap[REPODATA_LOCATION];
	  schema[3] = 0;
	  repodataschema = addschema(&cbdata, schema);
	}
      else if (!repodataschema_internal)
        {
	  Id schema[3];
	  schema[0] = cbdata.keymap[REPODATA_EXTERNAL];
	  schema[1] = cbdata.keymap[REPODATA_KEYS];
	  schema[2] = 0;
	  repodataschema_internal = addschema(&cbdata, schema);
	}
      if (2 * fileinfo[i].nkeys > cbdata.mykeys[cbdata.keymap[REPODATA_KEYS]].size)
	cbdata.mykeys[cbdata.keymap[REPODATA_KEYS]].size = 2 * fileinfo[i].nkeys;
      for (j = 1; j < fileinfo[i].nkeys; j++)
	needid[fileinfo[i].keys[j].name].need++;
#if 0
      fprintf (stderr, " %d nkeys: %d:", i, fileinfo[i].nkeys);
      for (j = 1; j < fileinfo[i].nkeys; j++)
        {
	  needid[fileinfo[i].keys[j].name].need++;
	  fprintf (stderr, " %s(%d,%d)", id2str(pool, fileinfo[i].keys[j].name),
	           fileinfo[i].keys[j].name, fileinfo[i].keys[j].type);
	}
      fprintf (stderr, "\n");
#endif
    }
  if (nsubfiles)
    cbdata.mykeys[cbdata.keymap[REPODATA_KEYS]].size -= 2;

/********************************************************************/

  /* remove unused keys, also increment needid for key names */
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
      needid[cbdata.mykeys[n].name].need++;
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
if (cbdata.dirused)
  fprintf(stderr, "dir %d used %d\n", i, cbdata.dirused[i]);
#endif
	  id = dirpool->dirs[i];
	  if (id <= 0)
	    continue;
	  if (cbdata.dirused && !cbdata.dirused[i])
	    continue;
	  needid[id].need++;
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

  needid[0].need = 0;
  needid[reloff].need = 0;

  for (i = 1; i < reloff + pool->nrels; i++)
    needid[i].map = i;

  cmp_pool = pool;
  qsort(needid + 1, reloff - 1, sizeof(*needid), needid_cmp_need_s);
  qsort(needid + reloff, pool->nrels, sizeof(*needid), needid_cmp_need);

  sizeid = 0;
  for (i = 1; i < reloff; i++)
    {
      if (!needid[i].need)
        break;
      needid[i].need = 0;
      sizeid += strlen(pool->ss.stringspace + pool->ss.strings[needid[i].map]) + 1;
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
	  dirmap[i] = needid[dirpool->dirs[dirmap[i]]].need;
	}
    }

/********************************************************************/
  cbdata.extdata = sat_calloc(cbdata.nmykeys, sizeof(struct extdata));
  cbdata.incorelen = sat_calloc(repo->nsolvables, sizeof(Id));
  /* calculate incore/vertical data and sizes */
  ext0len = 0;

  for (i = 1; i < cbdata.nmykeys; i++)
    if (cbdata.mykeys[i].storage != KEY_STORAGE_SOLVABLE)
      break;
  if (i < cbdata.nmykeys)
    {
      /* we need incore/vertical data */
      for (i = repo->start, s = pool->solvables + i, n = 0; i < repo->end; i++, s++)
	{
	  if (s->repo != repo)
	    continue;
	  for (j = 0, data = repo->repodata; j < repo->nrepodata; j++, data++)
	    {
	      if (!repodataused[j])
		continue;
	      if (i < data->start || i >= data->end)
		continue;
	      repodata_search(data, i - data->start, 0, repo_write_cb_sizes, &cbdata);
	    }
	  cbdata.incorelen[n] = cbdata.extdata[0].len - ext0len;
	  ext0len = cbdata.extdata[0].len;
	  n++;
	}
    }

/********************************************************************/

  /* write header */

  /* write file header */
  write_u32(fp, 'S' << 24 | 'O' << 16 | 'L' << 8 | 'V');
  write_u32(fp, SOLV_VERSION_5);

  /* write counts */
  write_u32(fp, nstrings);
  write_u32(fp, nrels);
  write_u32(fp, ndirmap);
  write_u32(fp, repo->nsolvables);
  write_u32(fp, cbdata.nmykeys);
  write_u32(fp, cbdata.nmyschemata);
  write_u32(fp, nsubfiles);	/* info blocks.  */
  solv_flags = 0;
  solv_flags |= SOLV_FLAG_PREFIX_POOL;
#if 0
  solv_flags |= SOLV_FLAG_VERTICAL;
#endif
  write_u32(fp, solv_flags);

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
      len = strlen (str + same) + 1;
      memcpy (pp, str + same, len);
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

  /*
   * write RelDeps
   */
  for (i = 0; i < nrels; i++)
    {
      ran = pool->rels + (needid[reloff + i].map - pool->ss.nstrings);
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
  if (setfileinfo)
    {
      fileinfo->nkeys = cbdata.nmykeys;
      fileinfo->keys = sat_calloc(fileinfo->nkeys, sizeof (*fileinfo->keys));
    }
  for (i = 1; i < cbdata.nmykeys; i++)
    {
      write_id(fp, needid[cbdata.mykeys[i].name].need);
      write_id(fp, cbdata.mykeys[i].type);
      if (cbdata.mykeys[i].storage != KEY_STORAGE_VERTICAL_OFFSET)
        write_id(fp, cbdata.mykeys[i].size);
      else
        write_id(fp, cbdata.extdata[i].len);
      write_id(fp, cbdata.mykeys[i].storage);
      if (setfileinfo)
        fileinfo->keys[i] = cbdata.mykeys[i];
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

  /*
   * write info block
   */
  for (i = 0; i < nsubfiles; i++)
    {
      int j;

      if (fileinfo[i].location)
        write_id(fp, repodataschema);
      else
        write_id(fp, repodataschema_internal);
      /* keys + location, write idarray */
      for (j = 1; j < fileinfo[i].nkeys; j++)
        {
	  	   
	  Id id = needid[fileinfo[i].keys[j].name].need;
#if 0
	  fprintf (stderr, "writing %d(%s) %d\n", id,
	           id2str(pool, needid[id].map),
		   fileinfo[i].keys[j].type);
#endif
	  write_id_value(fp, id, fileinfo[i].keys[j].type, j == fileinfo[i].nkeys - 1);
        }
      if (fileinfo[i].location)
        write_str(fp, fileinfo[i].location);
    }


/********************************************************************/


  /*
   * write Solvables
   */
  incoredata = cbdata.extdata[0].buf;
  for (i = repo->start, s = pool->solvables + i, n = 0; i < repo->end; i++, s++)
    {
      if (s->repo != repo)
	continue;
      id_bytes = 0;
      /* keep in sync with schema generation! */
      write_id(fp, cbdata.solvschemata[n]);
#if 0
{
  Id *sp;
  fprintf(stderr, "write solvable %d (%s): \n", n, id2str(pool, s->name));
  sp = cbdata.myschemadata + cbdata.myschemata[cbdata.solvschemata[n]];
  for (; *sp; sp++)
    fprintf(stderr, " (%d,%d)", cbdata.mykeys[*sp].name, cbdata.mykeys[*sp].type);
  fprintf(stderr, "\n");
}
#endif
      if (cbdata.keymap[SOLVABLE_NAME])
        write_id(fp, needid[s->name].need);
      if (cbdata.keymap[SOLVABLE_ARCH])
        write_id(fp, needid[s->arch].need);
      if (cbdata.keymap[SOLVABLE_EVR])
        write_id(fp, needid[s->evr].need);
      if (s->vendor && cbdata.keymap[SOLVABLE_VENDOR])
        write_id(fp, needid[s->vendor].need);
      if (s->provides && cbdata.keymap[SOLVABLE_PROVIDES])
        write_idarray_sort(fp, pool, needid, idarraydata + s->provides, SOLVABLE_FILEMARKER);
      if (s->obsoletes && cbdata.keymap[SOLVABLE_OBSOLETES])
        write_idarray_sort(fp, pool, needid, idarraydata + s->obsoletes, 0);
      if (s->conflicts && cbdata.keymap[SOLVABLE_CONFLICTS])
        write_idarray_sort(fp, pool, needid, idarraydata + s->conflicts, 0);
      if (s->requires && cbdata.keymap[SOLVABLE_REQUIRES])
        write_idarray_sort(fp, pool, needid, idarraydata + s->requires, SOLVABLE_PREREQMARKER);
      if (s->recommends && cbdata.keymap[SOLVABLE_RECOMMENDS])
        write_idarray_sort(fp, pool, needid, idarraydata + s->recommends, 0);
      if (s->suggests && cbdata.keymap[SOLVABLE_SUGGESTS])
        write_idarray_sort(fp, pool, needid, idarraydata + s->suggests, 0);
      if (s->supplements && cbdata.keymap[SOLVABLE_SUPPLEMENTS])
        write_idarray_sort(fp, pool, needid, idarraydata + s->supplements, 0);
      if (s->enhances && cbdata.keymap[SOLVABLE_ENHANCES])
        write_idarray_sort(fp, pool, needid, idarraydata + s->enhances, 0);
      if (s->freshens && cbdata.keymap[SOLVABLE_FRESHENS])
        write_idarray_sort(fp, pool, needid, idarraydata + s->freshens, 0);
      if (repo->rpmdbid && cbdata.keymap[RPM_RPMDBID])
        write_u32(fp, repo->rpmdbid[i - repo->start]),id_bytes+=4;
      if (cbdata.incorelen[n])
	{
	  write_blob(fp, incoredata, cbdata.incorelen[n]);
	  incoredata += cbdata.incorelen[n];
	  id_bytes += cbdata.incorelen[n];
	}
      n++;
    }
  sat_free(cbdata.extdata[0].buf);

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
#endif

  /* Fill fileinfo for our caller.  */
  if (setfileinfo)
    {
      fileinfo->checksum = 0;
      fileinfo->nchecksum = 0;
      fileinfo->checksumtype = 0;
      fileinfo->location = 0;
    }

  for (i = 1; i < cbdata.nmykeys; i++)
    sat_free(cbdata.extdata[i].buf);
  sat_free(cbdata.extdata);

  sat_free(needid);
  sat_free(cbdata.solvschemata);
  sat_free(cbdata.myschemadata);
  sat_free(cbdata.myschemata);
  sat_free(cbdata.schema);

  sat_free(cbdata.mykeys);
  sat_free(cbdata.keymap);
  sat_free(cbdata.keymapstart);
  sat_free(cbdata.dirused);
  sat_free(cbdata.incorelen);
  sat_free(repodataused);
}
