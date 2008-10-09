/*
 * Copyright (c) 2007, Novell Inc.
 *
 * This program is licensed under the BSD license, read LICENSE.BSD
 * for further information
 */

/*
 * repodata.c
 *
 * Manage data coming from one repository
 * 
 */

#define _GNU_SOURCE
#include <string.h>
#include <fnmatch.h>

#include <stdio.h>
#include <stdlib.h>
#include <unistd.h>
#include <assert.h>

#include "repo.h"
#include "pool.h"
#include "poolid_private.h"
#include "util.h"

#include "repopack.h"
#include "repopage.h"

extern unsigned int compress_buf (const unsigned char *in, unsigned int in_len,
				  unsigned char *out, unsigned int out_len);
extern unsigned int unchecked_decompress_buf (const unsigned char *in,
					      unsigned int in_len,
					      unsigned char *out,
					      unsigned int out_len);

#define REPODATA_BLOCK 255


void
repodata_init(Repodata *data, Repo *repo, int localpool)
{
  memset(data, 0, sizeof (*data));
  data->repo = repo;
  data->localpool = localpool;
  if (localpool)
    stringpool_init_empty(&data->spool);
  data->keys = sat_calloc(1, sizeof(Repokey));
  data->nkeys = 1;
  data->schemata = sat_calloc(1, sizeof(Id));
  data->schemadata = sat_calloc(1, sizeof(Id));
  data->nschemata = 1;
  data->schemadatalen = 1;
  data->pagefd = -1;
}

void
repodata_free(Repodata *data)
{
  int i;

  sat_free(data->keys);

  sat_free(data->schemata);
  sat_free(data->schemadata);
  sat_free(data->schematahash);

  stringpool_free(&data->spool);
  dirpool_free(&data->dirpool);

  sat_free(data->mainschemaoffsets);
  sat_free(data->incoredata);
  sat_free(data->incoreoffset);
  sat_free(data->verticaloffset);

  sat_free(data->blob_store);
  sat_free(data->pages);
  sat_free(data->mapped);

  sat_free(data->vincore);

  if (data->attrs)
    for (i = 0; i < data->end - data->start; i++)
      sat_free(data->attrs[i]);
  sat_free(data->attrs);
  if (data->xattrs)
    for (i = 0; i < data->nxattrs; i++)
      sat_free(data->xattrs[i]);
  sat_free(data->xattrs);

  sat_free(data->attrdata);
  sat_free(data->attriddata);
  
  if (data->pagefd != -1)
    close(data->pagefd);
}


/***************************************************************
 * key pool management
 */

/* this is not so time critical that we need a hash, so we do a simple
 * linear search */
Id
repodata_key2id(Repodata *data, Repokey *key, int create)
{
  Id keyid;

  for (keyid = 1; keyid < data->nkeys; keyid++)
    if (data->keys[keyid].name == key->name && data->keys[keyid].type == key->type)
      {    
        if ((key->type == REPOKEY_TYPE_CONSTANT || key->type == REPOKEY_TYPE_CONSTANTID) && key->size != data->keys[keyid].size)
          continue;
        break;
      }
  if (keyid == data->nkeys)
    {    
      if (!create)
	return 0;
      /* allocate new key */
      data->keys = sat_realloc2(data->keys, data->nkeys + 1, sizeof(Repokey));
      data->keys[data->nkeys++] = *key;
      if (data->verticaloffset)
        {
          data->verticaloffset = sat_realloc2(data->verticaloffset, data->nkeys, sizeof(Id));
          data->verticaloffset[data->nkeys - 1] = 0; 
        }
      data->keybits[(key->name >> 3) & (sizeof(data->keybits) - 1)] |= 1 << (key->name & 7);
    }
  return keyid;
}


/***************************************************************
 * schema pool management
 */

#define SCHEMATA_BLOCK 31
#define SCHEMATADATA_BLOCK 255

Id
repodata_schema2id(Repodata *data, Id *schema, int create)
{
  int h, len, i; 
  Id *sp, cid; 
  Id *schematahash;

  if ((schematahash = data->schematahash) == 0)
    {
      data->schematahash = schematahash = sat_calloc(256, sizeof(Id));
      for (i = 0; i < data->nschemata; i++)
	{
	  for (sp = data->schemadata + data->schemata[i], h = 0; *sp; len++)
	    h = h * 7 + *sp++;
	  h &= 255;
	  schematahash[h] = i + 1;
	}
      data->schemadata = sat_extend_resize(data->schemadata, data->schemadatalen, sizeof(Id), SCHEMATADATA_BLOCK); 
      data->schemata = sat_extend_resize(data->schemata, data->nschemata, sizeof(Id), SCHEMATA_BLOCK);
    }

  for (sp = schema, len = 0, h = 0; *sp; len++)
    h = h * 7 + *sp++;
  h &= 255; 
  len++;

  cid = schematahash[h];
  if (cid)
    {    
      cid--;
      if (!memcmp(data->schemadata + data->schemata[cid], schema, len * sizeof(Id)))
        return cid;
      /* cache conflict */
      for (cid = 0; cid < data->nschemata; cid++)
        if (!memcmp(data->schemadata + data->schemata[cid], schema, len * sizeof(Id)))
          return cid;
    }
  /* a new one */
  if (!create)
    return 0;
  data->schemadata = sat_extend(data->schemadata, data->schemadatalen, len, sizeof(Id), SCHEMATADATA_BLOCK); 
  data->schemata = sat_extend(data->schemata, data->nschemata, 1, sizeof(Id), SCHEMATA_BLOCK);
  /* add schema */
  memcpy(data->schemadata + data->schemadatalen, schema, len * sizeof(Id));
  data->schemata[data->nschemata] = data->schemadatalen;
  data->schemadatalen += len;
  schematahash[h] = data->nschemata + 1;
#if 0
fprintf(stderr, "schema2id: new schema\n");
#endif
  return data->nschemata++; 
}

void
repodata_free_schemahash(Repodata *data)
{
  data->schematahash = sat_free(data->schematahash);
  /* shrink arrays */
  data->schemata = sat_realloc2(data->schemata, data->nschemata, sizeof(Id));
  data->schemadata = sat_realloc2(data->schemadata, data->schemadatalen, sizeof(Id));
}


/***************************************************************
 * dir pool management
 */

Id
repodata_str2dir(Repodata *data, const char *dir, int create)
{
  Id id, parent;
  const char *dire;

  parent = 0;
  while (*dir == '/' && dir[1] == '/')
    dir++;
  if (*dir == '/' && !dir[1])
    return 1;
  while (*dir)
    {
      dire = strchrnul(dir, '/');
      if (data->localpool)
        id = stringpool_strn2id(&data->spool, dir, dire - dir, create);
      else
	id = strn2id(data->repo->pool, dir, dire - dir, create);
      if (!id)
	return 0;
      parent = dirpool_add_dir(&data->dirpool, parent, id, create);
      if (!parent)
	return 0;
      if (!*dire)
	break;
      dir = dire + 1;
      while (*dir == '/')
	dir++;
    }
  return parent;
}

const char *
repodata_dir2str(Repodata *data, Id did, const char *suf)
{
  Pool *pool = data->repo->pool;
  int l = 0;
  Id parent, comp;
  const char *comps;
  char *p;

  if (!did)
    return suf ? suf : "";
  parent = did;
  while (parent)
    {
      comp = dirpool_compid(&data->dirpool, parent);
      comps = stringpool_id2str(data->localpool ? &data->spool : &pool->ss, comp);
      l += strlen(comps);
      parent = dirpool_parent(&data->dirpool, parent);
      if (parent)
	l++;
    }
  if (suf)
    l += strlen(suf) + 1;
  p = pool_alloctmpspace(pool, l + 1) + l;
  *p = 0;
  if (suf)
    {
      p -= strlen(suf);
      strcpy(p, suf);
      *--p = '/';
    }
  parent = did;
  while (parent)
    {
      comp = dirpool_compid(&data->dirpool, parent);
      comps = stringpool_id2str(data->localpool ? &data->spool : &pool->ss, comp);
      l = strlen(comps);
      p -= l;
      strncpy(p, comps, l);
      parent = dirpool_parent(&data->dirpool, parent);
      if (parent)
        *--p = '/';
    }
  return p;
}


/***************************************************************
 * data management
 */

static inline unsigned char *
data_skip_schema(Repodata *data, unsigned char *dp, Id schema)
{
  Id *keyp = data->schemadata + data->schemata[schema];
  for (; *keyp; keyp++)
    dp = data_skip_key(data, dp, data->keys + *keyp);
  return dp;
}

unsigned char *
data_skip_key(Repodata *data, unsigned char *dp, Repokey *key)
{
  int nentries, schema;
  switch(key->type)
    {
    case REPOKEY_TYPE_FIXARRAY:
      dp = data_read_id(dp, &nentries);
      if (!nentries)
	return dp;
      dp = data_read_id(dp, &schema);
      while (nentries--)
	dp = data_skip_schema(data, dp, schema);
      return dp;
    case REPOKEY_TYPE_FLEXARRAY:
      dp = data_read_id(dp, &nentries);
      while (nentries--)
	{
	  dp = data_read_id(dp, &schema);
	  dp = data_skip_schema(data, dp, schema);
	}
      return dp;
    default:
      if (key->storage == KEY_STORAGE_INCORE)
        dp = data_skip(dp, key->type);
      else if (key->storage == KEY_STORAGE_VERTICAL_OFFSET)
	{
	  dp = data_skip(dp, REPOKEY_TYPE_ID);
	  dp = data_skip(dp, REPOKEY_TYPE_ID);
	}
      return dp;
    }
}

static unsigned char *
forward_to_key(Repodata *data, Id keyid, Id *keyp, unsigned char *dp)
{
  Id k;

  if (!keyid)
    return 0;
  while ((k = *keyp++) != 0)
    {
      if (k == keyid)
	return dp;
      if (data->keys[k].storage == KEY_STORAGE_VERTICAL_OFFSET)
	{
	  dp = data_skip(dp, REPOKEY_TYPE_ID);	/* skip offset */
	  dp = data_skip(dp, REPOKEY_TYPE_ID);	/* skip length */
	  continue;
	}
      if (data->keys[k].storage != KEY_STORAGE_INCORE)
	continue;
      dp = data_skip_key(data, dp, data->keys + k);
    }
  return 0;
}

static unsigned char *
get_vertical_data(Repodata *data, Repokey *key, Id off, Id len)
{
  unsigned char *dp;
  if (!len)
    return 0;
  if (off >= data->lastverticaloffset)
    {
      off -= data->lastverticaloffset;
      if (off + len > data->vincorelen)
	return 0;
      return data->vincore + off;
    }
  if (off + len > key->size)
    return 0;
  /* we now have the offset, go into vertical */
  off += data->verticaloffset[key - data->keys];
  /* fprintf(stderr, "key %d page %d\n", key->name, off / BLOB_PAGESIZE); */
  dp = repodata_load_page_range(data, off / BLOB_PAGESIZE, (off + len - 1) / BLOB_PAGESIZE);
  if (dp)
    dp += off % BLOB_PAGESIZE;
  return dp;
}

static inline unsigned char *
get_data(Repodata *data, Repokey *key, unsigned char **dpp)
{
  unsigned char *dp = *dpp;

  if (!dp)
    return 0;
  if (key->storage == KEY_STORAGE_INCORE)
    {
      /* hmm, this is a bit expensive */
      *dpp = data_skip_key(data, dp, key);
      return dp;
    }
  else if (key->storage == KEY_STORAGE_VERTICAL_OFFSET)
    {
      Id off, len;
      dp = data_read_id(dp, &off);
      dp = data_read_id(dp, &len);
      *dpp = dp;
      return get_vertical_data(data, key, off, len);
    }
  return 0;
}

static int
load_repodata(Repodata *data)
{
  if (data->loadcallback)
    {
      data->loadcallback(data);
      if (data->state == REPODATA_AVAILABLE)
	return 1;
    }
  data->state = REPODATA_ERROR;
  return 0;
}

static inline int
maybe_load_repodata(Repodata *data, Id keyname)
{
  if (keyname && !repodata_precheck_keyname(data, keyname))
    return 0;	/* do not bother... */
  switch(data->state)
    {
    case REPODATA_STUB:
      if (keyname)
	{
	  int i;
	  for (i = 0; i < data->nkeys; i++)
	    if (keyname == data->keys[i].name)
	      break;
	  if (i == data->nkeys)
	    return 0;
	}
      return load_repodata(data);
    case REPODATA_ERROR:
      return 0;
    case REPODATA_AVAILABLE:
      return 1;
    default:
      data->state = REPODATA_ERROR;
      return 0;
    }
}

static inline unsigned char*
entry2data(Repodata *data, Id entry, Id *schemap)
{
  unsigned char *dp = data->incoredata;
  if (!dp)
    return 0;
  if (entry == REPOENTRY_META)	/* META */
    dp += 1;
  else if (entry == REPOENTRY_POS)	/* META */
    {
      Pool *pool = data->repo->pool;
      if (data->repo != pool->pos.repo)
	return 0;
      if (data != data->repo->repodata + pool->pos.repodataid)
	return 0;
      *schemap = pool->pos.schema;
      return data->incoredata + pool->pos.dp;
    }
  else
    {
      if (entry < data->start || entry >= data->end)
	return 0;
      dp += data->incoreoffset[entry - data->start];
    }
  return data_read_id(dp, schemap);
}

/************************************************************************
 * data lookup
 */

static inline Id
find_schema_key(Repodata *data, Id schema, Id keyname)
{
  Id *keyp;
  for (keyp = data->schemadata + data->schemata[schema]; *keyp; keyp++)
    if (data->keys[*keyp].name == keyname)
      return *keyp;
  return 0;
}

static inline unsigned char *
find_key_data(Repodata *data, Id entry, Id keyname, Repokey **keyp)
{
  unsigned char *dp, *ddp;
  Id keyid, schema;
  Repokey *key;

  if (!maybe_load_repodata(data, keyname))
    return 0;
  dp = entry2data(data, entry, &schema);
  if (!dp)
    return 0;
  keyid = find_schema_key(data, schema, keyname);
  if (!keyid)
    return 0;
  key = data->keys + keyid;
  *keyp = key;
  if (key->type == REPOKEY_TYPE_VOID || key->type == REPOKEY_TYPE_CONSTANT || key->type == REPOKEY_TYPE_CONSTANTID)
    return dp;
  dp = forward_to_key(data, keyid, data->schemadata + data->schemata[schema], dp);
  if (!dp)
    return 0;
  ddp = get_data(data, key, &dp);
  return ddp;
}


Id
repodata_lookup_id(Repodata *data, Id entry, Id keyname)
{
  unsigned char *dp;
  Repokey *key;
  Id id;

  dp = find_key_data(data, entry, keyname, &key);
  if (!dp)
    return 0;
  if (key->type == REPOKEY_TYPE_CONSTANTID)
    return key->size;
  if (key->type != REPOKEY_TYPE_ID)
    return 0;
  dp = data_read_id(dp, &id);
  return id;
}

const char *
repodata_lookup_str(Repodata *data, Id entry, Id keyname)
{
  unsigned char *dp;
  Repokey *key;
  Id id;

  dp = find_key_data(data, entry, keyname, &key);
  if (!dp)
    return 0;
  if (key->type == REPOKEY_TYPE_STR)
    return (const char *)dp;
  if (key->type == REPOKEY_TYPE_CONSTANTID)
    return id2str(data->repo->pool, key->size);
  if (key->type == REPOKEY_TYPE_ID)
    dp = data_read_id(dp, &id);
  else
    return 0;
  if (data->localpool)
    return data->spool.stringspace + data->spool.strings[id];
  return id2str(data->repo->pool, id);
}

int
repodata_lookup_num(Repodata *data, Id entry, Id keyname, unsigned int *value)
{
  unsigned char *dp;
  Repokey *key;
  KeyValue kv;

  *value = 0;
  dp = find_key_data(data, entry, keyname, &key);
  if (!dp)
    return 0;
  if (key->type == REPOKEY_TYPE_NUM
      || key->type == REPOKEY_TYPE_U32
      || key->type == REPOKEY_TYPE_CONSTANT)
    {
      dp = data_fetch(dp, &kv, key);
      *value = kv.num;
      return 1;
    }
  return 0;
}

int
repodata_lookup_void(Repodata *data, Id entry, Id keyname)
{
  Id schema;
  Id *keyp;
  unsigned char *dp;

  if (!maybe_load_repodata(data, keyname))
    return 0;
  dp = entry2data(data, entry, &schema);
  if (!dp)
    return 0;
  /* can't use find_schema_key as we need to test the type */
  for (keyp = data->schemadata + data->schemata[schema]; *keyp; keyp++)
    if (data->keys[*keyp].name == keyname && data->keys[*keyp].type == REPOKEY_TYPE_VOID)
      return 1;
  return 0;
}

const unsigned char *
repodata_lookup_bin_checksum(Repodata *data, Id entry, Id keyname, Id *typep)
{
  unsigned char *dp;
  Repokey *key;

  dp = find_key_data(data, entry, keyname, &key);
  if (!dp)
    return 0;
  *typep = key->type;
  return dp;
}


/************************************************************************
 * data search
 */

struct subschema_data {
  Solvable *s;
  void *cbdata;
  KeyValue *parent;
};

/* search in a specific entry */
void
repodata_search(Repodata *data, Id entry, Id keyname, int (*callback)(void *cbdata, Solvable *s, Repodata *data, Repokey *key, KeyValue *kv), void *cbdata)
{
  Id schema;
  Repokey *key;
  Id k, keyid, *kp, *keyp;
  unsigned char *dp, *ddp;
  int onekey = 0;
  int stop;
  KeyValue kv;
  Solvable *s;

  if (!maybe_load_repodata(data, keyname))
    return;
  if (entry == REPOENTRY_SUBSCHEMA)
    {
      struct subschema_data *subd = cbdata;
      cbdata = subd->cbdata;
      s = subd->s;
      schema = subd->parent->id;
      dp = (unsigned char *)subd->parent->str;
      kv.parent = subd->parent;
    }
  else
    {
      schema = 0;
      dp = entry2data(data, entry, &schema);
      if (!dp)
	return;
      s = data->repo->pool->solvables + entry;
      kv.parent = 0;
    }
  keyp = data->schemadata + data->schemata[schema];
  if (keyname)
    {
      /* search for a specific key */
      for (kp = keyp; (k = *kp++) != 0; )
	if (data->keys[k].name == keyname)
	  break;
      if (k == 0)
	return;
      dp = forward_to_key(data, k, data->schemadata + data->schemata[schema], dp);
      if (!dp)
	return;
      keyp = kp - 1;
      onekey = 1;
    }
  while ((keyid = *keyp++) != 0)
    {
      stop = 0;
      key = data->keys + keyid;
      ddp = get_data(data, key, &dp);

      if (key->type == REPOKEY_TYPE_FLEXARRAY || key->type == REPOKEY_TYPE_FIXARRAY)
	{
	  struct subschema_data subd;
	  int nentries;
	  Id schema = 0;

	  subd.cbdata = cbdata;
	  subd.s = s;
	  subd.parent = &kv;
	  ddp = data_read_id(ddp, &nentries);
	  kv.num = nentries;
	  kv.entry = 0;
          while (ddp && nentries > 0)
	    {
	      if (key->type == REPOKEY_TYPE_FLEXARRAY || !kv.entry)
	        ddp = data_read_id(ddp, &schema);
	      kv.id = schema;
	      kv.str = (char *)ddp;
	      stop = callback(cbdata, s, data, key, &kv);
	      if (stop > SEARCH_NEXT_KEY)
		return;
	      if (stop)
		break;
	      if (!keyname)
	        repodata_search(data, REPOENTRY_SUBSCHEMA, 0, callback, &subd);
	      ddp = data_skip_schema(data, ddp, schema);
	      nentries--;
	      kv.entry++;
	    }
	  if (!nentries)
	    {
	      /* sentinel */
	      kv.eof = 1;
	      kv.str = (char *)ddp;
	      stop = callback(cbdata, s, data, key, &kv);
	      if (stop > SEARCH_NEXT_KEY)
		return;
	    }
	  if (onekey)
	    return;
	  continue;
	}
      kv.entry = 0;
      do
	{
	  ddp = data_fetch(ddp, &kv, key);
	  if (!ddp)
	    break;
	  stop = callback(cbdata, s, data, key, &kv);
	  kv.entry++;
	}
      while (!kv.eof && !stop);
      if (onekey || stop > SEARCH_NEXT_KEY)
	return;
    }
}

void
repodata_setpos_kv(Repodata *data, KeyValue *kv)
{
  Pool *pool = data->repo->pool;
  if (!kv)
    {
      pool->pos.repo = 0;
      pool->pos.repodataid = 0;
      pool->pos.dp = 0;
      pool->pos.schema = 0;
    }
  else
    {
      pool->pos.repo = 0;
      pool->pos.repodataid = data - data->repo->repodata;
      pool->pos.dp = (unsigned char *)kv->str - data->incoredata;
      pool->pos.schema = kv->id;
    }
}

/************************************************************************/

static Repokey solvablekeys[RPM_RPMDBID - SOLVABLE_NAME + 1] = {
  { SOLVABLE_NAME,        REPOKEY_TYPE_ID, 0, KEY_STORAGE_SOLVABLE },
  { SOLVABLE_ARCH,        REPOKEY_TYPE_ID, 0, KEY_STORAGE_SOLVABLE },
  { SOLVABLE_EVR,         REPOKEY_TYPE_ID, 0, KEY_STORAGE_SOLVABLE },
  { SOLVABLE_VENDOR,      REPOKEY_TYPE_ID, 0, KEY_STORAGE_SOLVABLE },
  { SOLVABLE_PROVIDES,    REPOKEY_TYPE_IDARRAY, 0, KEY_STORAGE_SOLVABLE },
  { SOLVABLE_OBSOLETES,   REPOKEY_TYPE_IDARRAY, 0, KEY_STORAGE_SOLVABLE },
  { SOLVABLE_CONFLICTS,   REPOKEY_TYPE_IDARRAY, 0, KEY_STORAGE_SOLVABLE },
  { SOLVABLE_REQUIRES,    REPOKEY_TYPE_IDARRAY, 0, KEY_STORAGE_SOLVABLE },
  { SOLVABLE_RECOMMENDS,  REPOKEY_TYPE_IDARRAY, 0, KEY_STORAGE_SOLVABLE },
  { SOLVABLE_SUGGESTS,    REPOKEY_TYPE_IDARRAY, 0, KEY_STORAGE_SOLVABLE },
  { SOLVABLE_SUPPLEMENTS, REPOKEY_TYPE_IDARRAY, 0, KEY_STORAGE_SOLVABLE },
  { SOLVABLE_ENHANCES,    REPOKEY_TYPE_IDARRAY, 0, KEY_STORAGE_SOLVABLE },
  { RPM_RPMDBID,          REPOKEY_TYPE_U32, 0, KEY_STORAGE_SOLVABLE },
};

#if 1
static inline Id *
solvabledata_fetch(Solvable *s, KeyValue *kv, Id keyname)
{
  kv->id = keyname;
  switch (keyname)
    {
    case SOLVABLE_NAME:
      kv->eof = 1;
      return &s->name;
    case SOLVABLE_ARCH:
      kv->eof = 1;
      return &s->arch;
    case SOLVABLE_EVR:
      kv->eof = 1;
      return &s->evr;
    case SOLVABLE_VENDOR:
      kv->eof = 1;
      return &s->vendor;
    case SOLVABLE_PROVIDES:
      kv->eof = 0;
      return s->provides ? s->repo->idarraydata + s->provides : 0;
    case SOLVABLE_OBSOLETES:
      kv->eof = 0;
      return s->obsoletes ? s->repo->idarraydata + s->obsoletes : 0;
    case SOLVABLE_CONFLICTS:
      kv->eof = 0;
      return s->conflicts ? s->repo->idarraydata + s->conflicts : 0;
    case SOLVABLE_REQUIRES:
      kv->eof = 0;
      return s->requires ? s->repo->idarraydata + s->requires : 0;
    case SOLVABLE_RECOMMENDS:
      kv->eof = 0;
      return s->recommends ? s->repo->idarraydata + s->recommends : 0;
    case SOLVABLE_SUPPLEMENTS:
      kv->eof = 0;
      return s->supplements ? s->repo->idarraydata + s->supplements : 0;
    case SOLVABLE_SUGGESTS:
      kv->eof = 0;
      return s->suggests ? s->repo->idarraydata + s->suggests : 0;
    case SOLVABLE_ENHANCES:
      kv->eof = 0;
      return s->enhances ? s->repo->idarraydata + s->enhances : 0;
    case RPM_RPMDBID:
      kv->eof = 1;
      return s->repo->rpmdbid ? s->repo->rpmdbid + (s - s->repo->pool->solvables - s->repo->start) : 0;
    default:
      return 0;
    }
}

void
datamatcher_init(Datamatcher *ma, Pool *pool, const char *match, int flags)
{
  ma->pool = pool;
  ma->match = (void *)match;
  ma->flags = flags;
  ma->error = 0;
  if ((flags & SEARCH_STRINGMASK) == SEARCH_REGEX)
    {
      ma->match = sat_calloc(1, sizeof(regex_t));
      ma->error = regcomp((regex_t *)ma->match, match, REG_EXTENDED | REG_NOSUB | REG_NEWLINE | ((flags & SEARCH_NOCASE) ? REG_ICASE : 0));
      if (ma->error)
	{
	  sat_free(ma->match);
	  ma->match = (void *)match;
	  ma->flags = (flags & ~SEARCH_STRINGMASK) | SEARCH_ERROR;
	}
    }
}

void
datamatcher_free(Datamatcher *ma)
{
  if ((ma->flags & SEARCH_STRINGMASK) == SEARCH_REGEX && ma->match)
    {
      regfree(ma->match);
      ma->match = sat_free(ma->match);
    }
}

int
datamatcher_match(Datamatcher *ma, Repodata *data, Repokey *key, KeyValue *kv)
{
  switch (key->type)
    {
    case REPOKEY_TYPE_ID:
    case REPOKEY_TYPE_IDARRAY:
      if (data && data->localpool)
	kv->str = stringpool_id2str(&data->spool, kv->id);
      else
	kv->str = id2str(ma->pool, kv->id);
      break;
    case REPOKEY_TYPE_STR:
      break;
    case REPOKEY_TYPE_DIRSTRARRAY:
      if (!(ma->flags & SEARCH_FILES))
	return 0;
      /* Put the full filename into kv->str.  */
      kv->str = repodata_dir2str(data, kv->id, kv->str);
      /* And to compensate for that put the "empty" directory into
	 kv->id, so that later calls to repodata_dir2str on this data
	 come up with the same filename again.  */
      kv->id = 0;
      break;
    default:
      return 0;
    }
  /* Maybe skip the kind specifier.  Do this only for SOLVABLE attributes,
     for the others we can't know if a colon separates a kind or not.  */
  if ((ma->flags & SEARCH_SKIP_KIND) != 0 && key->storage == KEY_STORAGE_SOLVABLE)
    {
      const char *s = strchr(kv->str, ':');
      if (s)
	kv->str = s + 1;
    }
  switch ((ma->flags & SEARCH_STRINGMASK))
    {
    case SEARCH_SUBSTRING:
      if (ma->flags & SEARCH_NOCASE)
	{
	  if (!strcasestr(kv->str, (const char *)ma->match))
	    return 0;
	}
      else
	{
	  if (!strstr(kv->str, (const char *)ma->match))
	    return 0;
	}
      break;
    case SEARCH_STRING:
      if (ma->flags & SEARCH_NOCASE)
	{
	  if (strcasecmp((const char *)ma->match, kv->str))
	    return 0;
	}
      else
	{
	  if (strcmp((const char *)ma->match, kv->str))
	    return 0;
	}
      break;
    case SEARCH_GLOB:
      if (fnmatch((const char *)ma->match, kv->str, (ma->flags & SEARCH_NOCASE) ? FNM_CASEFOLD : 0))
	return 0;
      break;
    case SEARCH_REGEX:
      if (regexec((const regex_t *)ma->match, kv->str, 0, NULL, 0))
	return 0;
      break;
    default:
      return 0;
    }
  return 1;
}

enum {
  di_bye,

  di_nextattr,
  di_nextkey,
  di_nextrepodata,
  di_nextsolvable,
  di_nextrepo,

  di_enterrepo,
  di_entersolvable,
  di_enterrepodata,
  di_enterkey,

  di_nextarrayelement,
  di_entersub,
  di_leavesub,

  di_nextsolvableattr,
  di_nextsolvablekey,
  di_entersolvablekey
};

void
dataiterator_init(Dataiterator *di, Repo *repo, Id p, Id keyname, const char *match, int flags)
{
  memset(di, 0, sizeof(*di));
  di->repo = repo;
  di->keyname = keyname;
  di->entry = p;
  di->pool = repo->pool;
  if (p)
    flags |= SEARCH_THISENTRY;
  di->flags = flags;
  if (repo)
    di->repoid = -1;
  if (match)
    datamatcher_init(&di->matcher, di->pool, match, flags);
  if (p == REPOENTRY_POS)
    {
      di->repo = di->pool->pos.repo;
      di->data = di->repo->repodata + di->pool->pos.repodataid;
      di->repoid = -1;
      di->repodataid = -1;
    }
  di->state = di_enterrepo;
}

void
dataiterator_free(Dataiterator *di)
{
  if (di->matcher.match)
    datamatcher_free(&di->matcher);
}

int
dataiterator_step(Dataiterator *di)
{
  Id schema;

  for (;;)
    {
      switch (di->state)
	{
	case di_nextattr: di_nextattr:
          di->kv.entry++;
	  di->ddp = data_fetch(di->ddp, &di->kv, di->key);
	  if (di->kv.eof)
	    di->state = di_nextkey;
	  else
	    di->state = di_nextattr;
	  break;

	case di_nextkey: di_nextkey:
	  if (!di->keyname)
	    {
	      if (*++di->keyp)
		goto di_enterkey;
	    }
	  else if ((di->flags & SEARCH_SUB) != 0)
	    {
	      Id *keyp = di->keyp;
	      for (keyp++; *keyp; keyp++)
		if (di->data->keys[*keyp].name == di->keyname || 
		    di->data->keys[*keyp].type == REPOKEY_TYPE_FIXARRAY || 
		    di->data->keys[*keyp].type == REPOKEY_TYPE_FLEXARRAY)
		  break;
	      if (*keyp && (di->dp = forward_to_key(di->data, *keyp, di->keyp, di->dp)) != 0)
		{
		  di->keyp = keyp;
		  goto di_enterkey;
		}
	    }

	  if (di->kv.parent)
	    goto di_leavesub;
	  /* FALLTHROUGH */

	case di_nextrepodata: di_nextrepodata:
	  if (di->repodataid >= 0 && ++di->repodataid < di->repo->nrepodata)
	      goto di_enterrepodata;
	  /* FALLTHROUGH */

	case di_nextsolvable:
	  if (!(di->flags & SEARCH_THISENTRY))
	    {
	      if (di->entry < 0)
		di->entry = di->repo->start;
	      else
	        di->entry++;
	      for (; di->entry < di->repo->end; di->entry++)
		{
		  if (di->pool->solvables[di->entry].repo == di->repo)
		    goto di_entersolvable;
		}
	    }
	  /* FALLTHROUGH */

	case di_nextrepo:
	  if (di->repoid >= 0)
	    {
	      di->repoid++;
	      if (di->repoid < di->pool->nrepos)
		{
		  di->repo = di->pool->repos[di->repoid];
	          goto di_enterrepo;
		}
	    }

	/* FALLTHROUGH */
	case di_bye:
	  di->state = di_bye;
	  return 0;

	case di_enterrepo: di_enterrepo:
	  if (!(di->flags & SEARCH_THISENTRY))
	    di->entry = di->repo->start;
	  /* FALLTHROUGH */

	case di_entersolvable: di_entersolvable:
	  if (di->repodataid >= 0)
	    {
	      di->repodataid = 0;
	      if (di->entry > 0 && (!di->keyname || (di->keyname >= SOLVABLE_NAME && di->keyname <= RPM_RPMDBID)))
		{
		  di->key = solvablekeys + (di->keyname ? di->keyname - SOLVABLE_NAME : 0);
		  di->data = 0;
		  goto di_entersolvablekey;
		}
	    }

	case di_enterrepodata: di_enterrepodata:
	  if (di->repodataid >= 0)
	    di->data = di->repo->repodata + di->repodataid;
	  if (!maybe_load_repodata(di->data, di->keyname))
	    goto di_nextrepodata;
	  di->dp = entry2data(di->data, di->entry, &schema);
	  if (!di->dp)
	    goto di_nextrepodata;
	  di->keyp = di->data->schemadata + di->data->schemata[schema];
	  if (di->keyname)
	    {
	      Id *keyp;
	      if ((di->flags & SEARCH_SUB) != 0)
		{
		  di->keyp--;
		  goto di_nextkey;
		}
	      for (keyp = di->keyp; *keyp; keyp++)
		if (di->data->keys[*keyp].name == di->keyname)
		  break;
	      if (!*keyp)
		goto di_nextrepodata;
	      di->dp = forward_to_key(di->data, *keyp, di->keyp, di->dp);
	      di->keyp = keyp;
	      if (!di->dp)
		goto di_nextrepodata;
	    }

	case di_enterkey: di_enterkey:
	  di->kv.entry = -1;
	  di->key = di->data->keys + *di->keyp;
	  di->ddp = get_data(di->data, di->key, &di->dp);
	  if (!di->ddp)
	    goto di_nextkey;
	  if (di->key->type == REPOKEY_TYPE_FIXARRAY || di->key->type == REPOKEY_TYPE_FLEXARRAY)
	    {
	      di->ddp = data_read_id(di->ddp, &di->kv.num);
	      di->kv.entry = -1;
	      di->kv.eof = 0;
	      goto di_nextarrayelement;
	    }
	  goto di_nextattr;

	case di_nextarrayelement: di_nextarrayelement:
	  di->kv.entry++;
	  if (di->kv.entry)
	    di->ddp = data_skip_schema(di->data, di->ddp, di->kv.id);
	  if (di->kv.entry == di->kv.num)
	    {
	      if (di->keyname && di->key->name != di->keyname)
		goto di_nextkey;
	      di->kv.str = (char *)di->ddp;
	      di->kv.eof = 1;
	      di->state = di_nextkey;
	      break;
	    }
	  if (di->key->type == REPOKEY_TYPE_FLEXARRAY || !di->kv.entry)
	    di->ddp = data_read_id(di->ddp, &di->kv.id);
	  di->kv.str = (char *)di->ddp;
	  if (di->keyname && di->key->name != di->keyname)
	    goto di_entersub;
	  if ((di->flags & SEARCH_SUB) != 0)
	    di->state = di_entersub;
	  else
	    di->state = di_nextarrayelement;
	  break;

	case di_entersub: di_entersub:
	  if (di->nparents == sizeof(di->parents)/sizeof(*di->parents) - 1)
	    goto di_nextarrayelement;	/* sorry, full */
	  di->parents[di->nparents].kv = di->kv;
	  di->parents[di->nparents].dp = di->dp;
	  di->parents[di->nparents].keyp = di->keyp;
	  di->dp = (unsigned char *)di->kv.str;
	  di->keyp = di->data->schemadata + di->data->schemata[di->kv.id];
	  memset(&di->kv, 0, sizeof(di->kv));
	  di->kv.parent = &di->parents[di->nparents].kv;
	  di->nparents++;
	  di->keyp--;
	  goto di_nextkey;
	  
	case di_leavesub: di_leavesub:
	  di->nparents--;
	  di->dp = di->parents[di->nparents].dp;
	  di->kv = di->parents[di->nparents].kv;
	  di->keyp = di->parents[di->nparents].keyp;
	  di->key = di->data->keys + *di->keyp;
	  di->ddp = (unsigned char *)di->kv.str;
	  goto di_nextarrayelement;

        /* special solvable attr handling follows */

	case di_nextsolvableattr:
	  di->kv.id = *di->idp++;
          di->kv.entry++;
	  if (!*di->idp)
	    {
	      di->kv.eof = 1;
	      di->state = di_nextsolvablekey;
	    }
	  break;

	case di_nextsolvablekey: di_nextsolvablekey:
	  if (di->keyname || di->key->name == RPM_RPMDBID)
	    goto di_enterrepodata;
	  di->key++;
	  /* FALLTHROUGH */

	case di_entersolvablekey: di_entersolvablekey:
	  di->idp = solvabledata_fetch(di->pool->solvables + di->entry, &di->kv, di->key->name);
	  if (!di->idp || !di->idp[0])
	    goto di_nextsolvablekey;
	  di->kv.id = di->idp[0];
	  di->kv.num = di->idp[0];
	  if (!di->kv.eof && !di->idp[1])
	    di->kv.eof = 1;
	  di->kv.entry = 0;
	  if (di->kv.eof)
	    di->state = di_nextsolvablekey;
	  else
	    di->state = di_nextsolvableattr;
	  break;
	}

      if (di->matcher.match)
	if (!datamatcher_match(&di->matcher, di->data, di->key, &di->kv))
	  continue;
      /* found something! */
      return 1;
    }
}

void
dataiterator_setpos(Dataiterator *di)
{
  di->pool->pos.repo = di->repo;
  di->pool->pos.repodataid = di->data - di->repo->repodata;
  di->pool->pos.schema = di->kv.id;
  di->pool->pos.dp = (unsigned char *)di->kv.str - di->data->incoredata;
}

void
dataiterator_skip_attribute(Dataiterator *di)
{
  if (di->state == di_nextsolvableattr)
    di->state = di_nextsolvablekey;
  else
    di->state = di_nextkey;
}

void
dataiterator_skip_solvable(Dataiterator *di)
{
  di->state = di_nextsolvable;
}

void
dataiterator_skip_repo(Dataiterator *di)
{
  di->state = di_nextrepo;
}

void
dataiterator_jump_to_solvable(Dataiterator *di, Solvable *s)
{
  di->repo = s->repo;
  di->repoid = -1;
  di->entry = s - di->pool->solvables;
  di->state = di_entersolvable;
}

void
dataiterator_jump_to_repo(Dataiterator *di, Repo *repo)
{
  di->repo = repo;
  di->repoid = -1;
  di->state = di_enterrepo;
}

int
dataiterator_match(Dataiterator *di, int flags, const void *vmatch)
{
  Datamatcher matcher = di->matcher;
  matcher.flags = flags;
  matcher.match = (void *)vmatch;
  return datamatcher_match(&matcher, di->data, di->key, &di->kv);
}

#else

/************************************************************************
 * data search iterator
 */

static void
dataiterator_newdata(Dataiterator *di)
{
  Id keyname = di->keyname;
  Repodata *data = di->data;
  di->nextkeydp = 0;

  if (data->state == REPODATA_STUB)
    {
      if (keyname)
	{
	  int j;
	  for (j = 1; j < data->nkeys; j++)
	    if (keyname == data->keys[j].name)
	      break;
	  if (j == data->nkeys)
	    return;
	}
      /* load it */
      if (data->loadcallback)
	data->loadcallback(data);
      else
	data->state = REPODATA_ERROR;
    }
  if (data->state == REPODATA_ERROR)
    return;

  Id schema;
  unsigned char *dp = data->incoredata;
  if (!dp)
    return;
  if (di->solvid >= 0)
    dp += data->incoreoffset[di->solvid - data->start];
  dp = data_read_id(dp, &schema);
  Id *keyp = data->schemadata + data->schemata[schema];
  if (keyname)
    {
      Id k, *kp;
      /* search in a specific key */
      for (kp = keyp; (k = *kp++) != 0; )
	if (data->keys[k].name == keyname)
	  break;
      if (k == 0)
	return;
      dp = forward_to_key(data, k, keyp, dp);
      if (!dp)
	return;
      keyp = kp - 1;
    }
  Id keyid = *keyp++;
  if (!keyid)
    return;

  di->data = data;
  di->key = di->data->keys + keyid;
  di->keyp = keyp;
  di->dp = 0;

  di->nextkeydp = dp;
  di->dp = get_data(di->data, di->key, &di->nextkeydp);
  di->kv.eof = 0;
}

void
dataiterator_init(Dataiterator *di, Repo *repo, Id p, Id keyname,
		  const char *match, int flags)
{
  di->flags = flags;
  if (p > 0)
    {
      di->solvid = p;
      di->flags |= __SEARCH_ONESOLVABLE;
      di->data = repo->repodata - 1;
      if (flags & SEARCH_NO_STORAGE_SOLVABLE)
	di->state = 0;
      else
	di->state = 1;
    }
  else
    {
      di->solvid = repo->start - 1;
      if (di->solvid < 0)
	{
	  fprintf(stderr, "A repo contains the NULL solvable!\n");
	  exit(1);
	}
      di->data = repo->repodata + repo->nrepodata - 1;
      di->state = 0;
    }

  di->match = match;
  if ((di->flags & SEARCH_STRINGMASK) == SEARCH_REGEX)
    {
      if (di->match)
        {
          /* We feed multiple lines eventually (e.g. authors or descriptions),
             so set REG_NEWLINE. */
          di->regex_err =
            regcomp(&di->regex, di->match,
              REG_EXTENDED | REG_NOSUB | REG_NEWLINE
              | ((di->flags & SEARCH_NOCASE) ? REG_ICASE : 0));
#if 0
          if (di->regex_err != 0)
            {
              fprintf(stderr, "Given regex failed to compile: %s\n", di->match);
              fprintf(stderr, "regcomp error code: %d\n", di->regex_err);
              exit(1);
            }
#else
        }
      else
        {
          di->flags |= (di->flags & SEARCH_STRINGMASK) | SEARCH_STRING;
          di->regex_err = 0;
#endif
        }
    }

  di->keyname = keyname;
  static Id zeroid = 0;
  di->keyp = &zeroid;
  di->kv.eof = 1;
  di->repo = repo;
  di->idp = 0;
  di->subkeyp = 0;
}

/* FIXME factor and merge with repo_matchvalue */
static int
dataiterator_match_int_real(Dataiterator *di, int flags, const void *vmatch)
{
  KeyValue *kv = &di->kv;
  const char *match = vmatch;
  if ((flags & SEARCH_STRINGMASK) != 0)
    {
      switch (di->key->type)
	{
	case REPOKEY_TYPE_ID:
	case REPOKEY_TYPE_IDARRAY:
	  if (di->data && di->data->localpool)
	    kv->str = stringpool_id2str(&di->data->spool, kv->id);
	  else
	    kv->str = id2str(di->repo->pool, kv->id);
	  break;
	case REPOKEY_TYPE_STR:
	  break;
	case REPOKEY_TYPE_DIRSTRARRAY:
	  if (!(flags & SEARCH_FILES))
	    return 0;
	  /* Put the full filename into kv->str.  */
	  kv->str = repodata_dir2str(di->data, kv->id, kv->str);
	  /* And to compensate for that put the "empty" directory into
	     kv->id, so that later calls to repodata_dir2str on this data
	     come up with the same filename again.  */
	  kv->id = 0;
	  break;
	default:
	  return 0;
	}
      /* Maybe skip the kind specifier.  Do this only for SOLVABLE attributes,
         for the others we can't know if a colon separates a kind or not.  */
      if ((flags & SEARCH_SKIP_KIND)
	  && di->key->storage == KEY_STORAGE_SOLVABLE)
	{
	  const char *s = strchr(kv->str, ':');
	  if (s)
	    kv->str = s + 1;
	}
      switch ((flags & SEARCH_STRINGMASK))
	{
	  case SEARCH_SUBSTRING:
	    if (flags & SEARCH_NOCASE)
	      {
	        if (!strcasestr(kv->str, match))
		  return 0;
	      }
	    else
	      {
	        if (!strstr(kv->str, match))
		  return 0;
	      }
	    break;
	  case SEARCH_STRING:
	    if (flags & SEARCH_NOCASE)
	      {
	        if (strcasecmp(match, kv->str))
		  return 0;
	      }
	    else
	      {
	        if (strcmp(match, kv->str))
		  return 0;
	      }
	    break;
	  case SEARCH_GLOB:
	    if (fnmatch(match, kv->str, (flags & SEARCH_NOCASE) ? FNM_CASEFOLD : 0))
	      return 0;
	    break;
	  case SEARCH_REGEX:
	    if (regexec((const regex_t *)vmatch, kv->str, 0, NULL, 0))
	      return 0;
	    break;
	  default:
	    return 0;
	}
    }
  return 1;
}

static int
dataiterator_match_int(Dataiterator *di)
{
  if ((di->flags & SEARCH_STRINGMASK) == SEARCH_REGEX)
    return dataiterator_match_int_real(di, di->flags, &di->regex);
  else
    return dataiterator_match_int_real(di, di->flags, di->match);
}

int
dataiterator_match(Dataiterator *di, int flags, const void *vmatch)
{
  return dataiterator_match_int_real(di, flags, vmatch);
}

int
dataiterator_step(Dataiterator *di)
{
restart:
  while (1)
    {
      if (di->state)
	{
	  /* we're stepping through solvable data, 1 -> SOLVABLE_NAME... */
	  if (di->idp)
	    {
	      /* we're stepping through an id array */
	      Id *idp = di->idp;
	      if (*idp)
		{
		  di->kv.id = *idp;
		  di->idp++;
		  di->kv.eof = idp[1] ? 0 : 1;
		  goto weg2;
		}
	      else
		di->idp = 0;
	    }
	  Solvable *s = di->repo->pool->solvables + di->solvid;
	  int state = di->state;
	  di->key = solvablekeys + state - 1;
	  if (di->keyname)
	    di->state = RPM_RPMDBID;
	  else
	    di->state++;
	  if (state == 1)
	    {
	      di->data = 0;
	      if (di->keyname)
		state = di->keyname - 1;
	    }
	  switch (state + 1)
	    {
	      case SOLVABLE_NAME:
		if (!s->name)
		  continue;
		di->kv.id = s->name;
		di->kv.eof = 1;
		break;
	      case SOLVABLE_ARCH:
		if (!s->arch)
		  continue;
		di->kv.id = s->arch;
		di->kv.eof = 1;
		break;
	      case SOLVABLE_EVR:
		if (!s->evr)
		  continue;
		di->kv.id = s->evr;
		di->kv.eof = 1;
		break;
	      case SOLVABLE_VENDOR:
		if (!s->vendor)
		  continue;
		di->kv.id = s->vendor;
		di->kv.eof = 1;
		break;
	      case SOLVABLE_PROVIDES:
		di->idp = s->provides
		    ? di->repo->idarraydata + s->provides : 0;
		continue;
	      case SOLVABLE_OBSOLETES:
		di->idp = s->obsoletes
		    ? di->repo->idarraydata + s->obsoletes : 0;
		continue;
	      case SOLVABLE_CONFLICTS:
		di->idp = s->conflicts
		    ? di->repo->idarraydata + s->conflicts : 0;
		continue;
	      case SOLVABLE_REQUIRES:
		di->idp = s->requires
		    ? di->repo->idarraydata + s->requires : 0;
		continue;
	      case SOLVABLE_RECOMMENDS:
		di->idp = s->recommends
		    ? di->repo->idarraydata + s->recommends : 0;
		continue;
	      case SOLVABLE_SUPPLEMENTS:
		di->idp = s->supplements
		    ? di->repo->idarraydata + s->supplements : 0;
		continue;
	      case SOLVABLE_SUGGESTS:
		di->idp = s->suggests
		    ? di->repo->idarraydata + s->suggests : 0;
		continue;
	      case SOLVABLE_ENHANCES:
		di->idp = s->enhances
		    ? di->repo->idarraydata + s->enhances : 0;
		continue;
	      case RPM_RPMDBID:
		if (!di->repo->rpmdbid)
		  continue;
		di->kv.num = di->repo->rpmdbid[di->solvid - di->repo->start];
		di->kv.eof = 1;
		break;
	      default:
		di->data = di->repo->repodata - 1;
		di->kv.eof = 1;
		di->state = 0;
		continue;
	    }
	}
      else if (di->subkeyp)
	{
	  Id keyid;
	  if (!di->subnum)
	    {
	      /* Send end-of-substruct.  We are here only when we saw a
	         _COUNTED key one level up.  Since then we didn't increment
		 ->keyp, so it still can be found at keyp[-1].  */
	      di->kv.eof = 2;
	      di->key = di->data->keys + di->keyp[-1];
	      di->subkeyp = 0;
	    }
	  else if (!(keyid = *di->subkeyp++))
	    {
	      /* Send end-of-element.  See above for keyp[-1].  */
	      di->kv.eof = 1;
	      di->key = di->data->keys + di->keyp[-1];
	      if (di->subschema)
	        di->subkeyp = di->data->schemadata + di->data->schemata[di->subschema];
	      else
		{
		  di->dp = data_read_id(di->dp, &di->subschema);
		  di->subkeyp = di->data->schemadata + di->data->schemata[di->subschema];
		  di->subschema = 0;
		}
	      di->subnum--;
	    }
	  else
	    {
	      di->key = di->data->keys + keyid;
	      di->dp = data_fetch(di->dp, &di->kv, di->key);
	      if (!di->dp)
		exit(1);
	    }
	}
      else
	{
	  if (di->kv.eof)
	    di->dp = 0;
	  else
	    di->dp = data_fetch(di->dp, &di->kv, di->key);

	  while (!di->dp)
	    {
	      Id keyid;
	      if (di->keyname || !(keyid = *di->keyp++))
		{
		  while (1)
		    {
		      Repo *repo = di->repo;
		      Repodata *data = ++di->data;
		      if (data >= repo->repodata + repo->nrepodata)
			{
			  if (di->flags & __SEARCH_ONESOLVABLE)
			    return 0;
			  if (di->solvid >= 0)
			    {
			      while (++di->solvid < repo->end)
				if (repo->pool->solvables[di->solvid].repo == repo)
				  break;
			      if (di->solvid >= repo->end)
				{
				  if (!(di->flags & SEARCH_EXTRA))
				    goto skiprepo;
				  goto skiprepo;
				}
			    }
			  else
			    {
				{
skiprepo:;
				  Pool *pool = di->repo->pool;
				  if (!(di->flags & SEARCH_ALL_REPOS)
				      || di->repo == pool->repos[pool->nrepos - 1])
				    return 0;
				  int i;
				  for (i = 0; i < pool->nrepos; i++)
				    if (di->repo == pool->repos[i])
				      break;
				  di->repo = pool->repos[i + 1];
				  dataiterator_init(di, di->repo, 0, di->keyname, di->match, di->flags);
				  continue;
				}
			    }
			  di->data = repo->repodata - 1;
			  if ((di->flags & SEARCH_NO_STORAGE_SOLVABLE))
			    continue;
			  static Id zeroid = 0;
			  di->keyp = &zeroid;
			  di->state = 1;
			  goto restart;
			}
		      if ((di->solvid >= 0 && di->solvid >= data->start && di->solvid < data->end))
			{
			  dataiterator_newdata(di);
			  if (di->nextkeydp)
			    break;
			}
		    }
		}
	      else
		{
		  di->key = di->data->keys + keyid;
		  di->dp = get_data(di->data, di->key, &di->nextkeydp);
		}
	      di->dp = data_fetch(di->dp, &di->kv, di->key);
	    }
	  if (di->key->type == REPOKEY_TYPE_FIXARRAY)
	    {
	      di->subnum = di->kv.num;
	      di->subschema = di->kv.id;
	      di->kv.eof = 0;
	      di->subkeyp = di->data->schemadata + di->data->schemata[di->subschema];
	    }
	  if (di->key->type == REPOKEY_TYPE_FLEXARRAY)
	    {
	      di->subnum = di->kv.num;
	      di->kv.eof = 0;
	      di->dp = data_read_id(di->dp, &di->subschema);
	      di->subkeyp = di->data->schemadata + di->data->schemata[di->subschema];
	      di->subschema = 0;
	    }
	}
weg2:
      if (!di->match
	  || dataiterator_match_int(di))
	break;
    }
  return 1;
}

void
dataiterator_skip_attribute(Dataiterator *di)
{
  if (di->state)
    di->idp = 0;
  /* This will make the next _step call to retrieve the next field.  */
  di->kv.eof = 1;
}

void
dataiterator_skip_solvable(Dataiterator *di)
{
  /* We're done with this field.  */
  di->kv.eof = 1;
  /* And with solvable data.  */
  di->state = 0;
  /* And with all keys for this repodata and thing. */
  static Id zeroid = 0;
  di->keyp = &zeroid;
  /* And with all repodatas for this thing.  */
  di->data = di->repo->repodata + di->repo->nrepodata - 1;
  /* Hence the next call to _step will retrieve the next thing.  */
}

void
dataiterator_skip_repo(Dataiterator *di)
{
  dataiterator_skip_solvable(di);
  /* We're done with all solvables and all extra things for this repo.  */
  di->solvid = -1;
}

void
dataiterator_jump_to_solvable(Dataiterator *di, Solvable *s)
{
  di->repo = s->repo;
  /* Simulate us being done with the solvable before the requested one.  */
  dataiterator_skip_solvable(di);
  di->solvid = s - s->repo->pool->solvables;
  di->solvid--;
}

void
dataiterator_jump_to_repo(Dataiterator *di, Repo *repo)
{
  di->repo = repo;
  dataiterator_skip_solvable(di);
  di->solvid = repo->start - 1;
}

#endif

/************************************************************************
 * data modify functions
 */

/* extend repodata so that it includes solvables p */
void
repodata_extend(Repodata *data, Id p)
{
  if (data->start == data->end)
    data->start = data->end = p;
  if (p >= data->end)
    {
      int old = data->end - data->start;
      int new = p - data->end + 1;
      if (data->attrs)
	{
	  data->attrs = sat_extend(data->attrs, old, new, sizeof(Id), REPODATA_BLOCK);
	  memset(data->attrs + old, 0, new * sizeof(Id));
	}
      data->incoreoffset = sat_extend(data->incoreoffset, old, new, sizeof(Id), REPODATA_BLOCK);
      memset(data->incoreoffset + old, 0, new * sizeof(Id));
      data->end = p + 1;
    }
  if (p < data->start)
    {
      int old = data->end - data->start;
      int new = data->start - p;
      if (data->attrs)
	{
	  data->attrs = sat_extend_resize(data->attrs, old + new, sizeof(Id), REPODATA_BLOCK);
	  memmove(data->attrs + new, data->attrs, old * sizeof(Id));
	  memset(data->attrs, 0, new * sizeof(Id));
	}
      data->incoreoffset = sat_extend_resize(data->incoreoffset, old + new, sizeof(Id), REPODATA_BLOCK);
      memmove(data->incoreoffset + new, data->incoreoffset, old * sizeof(Id));
      memset(data->incoreoffset, 0, new * sizeof(Id));
      data->start = p;
    }
}

void
repodata_extend_block(Repodata *data, Id start, Id num)
{
  if (!num)
    return;
  if (!data->incoreoffset)
    {
      data->incoreoffset = sat_calloc_block(num, sizeof(Id), REPODATA_BLOCK);
      data->start = start;
      data->end = start + num;
      return;
    }
  repodata_extend(data, start);
  if (num > 1)
    repodata_extend(data, start + num - 1);
}

/**********************************************************************/

#define REPODATA_ATTRS_BLOCK 63
#define REPODATA_ATTRDATA_BLOCK 1023
#define REPODATA_ATTRIDDATA_BLOCK 63


Id
repodata_new_handle(Repodata *data)
{
  if (!data->nxattrs)
    {
      data->xattrs = sat_calloc_block(1, sizeof(Id *), REPODATA_BLOCK);
      data->nxattrs = 2;
    }
  data->xattrs = sat_extend(data->xattrs, data->nxattrs, 1, sizeof(Id *), REPODATA_BLOCK);
  data->xattrs[data->nxattrs] = 0;
  return -(data->nxattrs++);
}

static inline Id **
repodata_get_attrp(Repodata *data, Id handle)
{
  if (handle == REPOENTRY_META)
    {
      if (!data->xattrs)
	{
	  data->xattrs = sat_calloc_block(1, sizeof(Id *), REPODATA_BLOCK);
          data->nxattrs = 2;
	}
    }
  if (handle < 0)
    return data->xattrs - handle;
  if (handle < data->start || handle >= data->end)
    repodata_extend(data, handle);
  if (!data->attrs)
    data->attrs = sat_calloc_block(data->end - data->start, sizeof(Id *), REPODATA_BLOCK);
  return data->attrs + (handle - data->start);
}

static void
repodata_insert_keyid(Repodata *data, Id handle, Id keyid, Id val, int overwrite)
{
  Id *pp;
  Id *ap, **app;
  int i;

  app = repodata_get_attrp(data, handle);
  ap = *app;
  i = 0;
  if (ap)
    {
      for (pp = ap; *pp; pp += 2)
	/* Determine equality based on the name only, allows us to change
	   type (when overwrite is set), and makes TYPE_CONSTANT work.  */
        if (data->keys[*pp].name == data->keys[keyid].name)
          break;
      if (*pp)
        {
	  if (overwrite)
	    {
	      pp[0] = keyid;
              pp[1] = val;
	    }
          return;
        }
      i = pp - ap;
    }
  ap = sat_extend(ap, i, 3, sizeof(Id), REPODATA_ATTRS_BLOCK);
  *app = ap;
  pp = ap + i;
  *pp++ = keyid;
  *pp++ = val;
  *pp = 0;
}


void
repodata_set(Repodata *data, Id handle, Repokey *key, Id val)
{
  Id keyid;

  keyid = repodata_key2id(data, key, 1);
  repodata_insert_keyid(data, handle, keyid, val, 1);
}

void
repodata_set_id(Repodata *data, Id handle, Id keyname, Id id)
{
  Repokey key;
  key.name = keyname;
  key.type = REPOKEY_TYPE_ID;
  key.size = 0;
  key.storage = KEY_STORAGE_INCORE;
  repodata_set(data, handle, &key, id);
}

void
repodata_set_num(Repodata *data, Id handle, Id keyname, unsigned int num)
{
  Repokey key;
  key.name = keyname;
  key.type = REPOKEY_TYPE_NUM;
  key.size = 0;
  key.storage = KEY_STORAGE_INCORE;
  repodata_set(data, handle, &key, (Id)num);
}

void
repodata_set_poolstr(Repodata *data, Id handle, Id keyname, const char *str)
{
  Repokey key;
  Id id;
  if (data->localpool)
    id = stringpool_str2id(&data->spool, str, 1);
  else
    id = str2id(data->repo->pool, str, 1);
  key.name = keyname;
  key.type = REPOKEY_TYPE_ID;
  key.size = 0;
  key.storage = KEY_STORAGE_INCORE;
  repodata_set(data, handle, &key, id);
}

void
repodata_set_constant(Repodata *data, Id handle, Id keyname, unsigned int constant)
{
  Repokey key;
  key.name = keyname;
  key.type = REPOKEY_TYPE_CONSTANT;
  key.size = constant;
  key.storage = KEY_STORAGE_INCORE;
  repodata_set(data, handle, &key, 0);
}

void
repodata_set_constantid(Repodata *data, Id handle, Id keyname, Id id)
{
  Repokey key;
  key.name = keyname;
  key.type = REPOKEY_TYPE_CONSTANTID;
  key.size = id;
  key.storage = KEY_STORAGE_INCORE;
  repodata_set(data, handle, &key, 0);
}

void
repodata_set_void(Repodata *data, Id handle, Id keyname)
{
  Repokey key;
  key.name = keyname;
  key.type = REPOKEY_TYPE_VOID;
  key.size = 0;
  key.storage = KEY_STORAGE_INCORE;
  repodata_set(data, handle, &key, 0);
}

void
repodata_set_str(Repodata *data, Id handle, Id keyname, const char *str)
{
  Repokey key;
  int l;

  l = strlen(str) + 1;
  key.name = keyname;
  key.type = REPOKEY_TYPE_STR;
  key.size = 0;
  key.storage = KEY_STORAGE_INCORE;
  data->attrdata = sat_extend(data->attrdata, data->attrdatalen, l, 1, REPODATA_ATTRDATA_BLOCK);
  memcpy(data->attrdata + data->attrdatalen, str, l);
  repodata_set(data, handle, &key, data->attrdatalen);
  data->attrdatalen += l;
}

static void
repodata_add_array(Repodata *data, Id handle, Id keyname, Id keytype, int entrysize)
{
  int oldsize;
  Id *ida, *pp, **ppp;

  if (handle == data->lasthandle && data->keys[data->lastkey].name == keyname && data->keys[data->lastkey].type == keytype && data->attriddatalen == data->lastdatalen)
    {
      /* great! just append the new data */
      data->attriddata = sat_extend(data->attriddata, data->attriddatalen, entrysize, sizeof(Id), REPODATA_ATTRIDDATA_BLOCK);
      data->attriddatalen--;	/* overwrite terminating 0  */
      data->lastdatalen += entrysize;
      return;
    }
  ppp = repodata_get_attrp(data, handle);
  pp = *ppp;
  if (pp)
    for (; *pp; pp += 2)
      if (data->keys[*pp].name == keyname && data->keys[*pp].type == keytype)
        break;
  if (!pp || !*pp)
    {
      /* not found. allocate new key */
      Repokey key;
      key.name = keyname;
      key.type = keytype;
      key.size = 0;
      key.storage = KEY_STORAGE_INCORE;
      data->attriddata = sat_extend(data->attriddata, data->attriddatalen, entrysize + 1, sizeof(Id), REPODATA_ATTRIDDATA_BLOCK);
      repodata_set(data, handle, &key, data->attriddatalen);
      data->lasthandle = 0;	/* next time... */
      return;
    }
  oldsize = 0;
  for (ida = data->attriddata + pp[1]; *ida; ida += entrysize)
    oldsize += entrysize;
  if (ida + 1 == data->attriddata + data->attriddatalen)
    {
      /* this was the last entry, just append it */
      data->attriddata = sat_extend(data->attriddata, data->attriddatalen, entrysize, sizeof(Id), REPODATA_ATTRIDDATA_BLOCK);
      data->attriddatalen--;	/* overwrite terminating 0  */
    }
  else
    {
      /* too bad. move to back. */
      data->attriddata = sat_extend(data->attriddata, data->attriddatalen,  oldsize + entrysize + 1, sizeof(Id), REPODATA_ATTRIDDATA_BLOCK);
      memcpy(data->attriddata + data->attriddatalen, data->attriddata + pp[1], oldsize * sizeof(Id));
      pp[1] = data->attriddatalen;
      data->attriddatalen += oldsize;
    }
  data->lasthandle = handle;
  data->lastkey = *pp;
  data->lastdatalen = data->attriddatalen + entrysize + 1;
}

static inline int
checksumtype2len(Id type)
{
  switch (type)
    {
    case REPOKEY_TYPE_MD5:
      return SIZEOF_MD5;
    case REPOKEY_TYPE_SHA1:
      return SIZEOF_SHA1;
    case REPOKEY_TYPE_SHA256:
      return SIZEOF_SHA256;
    default:
      return 0;
    }
}

void
repodata_set_bin_checksum(Repodata *data, Id handle, Id keyname, Id type,
		      const unsigned char *str)
{
  Repokey key;
  int l = checksumtype2len(type);

  if (!l)
    return;
  key.name = keyname;
  key.type = type;
  key.size = 0;
  key.storage = KEY_STORAGE_INCORE;
  data->attrdata = sat_extend(data->attrdata, data->attrdatalen, l, 1, REPODATA_ATTRDATA_BLOCK);
  memcpy(data->attrdata + data->attrdatalen, str, l);
  repodata_set(data, handle, &key, data->attrdatalen);
  data->attrdatalen += l;
}

static int
hexstr2bytes(unsigned char *buf, const char *str, int buflen)
{
  int i;
  for (i = 0; i < buflen; i++)
    {
#define c2h(c) (((c)>='0' && (c)<='9') ? ((c)-'0')	\
		: ((c)>='a' && (c)<='f') ? ((c)-'a'+10)	\
		: ((c)>='A' && (c)<='F') ? ((c)-'A'+10)	\
		: -1)
      int v = c2h(*str);
      str++;
      if (v < 0)
	return 0;
      buf[i] = v;
      v = c2h(*str);
      str++;
      if (v < 0)
	return 0;
      buf[i] = (buf[i] << 4) | v;
#undef c2h
    }
  return buflen;
}

void
repodata_set_checksum(Repodata *data, Id handle, Id keyname, Id type,
		      const char *str)
{
  unsigned char buf[64];
  int l = checksumtype2len(type);

  if (!l)
    return;
  if (hexstr2bytes(buf, str, l) != l)
    {
      fprintf(stderr, "Invalid hex character in '%s'\n", str);
      return;
    }
  repodata_set_bin_checksum(data, handle, keyname, type, buf);
}

const char *
repodata_chk2str(Repodata *data, Id type, const unsigned char *buf)
{
  int i, l;
  char *str, *s;

  l = checksumtype2len(type);
  if (!l)
    return "";
  s = str = pool_alloctmpspace(data->repo->pool, 2 * l + 1);
  for (i = 0; i < l; i++)
    {
      unsigned char v = buf[i];
      unsigned char w = v >> 4;
      *s++ = w >= 10 ? w + ('a' - 10) : w + '0';
      w = v & 15;
      *s++ = w >= 10 ? w + ('a' - 10) : w + '0';
    }
  *s = 0;
  return str;
}

Id
repodata_globalize_id(Repodata *data, Id id)
{ 
  if (!data || !data->localpool)
    return id;
  return str2id(data->repo->pool, stringpool_id2str(&data->spool, id), 1);
}

void
repodata_add_dirnumnum(Repodata *data, Id handle, Id keyname, Id dir, Id num, Id num2)
{
  assert(dir);
#if 0
fprintf(stderr, "repodata_add_dirnumnum %d %d %d %d (%d)\n", handle, dir, num, num2, data->attriddatalen);
#endif
  repodata_add_array(data, handle, keyname, REPOKEY_TYPE_DIRNUMNUMARRAY, 3);
  data->attriddata[data->attriddatalen++] = dir;
  data->attriddata[data->attriddatalen++] = num;
  data->attriddata[data->attriddatalen++] = num2;
  data->attriddata[data->attriddatalen++] = 0;
}

void
repodata_add_dirstr(Repodata *data, Id handle, Id keyname, Id dir, const char *str)
{
  Id stroff;
  int l;

  assert(dir);
  l = strlen(str) + 1;
  data->attrdata = sat_extend(data->attrdata, data->attrdatalen, l, 1, REPODATA_ATTRDATA_BLOCK);
  memcpy(data->attrdata + data->attrdatalen, str, l);
  stroff = data->attrdatalen;
  data->attrdatalen += l;

#if 0
fprintf(stderr, "repodata_add_dirstr %d %d %s (%d)\n", handle, dir, str,  data->attriddatalen);
#endif
  repodata_add_array(data, handle, keyname, REPOKEY_TYPE_DIRSTRARRAY, 2);
  data->attriddata[data->attriddatalen++] = dir;
  data->attriddata[data->attriddatalen++] = stroff;
  data->attriddata[data->attriddatalen++] = 0;
}

void
repodata_add_idarray(Repodata *data, Id handle, Id keyname, Id id)
{
#if 0
fprintf(stderr, "repodata_add_idarray %d %d (%d)\n", handle, id, data->attriddatalen);
#endif
  repodata_add_array(data, handle, keyname, REPOKEY_TYPE_IDARRAY, 1);
  data->attriddata[data->attriddatalen++] = id;
  data->attriddata[data->attriddatalen++] = 0;
}

void
repodata_add_poolstr_array(Repodata *data, Id handle, Id keyname,
			   const char *str)
{
  Id id;
  if (data->localpool)
    id = stringpool_str2id(&data->spool, str, 1);
  else
    id = str2id(data->repo->pool, str, 1);
  repodata_add_idarray(data, handle, keyname, id);
}

void
repodata_add_fixarray(Repodata *data, Id handle, Id keyname, Id ghandle)
{
  repodata_add_array(data, handle, keyname, REPOKEY_TYPE_FIXARRAY, 1);
  data->attriddata[data->attriddatalen++] = ghandle;
  data->attriddata[data->attriddatalen++] = 0;
}

void
repodata_add_flexarray(Repodata *data, Id handle, Id keyname, Id ghandle)
{
  repodata_add_array(data, handle, keyname, REPOKEY_TYPE_FLEXARRAY, 1);
  data->attriddata[data->attriddatalen++] = ghandle;
  data->attriddata[data->attriddatalen++] = 0;
}

void
repodata_merge_attrs(Repodata *data, Id dest, Id src)
{
  Id *keyp;
  if (dest == src || !(keyp = data->attrs[src]))
    return;
  for (; *keyp; keyp += 2)
    repodata_insert_keyid(data, dest, keyp[0], keyp[1], 0);
}




/**********************************************************************/

/* unify with repo_write! */

#define EXTDATA_BLOCK 1023

struct extdata {
  unsigned char *buf;
  int len;
};

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

/*********************************/

static void
repodata_serialize_key(Repodata *data, struct extdata *newincore,
		       struct extdata *newvincore,
		       Id *schema,
		       Repokey *key, Id val)
{
  /* Otherwise we have a new value.  Parse it into the internal
     form.  */
  Id *ida;
  struct extdata *xd;
  unsigned int oldvincorelen = 0;
  Id schemaid, *sp;

  xd = newincore;
  if (key->storage == KEY_STORAGE_VERTICAL_OFFSET)
    {
      xd = newvincore;
      oldvincorelen = xd->len;
    }
  switch (key->type)
    {
    case REPOKEY_TYPE_VOID:
    case REPOKEY_TYPE_CONSTANT:
    case REPOKEY_TYPE_CONSTANTID:
      break;
    case REPOKEY_TYPE_STR:
      data_addblob(xd, data->attrdata + val, strlen((char *)(data->attrdata + val)) + 1);
      break;
    case REPOKEY_TYPE_MD5:
      data_addblob(xd, data->attrdata + val, SIZEOF_MD5);
      break;
    case REPOKEY_TYPE_SHA1:
      data_addblob(xd, data->attrdata + val, SIZEOF_SHA1);
      break;
    case REPOKEY_TYPE_SHA256:
      data_addblob(xd, data->attrdata + val, SIZEOF_SHA256);
      break;
    case REPOKEY_TYPE_ID:
    case REPOKEY_TYPE_NUM:
    case REPOKEY_TYPE_DIR:
      data_addid(xd, val);
      break;
    case REPOKEY_TYPE_IDARRAY:
      for (ida = data->attriddata + val; *ida; ida++)
	data_addideof(xd, ida[0], ida[1] ? 0 : 1);
      break;
    case REPOKEY_TYPE_DIRNUMNUMARRAY:
      for (ida = data->attriddata + val; *ida; ida += 3)
	{
	  data_addid(xd, ida[0]);
	  data_addid(xd, ida[1]);
	  data_addideof(xd, ida[2], ida[3] ? 0 : 1);
	}
      break;
    case REPOKEY_TYPE_DIRSTRARRAY:
      for (ida = data->attriddata + val; *ida; ida += 2)
	{
	  data_addideof(xd, ida[0], ida[2] ? 0 : 1);
	  data_addblob(xd, data->attrdata + ida[1], strlen((char *)(data->attrdata + ida[1])) + 1);
	}
      break;
    case REPOKEY_TYPE_FIXARRAY:
      {
	int num = 0;
	schemaid = 0;
	for (ida = data->attriddata + val; *ida; ida++)
	  {
#if 0
	    fprintf(stderr, "serialize struct %d\n", *ida);
#endif
	    sp = schema;
	    Id *kp = data->xattrs[-*ida];
	    if (!kp)
	      continue;
	    num++;
	    for (;*kp; kp += 2)
	      {
#if 0
		fprintf(stderr, "  %s:%d\n", id2str(data->repo->pool, data->keys[*kp].name), kp[1]);
#endif
		*sp++ = *kp;
	      }
	    *sp = 0;
	    if (!schemaid)
	      schemaid = repodata_schema2id(data, schema, 1);
	    else if (schemaid != repodata_schema2id(data, schema, 0))
	      {
		fprintf(stderr, "  not yet implemented: substructs with different schemas\n");
		exit(1);
	      }
#if 0
	    fprintf(stderr, "  schema %d\n", schemaid);
#endif
	  }
	if (!num)
	  break;
	data_addid(xd, num);
	data_addid(xd, schemaid);
	for (ida = data->attriddata + val; *ida; ida++)
	  {
	    Id *kp = data->xattrs[-*ida];
	    if (!kp)
	      continue;
	    for (;*kp; kp += 2)
	      {
		repodata_serialize_key(data, newincore, newvincore,
				       schema, data->keys + *kp, kp[1]);
	      }
	  }
	break;
      }
    case REPOKEY_TYPE_FLEXARRAY:
      {
	int num = 0;
	for (ida = data->attriddata + val; *ida; ida++)
	  num++;
	data_addid(xd, num);
	for (ida = data->attriddata + val; *ida; ida++)
	  {
	    Id *kp = data->xattrs[-*ida];
	    if (!kp)
	      {
	        data_addid(xd, 0);	/* XXX */
	        continue;
	      }
	    sp = schema;
	    for (;*kp; kp += 2)
	      *sp++ = *kp;
	    *sp = 0;
	    schemaid = repodata_schema2id(data, schema, 1);
	    data_addid(xd, schemaid);
	    kp = data->xattrs[-*ida];
	    for (;*kp; kp += 2)
	      {
		repodata_serialize_key(data, newincore, newvincore,
				       schema, data->keys + *kp, kp[1]);
	      }
	  }
	break;
      }
    default:
      fprintf(stderr, "don't know how to handle type %d\n", key->type);
      exit(1);
    }
  if (key->storage == KEY_STORAGE_VERTICAL_OFFSET)
    {
      /* put offset/len in incore */
      data_addid(newincore, data->lastverticaloffset + oldvincorelen);
      oldvincorelen = xd->len - oldvincorelen;
      data_addid(newincore, oldvincorelen);
    }
}

void
repodata_internalize(Repodata *data)
{
  Repokey *key, solvkey;
  Id entry, nentry;
  Id schemaid, *schema, *sp, oldschema, *keyp, *keypstart, *seen;
  unsigned char *dp, *ndp;
  int newschema, oldcount;
  struct extdata newincore;
  struct extdata newvincore;
  Id solvkeyid;

  if (!data->attrs && !data->xattrs)
    return;

  newvincore.buf = data->vincore;
  newvincore.len = data->vincorelen;

  /* find the solvables key, create if needed */
  memset(&solvkey, 0, sizeof(solvkey));
  solvkey.name = REPOSITORY_SOLVABLES;
  solvkey.type = REPOKEY_TYPE_FLEXARRAY;
  solvkey.size = 0;
  solvkey.storage = KEY_STORAGE_INCORE;
  solvkeyid = repodata_key2id(data, &solvkey, data->end != data->start ? 1 : 0);

  schema = sat_malloc2(data->nkeys, sizeof(Id));
  seen = sat_malloc2(data->nkeys, sizeof(Id));

  /* Merge the data already existing (in data->schemata, ->incoredata and
     friends) with the new attributes in data->attrs[].  */
  nentry = data->end - data->start;
  memset(&newincore, 0, sizeof(newincore));
  data_addid(&newincore, 0);	/* start data at offset 1 */

  data->mainschema = 0;
  data->mainschemaoffsets = sat_free(data->mainschemaoffsets);

  /* join entry data */
  /* we start with the meta data, entry -1 */
  for (entry = -1; entry < nentry; entry++)
    {
      memset(seen, 0, data->nkeys * sizeof(Id));
      oldschema = 0;
      dp = data->incoredata;
      if (dp)
	{
	  dp += entry >= 0 ? data->incoreoffset[entry] : 1;
          dp = data_read_id(dp, &oldschema);
	}
#if 0
fprintf(stderr, "oldschema %d\n", oldschema);
fprintf(stderr, "schemata %d\n", data->schemata[oldschema]);
fprintf(stderr, "schemadata %p\n", data->schemadata);
#endif
      /* seen: -1: old data  0: skipped  >0: id + 1 */
      newschema = 0;
      oldcount = 0;
      sp = schema;
      for (keyp = data->schemadata + data->schemata[oldschema]; *keyp; keyp++)
	{
	  if (seen[*keyp])
	    {
	      fprintf(stderr, "Inconsistent old data (key occured twice).\n");
	      exit(1);
	    }
	  seen[*keyp] = -1;
	  *sp++ = *keyp;
	  oldcount++;
	}
      if (entry >= 0)
	keyp = data->attrs ? data->attrs[entry] : 0;
      else
	{
	  /* strip solvables key */
	  *sp = 0;
	  for (sp = keyp = schema; *sp; sp++)
	    if (*sp != solvkeyid)
	      *keyp++ = *sp;
	    else
	      oldcount--;
	  sp = keyp;
	  seen[solvkeyid] = 0;
	  keyp = data->xattrs ? data->xattrs[1] : 0;
	}
      if (keyp)
        for (; *keyp; keyp += 2)
	  {
	    if (!seen[*keyp])
	      {
	        newschema = 1;
	        *sp++ = *keyp;
	      }
	    seen[*keyp] = keyp[1] + 1;
	  }
      if (entry < 0 && data->end != data->start)
	{
	  *sp++ = solvkeyid;
	  newschema = 1;
	}
      *sp = 0;
      if (newschema)
        /* Ideally we'd like to sort the new schema here, to ensure
	   schema equality independend of the ordering.  We can't do that
	   yet.  For once see below (old ids need to come before new ids).
	   An additional difficulty is that we also need to move
	   the values with the keys.  */
	schemaid = repodata_schema2id(data, schema, 1);
      else
	schemaid = oldschema;


      /* Now create data blob.  We walk through the (possibly new) schema
	 and either copy over old data, or insert the new.  */
      /* XXX Here we rely on the fact that the (new) schema has the form
	 o1 o2 o3 o4 ... | n1 n2 n3 ...
	 (oX being the old keyids (possibly overwritten), and nX being
	  the new keyids).  This rules out sorting the keyids in order
	 to ensure a small schema count.  */
      if (entry >= 0)
        data->incoreoffset[entry] = newincore.len;
      data_addid(&newincore, schemaid);
      if (entry == -1)
	{
	  data->mainschema = schemaid;
	  data->mainschemaoffsets = sat_calloc(sp - schema, sizeof(Id));
	}
      keypstart = data->schemadata + data->schemata[schemaid];
      for (keyp = keypstart; *keyp; keyp++)
	{
	  if (entry == -1)
	    data->mainschemaoffsets[keyp - keypstart] = newincore.len;
	  if (*keyp == solvkeyid)
	    {
	      /* add flexarray entry count */
	      data_addid(&newincore, data->end - data->start);
	      break;
	    }
	  key = data->keys + *keyp;
#if 0
	  fprintf(stderr, "internalize %d:%s:%s\n", entry, id2str(data->repo->pool, key->name), id2str(data->repo->pool, key->type));
#endif
	  ndp = dp;
	  if (oldcount)
	    {
	      /* Skip the data associated with this old key.  */
	      if (key->storage == KEY_STORAGE_VERTICAL_OFFSET)
		{
		  ndp = data_skip(dp, REPOKEY_TYPE_ID);
		  ndp = data_skip(ndp, REPOKEY_TYPE_ID);
		}
	      else if (key->storage == KEY_STORAGE_INCORE)
		ndp = data_skip_key(data, dp, key);
	      oldcount--;
	    }
	  if (seen[*keyp] == -1)
	    {
	      /* If this key was an old one _and_ was not overwritten with
		 a different value copy over the old value (we skipped it
		 above).  */
	      if (dp != ndp)
		data_addblob(&newincore, dp, ndp - dp);
	      seen[*keyp] = 0;
	    }
	  else if (seen[*keyp])
	    {
	      /* Otherwise we have a new value.  Parse it into the internal
		 form.  */
	      repodata_serialize_key(data, &newincore, &newvincore,
				     schema, key, seen[*keyp] - 1);
	    }
	  dp = ndp;
	}
      if (entry >= 0 && data->attrs && data->attrs[entry])
	data->attrs[entry] = sat_free(data->attrs[entry]);
    }
  /* free all xattrs */
  for (entry = 0; entry < data->nxattrs; entry++)
    if (data->xattrs[entry])
      sat_free(data->xattrs[entry]);
  data->xattrs = sat_free(data->xattrs);
  data->nxattrs = 0;

  data->lasthandle = 0;
  data->lastkey = 0;
  data->lastdatalen = 0;
  sat_free(schema);
  sat_free(seen);
  repodata_free_schemahash(data);

  sat_free(data->incoredata);
  data->incoredata = newincore.buf;
  data->incoredatalen = newincore.len;
  data->incoredatafree = 0;
  
  sat_free(data->vincore);
  data->vincore = newvincore.buf;
  data->vincorelen = newvincore.len;

  data->attrs = sat_free(data->attrs);
  data->attrdata = sat_free(data->attrdata);
  data->attriddata = sat_free(data->attriddata);
  data->attrdatalen = 0;
  data->attriddatalen = 0;
}

void
repodata_disable_paging(Repodata *data)
{
  if (maybe_load_repodata(data, 0)
      && data->num_pages)
    repodata_load_page_range(data, 0, data->num_pages - 1);
}

/*
vim:cinoptions={.5s,g0,p5,t0,(0,^-0.5s,n-0.5s:tw=78:cindent:sw=4:
*/
