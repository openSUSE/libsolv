/*
 * Copyright (c) 2018, SUSE LLC.
 *
 * This program is licensed under the BSD license, read LICENSE.BSD
 * for further information
 */

/*
 * repodata.c
 *
 * Manage data coming from one repository
 *
 * a repository can contain multiple repodata entries, consisting of
 * different sets of keys and different sets of solvables
 */

#define _GNU_SOURCE
#include <string.h>
#include <fnmatch.h>

#include <stdio.h>
#include <stdlib.h>
#include <unistd.h>
#include <assert.h>
#include <regex.h>

#include "repo.h"
#include "pool.h"
#include "poolid_private.h"
#include "util.h"
#include "hash.h"
#include "chksum.h"

#include "repopack.h"
#include "repopage.h"

#ifdef _WIN32
  #include "strfncs.h"
#endif

#define REPODATA_BLOCK 255

static unsigned char *data_skip_key(Repodata *data, unsigned char *dp, Repokey *key);

void
repodata_initdata(Repodata *data, Repo *repo, int localpool)
{
  memset(data, 0, sizeof (*data));
  data->repodataid = data - repo->repodata;
  data->repo = repo;
  data->localpool = localpool;
  if (localpool)
    stringpool_init_empty(&data->spool);
  /* dirpool_init(&data->dirpool);	just zeros out again */
  data->keys = solv_calloc(1, sizeof(Repokey));
  data->nkeys = 1;
  data->schemata = solv_calloc(1, sizeof(Id));
  data->schemadata = solv_calloc(1, sizeof(Id));
  data->nschemata = 1;
  data->schemadatalen = 1;
  repopagestore_init(&data->store);
}

void
repodata_freedata(Repodata *data)
{
  int i;

  solv_free(data->keys);

  solv_free(data->schemata);
  solv_free(data->schemadata);
  solv_free(data->schematahash);

  stringpool_free(&data->spool);
  dirpool_free(&data->dirpool);

  solv_free(data->mainschemaoffsets);
  solv_free(data->incoredata);
  solv_free(data->incoreoffset);
  solv_free(data->verticaloffset);

  repopagestore_free(&data->store);

  solv_free(data->vincore);

  if (data->attrs)
    for (i = 0; i < data->end - data->start; i++)
      solv_free(data->attrs[i]);
  solv_free(data->attrs);
  if (data->xattrs)
    for (i = 0; i < data->nxattrs; i++)
      solv_free(data->xattrs[i]);
  solv_free(data->xattrs);

  solv_free(data->attrdata);
  solv_free(data->attriddata);
  solv_free(data->attrnum64data);

  solv_free(data->dircache);

  repodata_free_filelistfilter(data);
}

void
repodata_free(Repodata *data)
{
  Repo *repo = data->repo;
  int i = data - repo->repodata;
  if (i == 0)
    return;
  repodata_freedata(data);
  if (i < repo->nrepodata - 1)
    {
      /* whoa! this changes the repodataids! */
      memmove(repo->repodata + i, repo->repodata + i + 1, (repo->nrepodata - 1 - i) * sizeof(Repodata));
      for (; i < repo->nrepodata - 1; i++)
	repo->repodata[i].repodataid = i;
    }
  repo->nrepodata--;
  if (repo->nrepodata == 1)
    {
      repo->repodata = solv_free(repo->repodata);
      repo->nrepodata = 0;
    }
}

void
repodata_empty(Repodata *data, int localpool)
{
  void (*loadcallback)(Repodata *) = data->loadcallback;
  int state = data->state;
  repodata_freedata(data);
  repodata_initdata(data, data->repo, localpool);
  data->state = state;
  data->loadcallback = loadcallback;
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
      data->keys = solv_realloc2(data->keys, data->nkeys + 1, sizeof(Repokey));
      data->keys[data->nkeys++] = *key;
      if (data->verticaloffset)
        {
          data->verticaloffset = solv_realloc2(data->verticaloffset, data->nkeys, sizeof(Id));
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

  if (!*schema)
    return 0;	/* XXX: allow empty schema? */
  if ((schematahash = data->schematahash) == 0)
    {
      data->schematahash = schematahash = solv_calloc(256, sizeof(Id));
      for (i = 1; i < data->nschemata; i++)
	{
	  for (sp = data->schemadata + data->schemata[i], h = 0; *sp;)
	    h = h * 7 + *sp++;
	  h &= 255;
	  schematahash[h] = i;
	}
      data->schemadata = solv_extend_resize(data->schemadata, data->schemadatalen, sizeof(Id), SCHEMATADATA_BLOCK);
      data->schemata = solv_extend_resize(data->schemata, data->nschemata, sizeof(Id), SCHEMATA_BLOCK);
    }

  for (sp = schema, len = 0, h = 0; *sp; len++)
    h = h * 7 + *sp++;
  h &= 255;
  len++;

  cid = schematahash[h];
  if (cid)
    {
      if ((data->schemata[cid] + len <= data->schemadatalen) &&
			  !memcmp(data->schemadata + data->schemata[cid], schema, len * sizeof(Id)))
        return cid;
      /* cache conflict, do a slow search */
      for (cid = 1; cid < data->nschemata; cid++)
        if ((data->schemata[cid] + len <= data->schemadatalen) &&
				!memcmp(data->schemadata + data->schemata[cid], schema, len * sizeof(Id)))
          return cid;
    }
  /* a new one */
  if (!create)
    return 0;
  data->schemadata = solv_extend(data->schemadata, data->schemadatalen, len, sizeof(Id), SCHEMATADATA_BLOCK);
  data->schemata = solv_extend(data->schemata, data->nschemata, 1, sizeof(Id), SCHEMATA_BLOCK);
  /* add schema */
  memcpy(data->schemadata + data->schemadatalen, schema, len * sizeof(Id));
  data->schemata[data->nschemata] = data->schemadatalen;
  data->schemadatalen += len;
  schematahash[h] = data->nschemata;
#if 0
fprintf(stderr, "schema2id: new schema\n");
#endif
  return data->nschemata++;
}

void
repodata_free_schemahash(Repodata *data)
{
  data->schematahash = solv_free(data->schematahash);
  /* shrink arrays */
  data->schemata = solv_realloc2(data->schemata, data->nschemata, sizeof(Id));
  data->schemadata = solv_realloc2(data->schemadata, data->schemadatalen, sizeof(Id));
}


/***************************************************************
 * dir pool management
 */

#ifndef HAVE_STRCHRNUL
static inline const char *strchrnul(const char *str, char x)
{
  const char *p = strchr(str, x);
  return p ? p : str + strlen(str);
}
#endif

#define DIRCACHE_SIZE 41	/* < 1k */

#ifdef DIRCACHE_SIZE
struct dircache {
  Id ids[DIRCACHE_SIZE];
  char str[(DIRCACHE_SIZE * (DIRCACHE_SIZE - 1)) / 2];
};
#endif

Id
repodata_str2dir(Repodata *data, const char *dir, int create)
{
  Id id, parent;
#ifdef DIRCACHE_SIZE
  const char *dirs;
#endif
  const char *dire;

  if (!*dir)
    return data->dirpool.ndirs ? 0 : dirpool_add_dir(&data->dirpool, 0, 0, create);
  while (*dir == '/' && dir[1] == '/')
    dir++;
  if (*dir == '/' && !dir[1])
    return data->dirpool.ndirs ? 1 : dirpool_add_dir(&data->dirpool, 0, 1, create);
  parent = 0;
#ifdef DIRCACHE_SIZE
  dirs = dir;
  if (data->dircache)
    {
      int l;
      struct dircache *dircache = data->dircache;
      l = strlen(dir);
      while (l > 0)
	{
	  if (l < DIRCACHE_SIZE && dircache->ids[l] && !memcmp(dircache->str + l * (l - 1) / 2, dir, l))
	    {
	      parent = dircache->ids[l];
	      dir += l;
	      if (!*dir)
		return parent;
	      while (*dir == '/')
		dir++;
	      break;
	    }
	  while (--l)
	    if (dir[l] == '/')
	      break;
	}
    }
#endif
  while (*dir)
    {
      dire = strchrnul(dir, '/');
      if (data->localpool)
        id = stringpool_strn2id(&data->spool, dir, dire - dir, create);
      else
	id = pool_strn2id(data->repo->pool, dir, dire - dir, create);
      if (!id)
	return 0;
      parent = dirpool_add_dir(&data->dirpool, parent, id, create);
      if (!parent)
	return 0;
#ifdef DIRCACHE_SIZE
      if (!data->dircache)
	data->dircache = solv_calloc(1, sizeof(struct dircache));
      if (data->dircache)
	{
	  int l = dire - dirs;
	  if (l < DIRCACHE_SIZE)
	    {
	      data->dircache->ids[l] = parent;
	      memcpy(data->dircache->str + l * (l - 1) / 2, dirs, l);
	    }
	}
#endif
      if (!*dire)
	break;
      dir = dire + 1;
      while (*dir == '/')
	dir++;
    }
  return parent;
}

void
repodata_free_dircache(Repodata *data)
{
  data->dircache = solv_free(data->dircache);
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
  if (did == 1 && !suf)
    return "/";
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
      memcpy(p, comps, l);
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

static unsigned char *
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
  if (data->mainschemaoffsets && dp == data->incoredata + data->mainschemaoffsets[0] && keyp == data->schemadata + data->schemata[data->mainschema])
    {
      int i;
      for (i = 0; (k = *keyp++) != 0; i++)
        if (k == keyid)
	  return data->incoredata + data->mainschemaoffsets[i];
      return 0;
    }
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
  if (len <= 0)
    return 0;
  if (off >= data->lastverticaloffset)
    {
      off -= data->lastverticaloffset;
      if ((unsigned int)off + len > data->vincorelen)
	return 0;
      return data->vincore + off;
    }
  if ((unsigned int)off + len > key->size)
    return 0;
  /* we now have the offset, go into vertical */
  off += data->verticaloffset[key - data->keys];
  /* fprintf(stderr, "key %d page %d\n", key->name, off / REPOPAGE_BLOBSIZE); */
  dp = repopagestore_load_page_range(&data->store, off / REPOPAGE_BLOBSIZE, (off + len - 1) / REPOPAGE_BLOBSIZE);
  data->storestate++;
  if (dp)
    dp += off % REPOPAGE_BLOBSIZE;
  return dp;
}

static inline unsigned char *
get_data(Repodata *data, Repokey *key, unsigned char **dpp, int advance)
{
  unsigned char *dp = *dpp;

  if (!dp)
    return 0;
  if (key->storage == KEY_STORAGE_INCORE)
    {
      if (advance)
        *dpp = data_skip_key(data, dp, key);
      return dp;
    }
  else if (key->storage == KEY_STORAGE_VERTICAL_OFFSET)
    {
      Id off, len;
      dp = data_read_id(dp, &off);
      dp = data_read_id(dp, &len);
      if (advance)
        *dpp = dp;
      return get_vertical_data(data, key, off, len);
    }
  return 0;
}

void
repodata_load(Repodata *data)
{
  if (data->state != REPODATA_STUB)
    return;
  if (data->loadcallback)
    data->loadcallback(data);
  else
    data->state = REPODATA_ERROR;
}

static int
maybe_load_repodata_stub(Repodata *data, Id keyname)
{
  if (data->state != REPODATA_STUB)
    {
      data->state = REPODATA_ERROR;
      return 0;
    }
  if (keyname)
    {
      int i;
      for (i = 1; i < data->nkeys; i++)
	if (keyname == data->keys[i].name)
	  break;
      if (i == data->nkeys)
	return 0;
    }
  repodata_load(data);
  return data->state == REPODATA_AVAILABLE ? 1 : 0;
}

static inline int
maybe_load_repodata(Repodata *data, Id keyname)
{
  if (keyname && !repodata_precheck_keyname(data, keyname))
    return 0;	/* do not bother... */
  if (data->state == REPODATA_AVAILABLE || data->state == REPODATA_LOADING)
    return 1;
  if (data->state == REPODATA_ERROR)
    return 0;
  return maybe_load_repodata_stub(data, keyname);
}

static inline unsigned char *
solvid2data(Repodata *data, Id solvid, Id *schemap)
{
  unsigned char *dp = data->incoredata;
  if (!dp)
    return 0;
  if (solvid == SOLVID_META)
    dp += 1;	/* offset of "meta" solvable */
  else if (solvid == SOLVID_POS)
    {
      Pool *pool = data->repo->pool;
      if (data->repo != pool->pos.repo)
	return 0;
      if (data != data->repo->repodata + pool->pos.repodataid)
	return 0;
      dp += pool->pos.dp;
      if (pool->pos.dp != 1)
        {
          *schemap = pool->pos.schema;
          return dp;
	}
    }
  else
    {
      if (solvid < data->start || solvid >= data->end)
	return 0;
      dp += data->incoreoffset[solvid - data->start];
    }
  return data_read_id(dp, schemap);
}

/************************************************************************
 * data lookup
 */

static unsigned char *
find_key_data(Repodata *data, Id solvid, Id keyname, Repokey **keypp)
{
  unsigned char *dp;
  Id schema, *keyp, *kp;
  Repokey *key;

  if (!maybe_load_repodata(data, keyname))
    return 0;
  dp = solvid2data(data, solvid, &schema);
  if (!dp)
    return 0;
  keyp = data->schemadata + data->schemata[schema];
  for (kp = keyp; *kp; kp++)
    if (data->keys[*kp].name == keyname)
      break;
  if (!*kp)
    return 0;
  *keypp = key = data->keys + *kp;
  if (key->type == REPOKEY_TYPE_DELETED)
    return 0;
  if (key->type == REPOKEY_TYPE_VOID || key->type == REPOKEY_TYPE_CONSTANT || key->type == REPOKEY_TYPE_CONSTANTID)
    return dp;	/* no need to forward... */
  if (key->storage != KEY_STORAGE_INCORE && key->storage != KEY_STORAGE_VERTICAL_OFFSET)
    return 0;	/* get_data will not work, no need to forward */
  dp = forward_to_key(data, *kp, keyp, dp);
  if (!dp)
    return 0;
  return get_data(data, key, &dp, 0);
}

static const Id *
repodata_lookup_schemakeys(Repodata *data, Id solvid)
{
  Id schema;
  if (!maybe_load_repodata(data, 0))
    return 0;
  if (!solvid2data(data, solvid, &schema))
    return 0;
  return data->schemadata + data->schemata[schema];
}

static Id *
alloc_keyskip()
{
  Id *keyskip = solv_calloc(3 + 256, sizeof(Id));
  keyskip[0] = 256; 
  keyskip[1] = keyskip[2] = 1; 
  return keyskip;
}

Id *
repodata_fill_keyskip(Repodata *data, Id solvid, Id *keyskip)
{
  const Id *keyp;
  Id maxkeyname, value;
  keyp = repodata_lookup_schemakeys(data, solvid);
  if (!keyp)
    return keyskip;	/* no keys for this solvid */
  if (!keyskip)
    keyskip = alloc_keyskip();
  maxkeyname = keyskip[0];
  value = keyskip[1] + data->repodataid;
  for (; *keyp; keyp++)
    {
      Id keyname = data->keys[*keyp].name;
      if (keyname >= maxkeyname)
	{
	  int newmax = (keyname | 255) + 1; 
	  keyskip = solv_realloc2(keyskip, 3 + newmax, sizeof(Id));
	  memset(keyskip + (3 + maxkeyname), 0, (newmax - maxkeyname) * sizeof(Id));
	  keyskip[0] = maxkeyname = newmax;
	}
      keyskip[3 + keyname] = value;
    }
  return keyskip;
}

Id
repodata_lookup_type(Repodata *data, Id solvid, Id keyname)
{
  Id schema, *keyp, *kp;
  if (!maybe_load_repodata(data, keyname))
    return 0;
  if (!solvid2data(data, solvid, &schema))
    return 0;
  keyp = data->schemadata + data->schemata[schema];
  for (kp = keyp; *kp; kp++)
    if (data->keys[*kp].name == keyname)
      return data->keys[*kp].type;
  return 0;
}

Id
repodata_lookup_id(Repodata *data, Id solvid, Id keyname)
{
  unsigned char *dp;
  Repokey *key;
  Id id;

  dp = find_key_data(data, solvid, keyname, &key);
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
repodata_lookup_str(Repodata *data, Id solvid, Id keyname)
{
  unsigned char *dp;
  Repokey *key;
  Id id;

  dp = find_key_data(data, solvid, keyname, &key);
  if (!dp)
    return 0;
  if (key->type == REPOKEY_TYPE_STR)
    return (const char *)dp;
  if (key->type == REPOKEY_TYPE_CONSTANTID)
    id = key->size;
  else if (key->type == REPOKEY_TYPE_ID)
    dp = data_read_id(dp, &id);
  else
    return 0;
  if (data->localpool)
    return stringpool_id2str(&data->spool, id);
  return pool_id2str(data->repo->pool, id);
}

unsigned long long
repodata_lookup_num(Repodata *data, Id solvid, Id keyname, unsigned long long notfound)
{
  unsigned char *dp;
  Repokey *key;
  unsigned int high, low;

  dp = find_key_data(data, solvid, keyname, &key);
  if (!dp)
    return notfound;
  switch (key->type)
    {
    case REPOKEY_TYPE_NUM:
      data_read_num64(dp, &low, &high);
      return (unsigned long long)high << 32 | low;
    case REPOKEY_TYPE_CONSTANT:
      return key->size;
    default:
      return notfound;
    }
}

int
repodata_lookup_void(Repodata *data, Id solvid, Id keyname)
{
  return repodata_lookup_type(data, solvid, keyname) == REPOKEY_TYPE_VOID ? 1 : 0;
}

const unsigned char *
repodata_lookup_bin_checksum(Repodata *data, Id solvid, Id keyname, Id *typep)
{
  unsigned char *dp;
  Repokey *key;

  dp = find_key_data(data, solvid, keyname, &key);
  if (!dp)
    return 0;
  switch (key->type)
    {
    case_CHKSUM_TYPES:
      break;
    default:
      return 0;
    }
  *typep = key->type;
  return dp;
}

int
repodata_lookup_idarray(Repodata *data, Id solvid, Id keyname, Queue *q)
{
  unsigned char *dp;
  Repokey *key;
  Id id;
  int eof = 0;

  queue_empty(q);
  dp = find_key_data(data, solvid, keyname, &key);
  if (!dp)
    return 0;
  switch (key->type)
    {
    case REPOKEY_TYPE_CONSTANTID:
      queue_push(q, key->size);
      break;
    case REPOKEY_TYPE_ID:
      dp = data_read_id(dp, &id);
      queue_push(q, id);
      break;
    case REPOKEY_TYPE_IDARRAY:
      for (;;)
	{
	  dp = data_read_ideof(dp, &id, &eof);
	  queue_push(q, id);
	  if (eof)
	    break;
	}
      break;
    default:
      return 0;
    }
  return 1;
}

const void *
repodata_lookup_binary(Repodata *data, Id solvid, Id keyname, int *lenp)
{
  unsigned char *dp;
  Repokey *key;
  Id len;

  dp = find_key_data(data, solvid, keyname, &key);
  if (!dp || key->type != REPOKEY_TYPE_BINARY)
    {
      *lenp = 0;
      return 0;
    }
  dp = data_read_id(dp, &len);
  *lenp = len;
  return dp;
}

unsigned int
repodata_lookup_count(Repodata *data, Id solvid, Id keyname)
{
  unsigned char *dp;
  Repokey *key;
  unsigned int cnt = 0;

  dp = find_key_data(data, solvid, keyname, &key);
  if (!dp)
    return 0;
  switch (key->type)
    {
    case REPOKEY_TYPE_IDARRAY:
    case REPOKEY_TYPE_REL_IDARRAY:
      for (cnt = 1; (*dp & 0xc0) != 0; dp++)
	if ((*dp & 0xc0) == 0x40)
	  cnt++;
      return cnt;
    case REPOKEY_TYPE_FIXARRAY:
    case REPOKEY_TYPE_FLEXARRAY:
      data_read_id(dp, (int *)&cnt);
      return cnt;
    case REPOKEY_TYPE_DIRSTRARRAY:
      for (;;)
	{
	  cnt++;
	  while (*dp & 0x80)
	    dp++;
	  if (!(*dp++ & 0x40))
	    return cnt;
	  dp += strlen((const char *)dp) + 1;
	}
    case REPOKEY_TYPE_DIRNUMNUMARRAY:
      for (;;)
	{
	  cnt++;
	  while (*dp++ & 0x80)
	    ;
	  while (*dp++ & 0x80)
	    ;
	  while (*dp & 0x80)
	    dp++;
	  if (!(*dp++ & 0x40))
	    return cnt;
	}
      default:
	break;
    }
  return 1;
}

/* highly specialized function to speed up fileprovides adding.
 * - repodata must be available
 * - solvid must be >= data->start and < data->end
 * - returns NULL is not found, a "" entry if wrong type
 * - also returns wrong type for REPOKEY_TYPE_DELETED
 */
const unsigned char *
repodata_lookup_packed_dirstrarray(Repodata *data, Id solvid, Id keyname)
{
  static unsigned char wrongtype[2] = { 0x00 /* dir id 0 */, 0 /* "" */ };
  unsigned char *dp;
  Id schema, *keyp, *kp;
  Repokey *key;

  if (!data->incoredata || !data->incoreoffset[solvid - data->start])
    return 0;
  dp = data->incoredata + data->incoreoffset[solvid - data->start];
  dp = data_read_id(dp, &schema);
  keyp = data->schemadata + data->schemata[schema];
  for (kp = keyp; *kp; kp++)
    if (data->keys[*kp].name == keyname)
      break;
  if (!*kp)
    return 0;
  key = data->keys + *kp;
  if (key->type != REPOKEY_TYPE_DIRSTRARRAY)
    return wrongtype;
  dp = forward_to_key(data, *kp, keyp, dp);
  if (key->storage == KEY_STORAGE_INCORE)
    return dp;
  if (key->storage == KEY_STORAGE_VERTICAL_OFFSET && dp)
    {
      Id off, len;
      dp = data_read_id(dp, &off);
      data_read_id(dp, &len);
      return get_vertical_data(data, key, off, len);
    }
  return 0;
}

/* id translation functions */

Id
repodata_globalize_id(Repodata *data, Id id, int create)
{
  if (!id || !data || !data->localpool)
    return id;
  return pool_str2id(data->repo->pool, stringpool_id2str(&data->spool, id), create);
}

Id
repodata_localize_id(Repodata *data, Id id, int create)
{
  if (!id || !data || !data->localpool)
    return id;
  return stringpool_str2id(&data->spool, pool_id2str(data->repo->pool, id), create);
}

Id
repodata_translate_id(Repodata *data, Repodata *fromdata, Id id, int create)
{
  const char *s;
  if (!id || !data || !fromdata)
    return id;
  if (data == fromdata || (!data->localpool && !fromdata->localpool))
    return id;
  if (fromdata->localpool)
    s = stringpool_id2str(&fromdata->spool, id);
  else
    s = pool_id2str(data->repo->pool, id);
  if (data->localpool)
    return stringpool_str2id(&data->spool, s, create);
  else
    return pool_str2id(data->repo->pool, s, create);
}

Id
repodata_translate_dir_slow(Repodata *data, Repodata *fromdata, Id dir, int create, Id *cache)
{
  Id parent, compid;
  if (!dir)
    {
      /* make sure that the dirpool has an entry */
      if (create && !data->dirpool.ndirs)
        dirpool_add_dir(&data->dirpool, 0, 0, create);
      return 0;
    }
  parent = dirpool_parent(&fromdata->dirpool, dir);
  if (parent)
    {
      if (!(parent = repodata_translate_dir(data, fromdata, parent, create, cache)))
	return 0;
    }
  compid = dirpool_compid(&fromdata->dirpool, dir);
  if (compid > 1 && (data->localpool || fromdata->localpool))
    {
      if (!(compid = repodata_translate_id(data, fromdata, compid, create)))
	return 0;
    }
  if (!(compid = dirpool_add_dir(&data->dirpool, parent, compid, create)))
    return 0;
  if (cache)
    {
      cache[(dir & 255) * 2] = dir;
      cache[(dir & 255) * 2 + 1] = compid;
    }
  return compid;
}

/************************************************************************
 * uninternalized lookup / search
 */

static void
data_fetch_uninternalized(Repodata *data, Repokey *key, Id value, KeyValue *kv)
{
  Id *array;
  kv->eof = 1;
  switch (key->type)
    {
    case REPOKEY_TYPE_STR:
      kv->str = (const char *)data->attrdata + value;
      return;
    case REPOKEY_TYPE_CONSTANT:
      kv->num2 = 0;
      kv->num = key->size;
      return;
    case REPOKEY_TYPE_CONSTANTID:
      kv->id = key->size;
      return;
    case REPOKEY_TYPE_NUM:
      kv->num2 = 0;
      kv->num = value;
      if (value & 0x80000000)
	{
	  kv->num = (unsigned int)data->attrnum64data[value ^ 0x80000000];
	  kv->num2 = (unsigned int)(data->attrnum64data[value ^ 0x80000000] >> 32);
	}
      return;
    case_CHKSUM_TYPES:
      kv->num = 0;	/* not stringified */
      kv->str = (const char *)data->attrdata + value;
      return;
    case REPOKEY_TYPE_BINARY:
      kv->str = (const char *)data_read_id(data->attrdata + value, (Id *)&kv->num);
      return;
    case REPOKEY_TYPE_IDARRAY:
      array = data->attriddata + (value + kv->entry);
      kv->id = array[0];
      kv->eof = array[1] ? 0 : 1;
      return;
    case REPOKEY_TYPE_DIRSTRARRAY:
      kv->num = 0;	/* not stringified */
      array = data->attriddata + (value + kv->entry * 2);
      kv->id = array[0];
      kv->str = (const char *)data->attrdata + array[1];
      kv->eof = array[2] ? 0 : 1;
      return;
    case REPOKEY_TYPE_DIRNUMNUMARRAY:
      array = data->attriddata + (value + kv->entry * 3);
      kv->id = array[0];
      kv->num = array[1];
      kv->num2 = array[2];
      kv->eof = array[3] ? 0 : 1;
      return;
    case REPOKEY_TYPE_FIXARRAY:
    case REPOKEY_TYPE_FLEXARRAY:
      array = data->attriddata + (value + kv->entry);
      kv->id = array[0];		/* the handle */
      kv->eof = array[1] ? 0 : 1;
      return;
    default:
      kv->id = value;
      return;
    }
}

Repokey *
repodata_lookup_kv_uninternalized(Repodata *data, Id solvid, Id keyname, KeyValue *kv)
{
  Id *ap;
  if (!data->attrs || solvid < data->start || solvid >= data->end)
    return 0;
  ap = data->attrs[solvid - data->start];
  if (!ap)
    return 0;
  for (; *ap; ap += 2)
    {
      Repokey *key = data->keys + *ap;
      if (key->name != keyname)
	continue;
      data_fetch_uninternalized(data, key, ap[1], kv);
      return key;
    }
  return 0;
}

void
repodata_search_uninternalized(Repodata *data, Id solvid, Id keyname, int flags, int (*callback)(void *cbdata, Solvable *s, Repodata *data, Repokey *key, KeyValue *kv), void *cbdata)
{
  Id *ap;
  int stop;
  Solvable *s;
  KeyValue kv;

  if (!data->attrs || solvid < data->start || solvid >= data->end)
    return;
  ap = data->attrs[solvid - data->start];
  if (!ap)
    return;
  for (; *ap; ap += 2)
    {
      Repokey *key = data->keys + *ap;
      if (keyname && key->name != keyname)
	continue;
      s = solvid > 0 ? data->repo->pool->solvables + solvid : 0;
      kv.entry = 0;
      do
	{
	  data_fetch_uninternalized(data, key, ap[1], &kv);
	  stop = callback(cbdata, s, data, key, &kv);
	  kv.entry++;
	}
      while (!kv.eof && !stop);
      if (keyname || stop > SEARCH_NEXT_KEY)
	return;
    }
}

/************************************************************************
 * data search
 */


const char *
repodata_stringify(Pool *pool, Repodata *data, Repokey *key, KeyValue *kv, int flags)
{
  switch (key->type)
    {
    case REPOKEY_TYPE_ID:
    case REPOKEY_TYPE_CONSTANTID:
    case REPOKEY_TYPE_IDARRAY:
      if (data && data->localpool)
	kv->str = stringpool_id2str(&data->spool, kv->id);
      else
	kv->str = pool_id2str(pool, kv->id);
      if ((flags & SEARCH_SKIP_KIND) != 0 && key->storage == KEY_STORAGE_SOLVABLE && (key->name == SOLVABLE_NAME || key->type == REPOKEY_TYPE_IDARRAY))
	{
	  const char *s;
	  for (s = kv->str; *s >= 'a' && *s <= 'z'; s++)
	    ;
	  if (*s == ':' && s > kv->str)
	    kv->str = s + 1;
	}
      return kv->str;
    case REPOKEY_TYPE_STR:
      return kv->str;
    case REPOKEY_TYPE_DIRSTRARRAY:
      if (!(flags & SEARCH_FILES))
	return kv->str;	/* match just the basename */
      if (kv->num)
	return kv->str;	/* already stringified */
      /* Put the full filename into kv->str.  */
      kv->str = repodata_dir2str(data, kv->id, kv->str);
      kv->num = 1;	/* mark stringification */
      return kv->str;
    case_CHKSUM_TYPES:
      if (!(flags & SEARCH_CHECKSUMS))
	return 0;	/* skip em */
      if (kv->num)
	return kv->str;	/* already stringified */
      kv->str = repodata_chk2str(data, key->type, (const unsigned char *)kv->str);
      kv->num = 1;	/* mark stringification */
      return kv->str;
    default:
      return 0;
    }
}


/* this is an internal hack to pass the parent kv to repodata_search_keyskip */
struct subschema_data {
  void *cbdata;
  Id solvid;
  KeyValue *parent;
};

void
repodata_search_arrayelement(Repodata *data, Id solvid, Id keyname, int flags, KeyValue *kv, int (*callback)(void *cbdata, Solvable *s, Repodata *data, Repokey *key, KeyValue *kv), void *cbdata)
{
  repodata_search_keyskip(data, solvid, keyname, flags | SEARCH_SUBSCHEMA, (Id *)kv, callback, cbdata);
}

static int
repodata_search_array(Repodata *data, Id solvid, Id keyname, int flags, Repokey *key, KeyValue *kv, int (*callback)(void *cbdata, Solvable *s, Repodata *data, Repokey *key, KeyValue *kv), void *cbdata)
{
  Solvable *s = solvid > 0 ? data->repo->pool->solvables + solvid : 0;
  unsigned char *dp = (unsigned char *)kv->str;
  int stop;
  Id schema = 0;

  if (!dp || kv->entry != -1)
    return 0;
  while (++kv->entry < (int)kv->num)
    {
      if (kv->entry)
	dp = data_skip_schema(data, dp, schema);
      if (kv->entry == 0 || key->type == REPOKEY_TYPE_FLEXARRAY)
	dp = data_read_id(dp, &schema);
      kv->id = schema;
      kv->str = (const char *)dp;
      kv->eof = kv->entry == kv->num - 1 ? 1 : 0;
      stop = callback(cbdata, s, data, key, kv);
      if (stop && stop != SEARCH_ENTERSUB)
	return stop;
      if ((flags & SEARCH_SUB) != 0 || stop == SEARCH_ENTERSUB)
        repodata_search_keyskip(data, solvid, keyname, flags | SEARCH_SUBSCHEMA, (Id *)kv, callback, cbdata);
    }
  if ((flags & SEARCH_ARRAYSENTINEL) != 0)
    {
      if (kv->entry)
	dp = data_skip_schema(data, dp, schema);
      kv->id = 0;
      kv->str = (const char *)dp;
      kv->eof = 2;
      return callback(cbdata, s, data, key, kv);
    }
  return 0;
}

/* search a specific repodata */
void
repodata_search_keyskip(Repodata *data, Id solvid, Id keyname, int flags, Id *keyskip, int (*callback)(void *cbdata, Solvable *s, Repodata *data, Repokey *key, KeyValue *kv), void *cbdata)
{
  Id schema;
  Repokey *key;
  Id keyid, *kp, *keyp;
  unsigned char *dp, *ddp;
  int onekey = 0;
  int stop;
  KeyValue kv;
  Solvable *s;

  if (!maybe_load_repodata(data, keyname))
    return;
  if ((flags & SEARCH_SUBSCHEMA) != 0)
    {
      flags ^= SEARCH_SUBSCHEMA;
      kv.parent = (KeyValue *)keyskip;
      keyskip = 0;
      schema = kv.parent->id;
      dp = (unsigned char *)kv.parent->str;
    }
  else
    {
      schema = 0;
      dp = solvid2data(data, solvid, &schema);
      if (!dp)
	return;
      kv.parent = 0;
    }
  s = solvid > 0 ? data->repo->pool->solvables + solvid : 0;
  keyp = data->schemadata + data->schemata[schema];
  if (keyname)
    {
      /* search for a specific key */
      for (kp = keyp; *kp; kp++)
	if (data->keys[*kp].name == keyname)
	  break;
      if (!*kp)
	return;
      dp = forward_to_key(data, *kp, keyp, dp);
      if (!dp)
	return;
      keyp = kp;
      onekey = 1;
    }
  while ((keyid = *keyp++) != 0)
    {
      stop = 0;
      key = data->keys + keyid;
      ddp = get_data(data, key, &dp, *keyp && !onekey ? 1 : 0);

      if (keyskip && (key->name >= keyskip[0] || keyskip[3 + key->name] != keyskip[1] + data->repodataid))
	{
	  if (onekey)
	    return;
	  continue;
	}
      if (key->type == REPOKEY_TYPE_DELETED && !(flags & SEARCH_KEEP_TYPE_DELETED))
	{
	  if (onekey)
	    return;
	  continue;
	}
      if (key->type == REPOKEY_TYPE_FLEXARRAY || key->type == REPOKEY_TYPE_FIXARRAY)
	{
	  kv.entry = -1;
	  ddp = data_read_id(ddp, (Id *)&kv.num);
	  kv.str = (const char *)ddp;
	  stop = repodata_search_array(data, solvid, 0, flags, key, &kv, callback, cbdata);
	  if (onekey || stop > SEARCH_NEXT_KEY)
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
repodata_search(Repodata *data, Id solvid, Id keyname, int flags, int (*callback)(void *cbdata, Solvable *s, Repodata *data, Repokey *key, KeyValue *kv), void *cbdata)
{
  repodata_search_keyskip(data, solvid, keyname, flags, 0, callback, cbdata);
}

void
repodata_setpos_kv(Repodata *data, KeyValue *kv)
{
  Pool *pool = data->repo->pool;
  if (!kv)
    pool_clear_pos(pool);
  else
    {
      pool->pos.repo = data->repo;
      pool->pos.repodataid = data - data->repo->repodata;
      pool->pos.dp = (unsigned char *)kv->str - data->incoredata;
      pool->pos.schema = kv->id;
    }
}

/************************************************************************
 * data iterator functions
 */

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

int
datamatcher_init(Datamatcher *ma, const char *match, int flags)
{
  match = match ? solv_strdup(match) : 0;
  ma->match = match;
  ma->flags = flags;
  ma->error = 0;
  ma->matchdata = 0;
  if ((flags & SEARCH_STRINGMASK) == SEARCH_REGEX)
    {
      ma->matchdata = solv_calloc(1, sizeof(regex_t));
      ma->error = regcomp((regex_t *)ma->matchdata, match, REG_EXTENDED | REG_NOSUB | REG_NEWLINE | ((flags & SEARCH_NOCASE) ? REG_ICASE : 0));
      if (ma->error)
	{
	  solv_free(ma->matchdata);
	  ma->flags = (flags & ~SEARCH_STRINGMASK) | SEARCH_ERROR;
	}
    }
  if ((flags & SEARCH_FILES) != 0 && match)
    {
      /* prepare basename check */
      if ((flags & SEARCH_STRINGMASK) == SEARCH_STRING || (flags & SEARCH_STRINGMASK) == SEARCH_STRINGEND)
	{
	  const char *p = strrchr(match, '/');
	  ma->matchdata = (void *)(p ? p + 1 : match);
	}
      else if ((flags & SEARCH_STRINGMASK) == SEARCH_GLOB)
	{
	  const char *p;
	  for (p = match + strlen(match) - 1; p >= match; p--)
	    if (*p == '[' || *p == ']' || *p == '*' || *p == '?' || *p == '/')
	      break;
	  ma->matchdata = (void *)(p + 1);
	}
    }
  return ma->error;
}

void
datamatcher_free(Datamatcher *ma)
{
  if (ma->match)
    ma->match = solv_free((char *)ma->match);
  if ((ma->flags & SEARCH_STRINGMASK) == SEARCH_REGEX && ma->matchdata)
    {
      regfree(ma->matchdata);
      solv_free(ma->matchdata);
    }
  ma->matchdata = 0;
}

int
datamatcher_match(Datamatcher *ma, const char *str)
{
  int l;
  switch ((ma->flags & SEARCH_STRINGMASK))
    {
    case SEARCH_SUBSTRING:
      if (ma->flags & SEARCH_NOCASE)
	return strcasestr(str, ma->match) != 0;
      else
	return strstr(str, ma->match) != 0;
    case SEARCH_STRING:
      if (ma->flags & SEARCH_NOCASE)
	return !strcasecmp(ma->match, str);
      else
	return !strcmp(ma->match, str);
    case SEARCH_STRINGSTART:
      if (ma->flags & SEARCH_NOCASE)
        return !strncasecmp(ma->match, str, strlen(ma->match));
      else
        return !strncmp(ma->match, str, strlen(ma->match));
    case SEARCH_STRINGEND:
      l = strlen(str) - strlen(ma->match);
      if (l < 0)
	return 0;
      if (ma->flags & SEARCH_NOCASE)
	return !strcasecmp(ma->match, str + l);
      else
	return !strcmp(ma->match, str + l);
    case SEARCH_GLOB:
      return !fnmatch(ma->match, str, (ma->flags & SEARCH_NOCASE) ? FNM_CASEFOLD : 0);
    case SEARCH_REGEX:
      return !regexec((const regex_t *)ma->matchdata, str, 0, NULL, 0);
    default:
      return 0;
    }
}

/* check if the matcher can match the provides basename */

int
datamatcher_checkbasename(Datamatcher *ma, const char *basename)
{
  int l;
  const char *match = ma->matchdata;
  if (!match)
    return 1;
  switch (ma->flags & SEARCH_STRINGMASK)
    {
    case SEARCH_STRING:
      break;
    case SEARCH_STRINGEND:
      if (match != ma->match)
	break;		/* had slash, do exact match on basename */
      /* FALLTHROUGH */
    case SEARCH_GLOB:
      /* check if the basename ends with match */
      l = strlen(basename) - strlen(match);
      if (l < 0)
	return 0;
      basename += l;
      break;
    default:
      return 1;	/* maybe matches */
    }
  if ((ma->flags & SEARCH_NOCASE) != 0)
    return !strcasecmp(match, basename);
  else
    return !strcmp(match, basename);
}

enum {
  di_bye,

  di_enterrepo,
  di_entersolvable,
  di_enterrepodata,
  di_enterschema,
  di_enterkey,

  di_nextattr,
  di_nextkey,
  di_nextrepodata,
  di_nextsolvable,
  di_nextrepo,

  di_enterarray,
  di_nextarrayelement,

  di_entersub,
  di_leavesub,

  di_nextsolvablekey,
  di_entersolvablekey,
  di_nextsolvableattr
};

/* see dataiterator.h for documentation */
int
dataiterator_init(Dataiterator *di, Pool *pool, Repo *repo, Id p, Id keyname, const char *match, int flags)
{
  memset(di, 0, sizeof(*di));
  di->pool = pool;
  di->flags = flags & ~SEARCH_THISSOLVID;
  if (!pool || (repo && repo->pool != pool))
    {
      di->state = di_bye;
      return -1;
    }
  if (match)
    {
      int error;
      if ((error = datamatcher_init(&di->matcher, match, flags)) != 0)
	{
	  di->state = di_bye;
	  return error;
	}
    }
  di->keyname = keyname;
  di->keynames[0] = keyname;
  dataiterator_set_search(di, repo, p);
  return 0;
}

void
dataiterator_init_clone(Dataiterator *di, Dataiterator *from)
{
  *di = *from;
  if (di->dupstr)
    {
      if (di->dupstr == di->kv.str)
        di->dupstr = solv_memdup(di->dupstr, di->dupstrn);
      else
	{
	  di->dupstr = 0;
	  di->dupstrn = 0;
	}
    }
  memset(&di->matcher, 0, sizeof(di->matcher));
  if (from->matcher.match)
    datamatcher_init(&di->matcher, from->matcher.match, from->matcher.flags);
  if (di->nparents)
    {
      /* fix pointers */
      int i;
      for (i = 1; i < di->nparents; i++)
	di->parents[i].kv.parent = &di->parents[i - 1].kv;
      di->kv.parent = &di->parents[di->nparents - 1].kv;
    }
  if (di->oldkeyskip)
    di->oldkeyskip = solv_memdup2(di->oldkeyskip, 3 + di->oldkeyskip[0], sizeof(Id));
  if (di->keyskip)
    di->keyskip = di->oldkeyskip;
}

int
dataiterator_set_match(Dataiterator *di, const char *match, int flags)
{
  di->flags = (flags & ~SEARCH_THISSOLVID) | (di->flags & SEARCH_THISSOLVID);
  datamatcher_free(&di->matcher);
  memset(&di->matcher, 0, sizeof(di->matcher));
  if (match)
    {
      int error;
      if ((error = datamatcher_init(&di->matcher, match, flags)) != 0)
	{
	  di->state = di_bye;
	  return error;
	}
    }
  return 0;
}

void
dataiterator_set_search(Dataiterator *di, Repo *repo, Id p)
{
  di->repo = repo;
  di->repoid = 0;
  di->flags &= ~SEARCH_THISSOLVID;
  di->nparents = 0;
  di->rootlevel = 0;
  di->repodataid = 1;
  if (!di->pool->urepos)
    {
      di->state = di_bye;
      return;
    }
  if (!repo)
    {
      di->repoid = 1;
      di->repo = di->pool->repos[di->repoid];
    }
  di->state = di_enterrepo;
  if (p)
    dataiterator_jump_to_solvid(di, p);
}

void
dataiterator_set_keyname(Dataiterator *di, Id keyname)
{
  di->nkeynames = 0;
  di->keyname = keyname;
  di->keynames[0] = keyname;
}

void
dataiterator_prepend_keyname(Dataiterator *di, Id keyname)
{
  int i;

  if (di->nkeynames >= sizeof(di->keynames)/sizeof(*di->keynames) - 2)
    {
      di->state = di_bye;	/* sorry */
      return;
    }
  for (i = di->nkeynames + 1; i > 0; i--)
    di->keynames[i] = di->keynames[i - 1];
  di->keynames[0] = di->keyname = keyname;
  di->nkeynames++;
}

void
dataiterator_free(Dataiterator *di)
{
  if (di->matcher.match)
    datamatcher_free(&di->matcher);
  if (di->dupstr)
    solv_free(di->dupstr);
  if (di->oldkeyskip)
    solv_free(di->oldkeyskip);
}

static unsigned char *
dataiterator_find_keyname(Dataiterator *di, Id keyname)
{
  Id *keyp;
  Repokey *keys = di->data->keys, *key;
  unsigned char *dp;

  for (keyp = di->keyp; *keyp; keyp++)
    if (keys[*keyp].name == keyname)
      break;
  if (!*keyp)
    return 0;
  key = keys + *keyp;
  if (key->type == REPOKEY_TYPE_DELETED)
    return 0;
  if (key->storage != KEY_STORAGE_INCORE && key->storage != KEY_STORAGE_VERTICAL_OFFSET)
    return 0;		/* get_data will not work, no need to forward */
  dp = forward_to_key(di->data, *keyp, di->keyp, di->dp);
  if (!dp)
    return 0;
  di->keyp = keyp;
  return dp;
}

int
dataiterator_step(Dataiterator *di)
{
  Id schema;

  if (di->state == di_nextattr && di->key->storage == KEY_STORAGE_VERTICAL_OFFSET && di->vert_ddp && di->vert_storestate != di->data->storestate)
    {
      unsigned int ddpoff = di->ddp - di->vert_ddp;
      di->vert_off += ddpoff;
      di->vert_len -= ddpoff;
      di->ddp = di->vert_ddp = get_vertical_data(di->data, di->key, di->vert_off, di->vert_len);
      di->vert_storestate = di->data->storestate;
      if (!di->ddp)
	di->state = di_nextkey;
    }
  for (;;)
    {
      switch (di->state)
	{
	case di_enterrepo: di_enterrepo:
	  if (!di->repo || (di->repo->disabled && !(di->flags & SEARCH_DISABLED_REPOS)))
	    goto di_nextrepo;
	  if (!(di->flags & SEARCH_THISSOLVID))
	    {
	      di->solvid = di->repo->start - 1;	/* reset solvid iterator */
	      goto di_nextsolvable;
	    }
	  /* FALLTHROUGH */

	case di_entersolvable: di_entersolvable:
	  if (!di->repodataid)
	    goto di_enterrepodata;	/* POS case, repodata is set */
	  if (di->solvid > 0 && !(di->flags & SEARCH_NO_STORAGE_SOLVABLE) && (!di->keyname || (di->keyname >= SOLVABLE_NAME && di->keyname <= RPM_RPMDBID)) && di->nparents - di->rootlevel == di->nkeynames)
	    {
	      extern Repokey repo_solvablekeys[RPM_RPMDBID - SOLVABLE_NAME + 1];
	      di->key = repo_solvablekeys + (di->keyname ? di->keyname - SOLVABLE_NAME : 0);
	      di->data = 0;
	      goto di_entersolvablekey;
	    }

	  if (di->keyname)
	    {
	      di->data = di->keyname == SOLVABLE_FILELIST ? repo_lookup_filelist_repodata(di->repo, di->solvid, &di->matcher) : repo_lookup_repodata_opt(di->repo, di->solvid, di->keyname);
	      if (!di->data)
		goto di_nextsolvable;
	      di->repodataid = di->data - di->repo->repodata;
	      di->keyskip = 0;
	      goto di_enterrepodata;
	    }
	di_leavesolvablekey:
	  di->repodataid = 1;	/* reset repodata iterator */
	  di->keyskip = repo_create_keyskip(di->repo, di->solvid, &di->oldkeyskip);
	  /* FALLTHROUGH */

	case di_enterrepodata: di_enterrepodata:
	  if (di->repodataid)
	    {
	      if (di->repodataid >= di->repo->nrepodata)
		goto di_nextsolvable;
	      di->data = di->repo->repodata + di->repodataid;
	    }
	  if (!maybe_load_repodata(di->data, di->keyname))
	    goto di_nextrepodata;
	  di->dp = solvid2data(di->data, di->solvid, &schema);
	  if (!di->dp)
	    goto di_nextrepodata;
	  if (di->solvid == SOLVID_POS)
	    di->solvid = di->pool->pos.solvid;
	  /* reset key iterator */
	  di->keyp = di->data->schemadata + di->data->schemata[schema];
	  /* FALLTHROUGH */

	case di_enterschema: di_enterschema:
	  if (di->keyname)
	    di->dp = dataiterator_find_keyname(di, di->keyname);
	  if (!di->dp || !*di->keyp)
	    {
	      if (di->kv.parent)
		goto di_leavesub;
	      goto di_nextrepodata;
	    }
	  /* FALLTHROUGH */

	case di_enterkey: di_enterkey:
	  di->kv.entry = -1;
	  di->key = di->data->keys + *di->keyp;
	  if (!di->dp)
	    goto di_nextkey;
	  /* this is get_data() modified to store vert_ data */
	  if (di->key->storage == KEY_STORAGE_VERTICAL_OFFSET)
	    {
	      Id off, len;
	      di->dp = data_read_id(di->dp, &off);
	      di->dp = data_read_id(di->dp, &len);
	      di->vert_ddp = di->ddp = get_vertical_data(di->data, di->key, off, len);
	      di->vert_off = off;
	      di->vert_len = len;
	      di->vert_storestate = di->data->storestate;
	    }
	  else if (di->key->storage == KEY_STORAGE_INCORE)
	    {
	      di->ddp = di->dp;		/* start of data */
	      if (di->keyp[1] && (!di->keyname || (di->flags & SEARCH_SUB) != 0))
		di->dp = data_skip_key(di->data, di->dp, di->key);	/* advance to next key */
	    }
	  else
	    di->ddp = 0;
	  if (!di->ddp)
	    goto di_nextkey;
	  if (di->keyskip && (di->key->name >= di->keyskip[0] || di->keyskip[3 + di->key->name] != di->keyskip[1] + di->data->repodataid))
	    goto di_nextkey;
          if (di->key->type == REPOKEY_TYPE_DELETED && !(di->flags & SEARCH_KEEP_TYPE_DELETED))
	    goto di_nextkey;
	  if (di->key->type == REPOKEY_TYPE_FIXARRAY || di->key->type == REPOKEY_TYPE_FLEXARRAY)
	    goto di_enterarray;
	  if (di->nkeynames && di->nparents - di->rootlevel < di->nkeynames)
	    goto di_nextkey;
	  /* FALLTHROUGH */

	case di_nextattr:
          di->kv.entry++;
	  di->ddp = data_fetch(di->ddp, &di->kv, di->key);
	  di->state = di->kv.eof ? di_nextkey : di_nextattr;
	  break;

	case di_nextkey: di_nextkey:
	  if (!di->keyname && *++di->keyp)
	    goto di_enterkey;
	  if (di->kv.parent)
	    goto di_leavesub;
	  /* FALLTHROUGH */

	case di_nextrepodata: di_nextrepodata:
	  if (!di->keyname && di->repodataid && ++di->repodataid < di->repo->nrepodata)
	      goto di_enterrepodata;
	  /* FALLTHROUGH */

	case di_nextsolvable: di_nextsolvable:
	  if (!(di->flags & SEARCH_THISSOLVID))
	    {
	      if (di->solvid < 0)
		di->solvid = di->repo->start;
	      else
	        di->solvid++;
	      for (; di->solvid < di->repo->end; di->solvid++)
		{
		  if (di->pool->solvables[di->solvid].repo == di->repo)
		    goto di_entersolvable;
		}
	    }
	  /* FALLTHROUGH */

	case di_nextrepo: di_nextrepo:
	  if (di->repoid > 0)
	    {
	      di->repoid++;
	      di->repodataid = 1;
	      if (di->repoid < di->pool->nrepos)
		{
		  di->repo = di->pool->repos[di->repoid];
	          goto di_enterrepo;
		}
	    }
	/* FALLTHROUGH */

	case di_bye: di_bye:
	  di->state = di_bye;
	  return 0;

	case di_enterarray: di_enterarray:
	  if (di->key->name == REPOSITORY_SOLVABLES)
	    goto di_nextkey;
	  di->ddp = data_read_id(di->ddp, (Id *)&di->kv.num);
	  di->kv.eof = 0;
	  di->kv.entry = -1;
	  /* FALLTHROUGH */

	case di_nextarrayelement: di_nextarrayelement:
	  di->kv.entry++;
	  if (di->kv.entry)
	    di->ddp = data_skip_schema(di->data, di->ddp, di->kv.id);
	  if (di->kv.entry == di->kv.num)
	    {
	      if (di->nkeynames && di->nparents - di->rootlevel < di->nkeynames)
		goto di_nextkey;
	      if (!(di->flags & SEARCH_ARRAYSENTINEL))
		goto di_nextkey;
	      di->kv.str = (char *)di->ddp;
	      di->kv.eof = 2;
	      di->state = di_nextkey;
	      break;
	    }
	  if (di->kv.entry == di->kv.num - 1)
	    di->kv.eof = 1;
	  if (di->key->type == REPOKEY_TYPE_FLEXARRAY || !di->kv.entry)
	    di->ddp = data_read_id(di->ddp, &di->kv.id);
	  di->kv.str = (char *)di->ddp;
	  if (di->nkeynames && di->nparents - di->rootlevel < di->nkeynames)
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
	  di->keyname = di->keynames[di->nparents - di->rootlevel];
	  goto di_enterschema;

	case di_leavesub: di_leavesub:
	  if (di->nparents - 1 < di->rootlevel)
	    goto di_bye;
	  di->nparents--;
	  di->dp = di->parents[di->nparents].dp;
	  di->kv = di->parents[di->nparents].kv;
	  di->keyp = di->parents[di->nparents].keyp;
	  di->key = di->data->keys + *di->keyp;
	  di->ddp = (unsigned char *)di->kv.str;
	  di->keyname = di->keynames[di->nparents - di->rootlevel];
	  goto di_nextarrayelement;

        /* special solvable attr handling follows */

	case di_nextsolvablekey: di_nextsolvablekey:
	  if (di->keyname)
	    goto di_nextsolvable;
	  if (di->key->name == RPM_RPMDBID)	/* reached end of list? */
	    goto di_leavesolvablekey;
	  di->key++;
	  /* FALLTHROUGH */

	case di_entersolvablekey: di_entersolvablekey:
	  di->idp = solvabledata_fetch(di->pool->solvables + di->solvid, &di->kv, di->key->name);
	  if (!di->idp || !*di->idp)
	    goto di_nextsolvablekey;
	  if (di->kv.eof)
	    {
	      /* not an array */
	      di->kv.id = *di->idp;
	      di->kv.num = *di->idp;	/* for rpmdbid */
	      di->kv.num2 = 0;		/* for rpmdbid */
	      di->kv.entry = 0;
	      di->state = di_nextsolvablekey;
	      break;
	    }
	  di->kv.entry = -1;
	  /* FALLTHROUGH */

	case di_nextsolvableattr:
	  di->state = di_nextsolvableattr;
	  di->kv.id = *di->idp++;
	  di->kv.entry++;
	  if (!*di->idp)
	    {
	      di->kv.eof = 1;
	      di->state = di_nextsolvablekey;
	    }
	  break;

	}

      /* we have a potential match */
      if (di->matcher.match)
	{
	  const char *str;
	  /* simple pre-check so that we don't need to stringify */
	  if (di->keyname == SOLVABLE_FILELIST && di->key->type == REPOKEY_TYPE_DIRSTRARRAY && (di->matcher.flags & SEARCH_FILES) != 0)
	    if (!datamatcher_checkbasename(&di->matcher, di->kv.str))
	      continue;
	  /* now stringify so that we can do the matching */
	  if (!(str = repodata_stringify(di->pool, di->data, di->key, &di->kv, di->flags)))
	    {
	      if (di->keyname && (di->key->type == REPOKEY_TYPE_FIXARRAY || di->key->type == REPOKEY_TYPE_FLEXARRAY))
		return 1;
	      continue;
	    }
	  if (!datamatcher_match(&di->matcher, str))
	    continue;
	}
      else
	{
	  /* stringify filelist if requested */
	  if (di->keyname == SOLVABLE_FILELIST && di->key->type == REPOKEY_TYPE_DIRSTRARRAY && (di->flags & SEARCH_FILES) != 0)
	    repodata_stringify(di->pool, di->data, di->key, &di->kv, di->flags);
	}
      /* found something! */
      return 1;
    }
}

void
dataiterator_entersub(Dataiterator *di)
{
  if (di->state == di_nextarrayelement)
    di->state = di_entersub;
}

void
dataiterator_setpos(Dataiterator *di)
{
  if (di->kv.eof == 2)
    {
      pool_clear_pos(di->pool);
      return;
    }
  di->pool->pos.solvid = di->solvid;
  di->pool->pos.repo = di->repo;
  di->pool->pos.repodataid = di->data - di->repo->repodata;
  di->pool->pos.schema = di->kv.id;
  di->pool->pos.dp = (unsigned char *)di->kv.str - di->data->incoredata;
}

void
dataiterator_setpos_parent(Dataiterator *di)
{
  if (!di->kv.parent || di->kv.parent->eof == 2)
    {
      pool_clear_pos(di->pool);
      return;
    }
  di->pool->pos.solvid = di->solvid;
  di->pool->pos.repo = di->repo;
  di->pool->pos.repodataid = di->data - di->repo->repodata;
  di->pool->pos.schema = di->kv.parent->id;
  di->pool->pos.dp = (unsigned char *)di->kv.parent->str - di->data->incoredata;
}

/* clones just the position, not the search keys/matcher */
void
dataiterator_clonepos(Dataiterator *di, Dataiterator *from)
{
  di->state = from->state;
  di->flags &= ~SEARCH_THISSOLVID;
  di->flags |= (from->flags & SEARCH_THISSOLVID);
  di->repo = from->repo;
  di->data = from->data;
  di->dp = from->dp;
  di->ddp = from->ddp;
  di->idp = from->idp;
  di->keyp = from->keyp;
  di->key = from->key;
  di->kv = from->kv;
  di->repodataid = from->repodataid;
  di->solvid = from->solvid;
  di->repoid = from->repoid;
  di->rootlevel = from->rootlevel;
  memcpy(di->parents, from->parents, sizeof(from->parents));
  di->nparents = from->nparents;
  if (di->nparents)
    {
      int i;
      for (i = 1; i < di->nparents; i++)
	di->parents[i].kv.parent = &di->parents[i - 1].kv;
      di->kv.parent = &di->parents[di->nparents - 1].kv;
    }
  di->dupstr = 0;
  di->dupstrn = 0;
  if (from->dupstr && from->dupstr == from->kv.str)
    {
      di->dupstrn = from->dupstrn;
      di->dupstr = solv_memdup(from->dupstr, from->dupstrn);
    }
}

void
dataiterator_seek(Dataiterator *di, int whence)
{
  if ((whence & DI_SEEK_STAY) != 0)
    di->rootlevel = di->nparents;
  switch (whence & ~DI_SEEK_STAY)
    {
    case DI_SEEK_CHILD:
      if (di->state != di_nextarrayelement)
	break;
      if ((whence & DI_SEEK_STAY) != 0)
	di->rootlevel = di->nparents + 1;	/* XXX: dangerous! */
      di->state = di_entersub;
      break;
    case DI_SEEK_PARENT:
      if (!di->nparents)
	{
	  di->state = di_bye;
	  break;
	}
      di->nparents--;
      if (di->rootlevel > di->nparents)
	di->rootlevel = di->nparents;
      di->dp = di->parents[di->nparents].dp;
      di->kv = di->parents[di->nparents].kv;
      di->keyp = di->parents[di->nparents].keyp;
      di->key = di->data->keys + *di->keyp;
      di->ddp = (unsigned char *)di->kv.str;
      di->keyname = di->keynames[di->nparents - di->rootlevel];
      di->state = di_nextarrayelement;
      break;
    case DI_SEEK_REWIND:
      if (!di->nparents)
	{
	  di->state = di_bye;
	  break;
	}
      di->dp = (unsigned char *)di->kv.parent->str;
      di->keyp = di->data->schemadata + di->data->schemata[di->kv.parent->id];
      di->state = di_enterschema;
      break;
    default:
      break;
    }
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
  di->nparents = 0;
  di->kv.parent = 0;
  di->rootlevel = 0;
  di->keyname = di->keynames[0];
  di->state = di_nextsolvable;
}

void
dataiterator_skip_repo(Dataiterator *di)
{
  di->nparents = 0;
  di->kv.parent = 0;
  di->rootlevel = 0;
  di->keyname = di->keynames[0];
  di->state = di_nextrepo;
}

void
dataiterator_final_solvable(Dataiterator *di)
{
  di->flags |= SEARCH_THISSOLVID;
  di->repoid = 0;
}

void
dataiterator_final_repo(Dataiterator *di)
{
  di->repoid = 0;
}

void
dataiterator_jump_to_solvid(Dataiterator *di, Id solvid)
{
  di->nparents = 0;
  di->kv.parent = 0;
  di->rootlevel = 0;
  di->keyname = di->keynames[0];
  if (solvid == SOLVID_POS)
    {
      di->repo = di->pool->pos.repo;
      if (!di->repo)
	{
	  di->state = di_bye;
	  return;
	}
      di->repoid = 0;
      if (!di->pool->pos.repodataid && di->pool->pos.solvid == SOLVID_META) {
	solvid = SOLVID_META;		/* META pos hack */
      } else {
        di->data = di->repo->repodata + di->pool->pos.repodataid;
        di->repodataid = 0;
      }
    }
  else if (solvid > 0)
    {
      di->repo = di->pool->solvables[solvid].repo;
      di->repoid = 0;
    }
  if (di->repoid > 0)
    {
      if (!di->pool->urepos)
	{
	  di->state = di_bye;
	  return;
	}
      di->repoid = 1;
      di->repo = di->pool->repos[di->repoid];
    }
  if (solvid != SOLVID_POS)
    di->repodataid = 1;
  di->solvid = solvid;
  if (solvid)
    di->flags |= SEARCH_THISSOLVID;
  di->state = di_enterrepo;
}

void
dataiterator_jump_to_repo(Dataiterator *di, Repo *repo)
{
  di->nparents = 0;
  di->kv.parent = 0;
  di->rootlevel = 0;
  di->repo = repo;
  di->repoid = 0;	/* 0 means stay at repo */
  di->repodataid = 1;
  di->solvid = 0;
  di->flags &= ~SEARCH_THISSOLVID;
  di->state = di_enterrepo;
}

int
dataiterator_match(Dataiterator *di, Datamatcher *ma)
{
  const char *str;
  if (!(str = repodata_stringify(di->pool, di->data, di->key, &di->kv, di->flags)))
    return 0;
  return ma ? datamatcher_match(ma, str) : 1;
}

void
dataiterator_strdup(Dataiterator *di)
{
  int l = -1;

  if (!di->kv.str || di->kv.str == di->dupstr)
    return;
  switch (di->key->type)
    {
    case_CHKSUM_TYPES:
    case REPOKEY_TYPE_DIRSTRARRAY:
      if (di->kv.num)	/* was it stringified into tmp space? */
        l = strlen(di->kv.str) + 1;
      break;
    default:
      break;
    }
  if (l < 0 && di->key->storage == KEY_STORAGE_VERTICAL_OFFSET)
    {
      switch (di->key->type)
	{
	case REPOKEY_TYPE_STR:
	case REPOKEY_TYPE_DIRSTRARRAY:
	  l = strlen(di->kv.str) + 1;
	  break;
	case_CHKSUM_TYPES:
	  l = solv_chksum_len(di->key->type);
	  break;
	case REPOKEY_TYPE_BINARY:
	  l = di->kv.num;
	  break;
	}
    }
  if (l >= 0)
    {
      if (!di->dupstrn || di->dupstrn < l)
	{
	  di->dupstrn = l + 16;
	  di->dupstr = solv_realloc(di->dupstr, di->dupstrn);
	}
      if (l)
        memcpy(di->dupstr, di->kv.str, l);
      di->kv.str = di->dupstr;
    }
}

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
	  data->attrs = solv_extend(data->attrs, old, new, sizeof(Id *), REPODATA_BLOCK);
	  memset(data->attrs + old, 0, new * sizeof(Id *));
	}
      data->incoreoffset = solv_extend(data->incoreoffset, old, new, sizeof(Id), REPODATA_BLOCK);
      memset(data->incoreoffset + old, 0, new * sizeof(Id));
      data->end = p + 1;
    }
  if (p < data->start)
    {
      int old = data->end - data->start;
      int new = data->start - p;
      if (data->attrs)
	{
	  data->attrs = solv_extend_resize(data->attrs, old + new, sizeof(Id *), REPODATA_BLOCK);
	  memmove(data->attrs + new, data->attrs, old * sizeof(Id *));
	  memset(data->attrs, 0, new * sizeof(Id *));
	}
      data->incoreoffset = solv_extend_resize(data->incoreoffset, old + new, sizeof(Id), REPODATA_BLOCK);
      memmove(data->incoreoffset + new, data->incoreoffset, old * sizeof(Id));
      memset(data->incoreoffset, 0, new * sizeof(Id));
      data->start = p;
    }
}

/* shrink end of repodata */
void
repodata_shrink(Repodata *data, int end)
{
  int i;

  if (data->end <= end)
    return;
  if (data->start >= end)
    {
      if (data->attrs)
	{
	  for (i = 0; i < data->end - data->start; i++)
	    solv_free(data->attrs[i]);
          data->attrs = solv_free(data->attrs);
	}
      data->incoreoffset = solv_free(data->incoreoffset);
      data->start = data->end = 0;
      return;
    }
  if (data->attrs)
    {
      for (i = end; i < data->end; i++)
	solv_free(data->attrs[i - data->start]);
      data->attrs = solv_extend_resize(data->attrs, end - data->start, sizeof(Id *), REPODATA_BLOCK);
    }
  if (data->incoreoffset)
    data->incoreoffset = solv_extend_resize(data->incoreoffset, end - data->start, sizeof(Id), REPODATA_BLOCK);
  data->end = end;
}

/* extend repodata so that it includes solvables from start to start + num - 1 */
void
repodata_extend_block(Repodata *data, Id start, Id num)
{
  if (!num)
    return;
  if (!data->incoreoffset)
    {
      /* this also means that data->attrs is NULL */
      data->incoreoffset = solv_calloc_block(num, sizeof(Id), REPODATA_BLOCK);
      data->start = start;
      data->end = start + num;
      return;
    }
  repodata_extend(data, start);
  if (num > 1)
    repodata_extend(data, start + num - 1);
}

/**********************************************************************/


#define REPODATA_ATTRS_BLOCK 31
#define REPODATA_ATTRDATA_BLOCK 1023
#define REPODATA_ATTRIDDATA_BLOCK 63
#define REPODATA_ATTRNUM64DATA_BLOCK 15


Id
repodata_new_handle(Repodata *data)
{
  if (!data->nxattrs)
    {
      data->xattrs = solv_calloc_block(1, sizeof(Id *), REPODATA_BLOCK);
      data->nxattrs = 2;	/* -1: SOLVID_META */
    }
  data->xattrs = solv_extend(data->xattrs, data->nxattrs, 1, sizeof(Id *), REPODATA_BLOCK);
  data->xattrs[data->nxattrs] = 0;
  return -(data->nxattrs++);
}

static inline Id **
repodata_get_attrp(Repodata *data, Id handle)
{
  if (handle < 0)
    {
      if (handle == SOLVID_META && !data->xattrs)
	{
	  data->xattrs = solv_calloc_block(1, sizeof(Id *), REPODATA_BLOCK);
          data->nxattrs = 2;
	}
      return data->xattrs - handle;
    }
  if (handle < data->start || handle >= data->end)
    repodata_extend(data, handle);
  if (!data->attrs)
    data->attrs = solv_calloc_block(data->end - data->start, sizeof(Id *), REPODATA_BLOCK);
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
      /* Determine equality based on the name only, allows us to change
         type (when overwrite is set), and makes TYPE_CONSTANT work.  */
      for (pp = ap; *pp; pp += 2)
        if (data->keys[*pp].name == data->keys[keyid].name)
          break;
      if (*pp)
        {
	  if (overwrite || data->keys[*pp].type == REPOKEY_TYPE_DELETED)
	    {
	      pp[0] = keyid;
              pp[1] = val;
	    }
          return;
        }
      i = pp - ap;
    }
  ap = solv_extend(ap, i, 3, sizeof(Id), REPODATA_ATTRS_BLOCK);
  *app = ap;
  pp = ap + i;
  *pp++ = keyid;
  *pp++ = val;
  *pp = 0;
}


static void
repodata_set(Repodata *data, Id solvid, Repokey *key, Id val)
{
  Id keyid;

  keyid = repodata_key2id(data, key, 1);
  repodata_insert_keyid(data, solvid, keyid, val, 1);
}

void
repodata_set_id(Repodata *data, Id solvid, Id keyname, Id id)
{
  Repokey key;
  key.name = keyname;
  key.type = REPOKEY_TYPE_ID;
  key.size = 0;
  key.storage = KEY_STORAGE_INCORE;
  repodata_set(data, solvid, &key, id);
}

void
repodata_set_num(Repodata *data, Id solvid, Id keyname, unsigned long long num)
{
  Repokey key;
  key.name = keyname;
  key.type = REPOKEY_TYPE_NUM;
  key.size = 0;
  key.storage = KEY_STORAGE_INCORE;
  if (num >= 0x80000000)
    {
      data->attrnum64data = solv_extend(data->attrnum64data, data->attrnum64datalen, 1, sizeof(unsigned long long), REPODATA_ATTRNUM64DATA_BLOCK);
      data->attrnum64data[data->attrnum64datalen] = num;
      num = 0x80000000 | data->attrnum64datalen++;
    }
  repodata_set(data, solvid, &key, (Id)num);
}

void
repodata_set_poolstr(Repodata *data, Id solvid, Id keyname, const char *str)
{
  Repokey key;
  Id id;
  if (data->localpool)
    id = stringpool_str2id(&data->spool, str, 1);
  else
    id = pool_str2id(data->repo->pool, str, 1);
  key.name = keyname;
  key.type = REPOKEY_TYPE_ID;
  key.size = 0;
  key.storage = KEY_STORAGE_INCORE;
  repodata_set(data, solvid, &key, id);
}

void
repodata_set_constant(Repodata *data, Id solvid, Id keyname, unsigned int constant)
{
  Repokey key;
  key.name = keyname;
  key.type = REPOKEY_TYPE_CONSTANT;
  key.size = constant;
  key.storage = KEY_STORAGE_INCORE;
  repodata_set(data, solvid, &key, 0);
}

void
repodata_set_constantid(Repodata *data, Id solvid, Id keyname, Id id)
{
  Repokey key;
  key.name = keyname;
  key.type = REPOKEY_TYPE_CONSTANTID;
  key.size = id;
  key.storage = KEY_STORAGE_INCORE;
  repodata_set(data, solvid, &key, 0);
}

void
repodata_set_void(Repodata *data, Id solvid, Id keyname)
{
  Repokey key;
  key.name = keyname;
  key.type = REPOKEY_TYPE_VOID;
  key.size = 0;
  key.storage = KEY_STORAGE_INCORE;
  repodata_set(data, solvid, &key, 0);
}

void
repodata_set_str(Repodata *data, Id solvid, Id keyname, const char *str)
{
  Repokey key;
  int l;

  l = strlen(str) + 1;
  key.name = keyname;
  key.type = REPOKEY_TYPE_STR;
  key.size = 0;
  key.storage = KEY_STORAGE_INCORE;
  data->attrdata = solv_extend(data->attrdata, data->attrdatalen, l, 1, REPODATA_ATTRDATA_BLOCK);
  memcpy(data->attrdata + data->attrdatalen, str, l);
  repodata_set(data, solvid, &key, data->attrdatalen);
  data->attrdatalen += l;
}

void
repodata_set_binary(Repodata *data, Id solvid, Id keyname, void *buf, int len)
{
  Repokey key;
  unsigned char *dp;

  if (len < 0)
    return;
  key.name = keyname;
  key.type = REPOKEY_TYPE_BINARY;
  key.size = 0;
  key.storage = KEY_STORAGE_INCORE;
  data->attrdata = solv_extend(data->attrdata, data->attrdatalen, len + 5, 1, REPODATA_ATTRDATA_BLOCK);
  dp = data->attrdata + data->attrdatalen;
  if (len >= (1 << 14))
    {
      if (len >= (1 << 28))
        *dp++ = (len >> 28) | 128;
      if (len >= (1 << 21))
        *dp++ = (len >> 21) | 128;
      *dp++ = (len >> 14) | 128;
    }
  if (len >= (1 << 7))
    *dp++ = (len >> 7) | 128;
  *dp++ = len & 127;
  if (len)
    memcpy(dp, buf, len);
  repodata_set(data, solvid, &key, data->attrdatalen);
  data->attrdatalen = dp + len - data->attrdata;
}

/* add an array element consisting of entrysize Ids to the repodata. modifies attriddata
 * so that the caller can append entrysize new elements plus the termination zero there */
static void
repodata_add_array(Repodata *data, Id handle, Id keyname, Id keytype, int entrysize)
{
  int oldsize;
  Id *ida, *pp, **ppp;

  /* check if it is the same as last time, this speeds things up a lot */
  if (handle == data->lasthandle && data->keys[data->lastkey].name == keyname && data->keys[data->lastkey].type == keytype && data->attriddatalen == data->lastdatalen)
    {
      /* great! just append the new data */
      data->attriddata = solv_extend(data->attriddata, data->attriddatalen, entrysize, sizeof(Id), REPODATA_ATTRIDDATA_BLOCK);
      data->attriddatalen--;	/* overwrite terminating 0  */
      data->lastdatalen += entrysize;
      return;
    }

  ppp = repodata_get_attrp(data, handle);
  pp = *ppp;
  if (pp)
    {
      for (; *pp; pp += 2)
        if (data->keys[*pp].name == keyname)
          break;
    }
  if (!pp || !*pp || data->keys[*pp].type != keytype)
    {
      /* not found. allocate new key */
      Repokey key;
      Id keyid;
      key.name = keyname;
      key.type = keytype;
      key.size = 0;
      key.storage = KEY_STORAGE_INCORE;
      data->attriddata = solv_extend(data->attriddata, data->attriddatalen, entrysize + 1, sizeof(Id), REPODATA_ATTRIDDATA_BLOCK);
      keyid = repodata_key2id(data, &key, 1);
      repodata_insert_keyid(data, handle, keyid, data->attriddatalen, 1);
      data->lasthandle = handle;
      data->lastkey = keyid;
      data->lastdatalen = data->attriddatalen + entrysize + 1;
      return;
    }
  oldsize = 0;
  for (ida = data->attriddata + pp[1]; *ida; ida += entrysize)
    oldsize += entrysize;
  if (ida + 1 == data->attriddata + data->attriddatalen)
    {
      /* this was the last entry, just append it */
      data->attriddata = solv_extend(data->attriddata, data->attriddatalen, entrysize, sizeof(Id), REPODATA_ATTRIDDATA_BLOCK);
      data->attriddatalen--;	/* overwrite terminating 0  */
    }
  else
    {
      /* too bad. move to back. */
      data->attriddata = solv_extend(data->attriddata, data->attriddatalen,  oldsize + entrysize + 1, sizeof(Id), REPODATA_ATTRIDDATA_BLOCK);
      memcpy(data->attriddata + data->attriddatalen, data->attriddata + pp[1], oldsize * sizeof(Id));
      pp[1] = data->attriddatalen;
      data->attriddatalen += oldsize;
    }
  data->lasthandle = handle;
  data->lastkey = *pp;
  data->lastdatalen = data->attriddatalen + entrysize + 1;
}

void
repodata_set_bin_checksum(Repodata *data, Id solvid, Id keyname, Id type,
		      const unsigned char *str)
{
  Repokey key;
  int l;

  if (!(l = solv_chksum_len(type)))
    return;
  key.name = keyname;
  key.type = type;
  key.size = 0;
  key.storage = KEY_STORAGE_INCORE;
  data->attrdata = solv_extend(data->attrdata, data->attrdatalen, l, 1, REPODATA_ATTRDATA_BLOCK);
  memcpy(data->attrdata + data->attrdatalen, str, l);
  repodata_set(data, solvid, &key, data->attrdatalen);
  data->attrdatalen += l;
}

void
repodata_set_checksum(Repodata *data, Id solvid, Id keyname, Id type,
		      const char *str)
{
  unsigned char buf[64];
  int l;

  if (!(l = solv_chksum_len(type)))
    return;
  if (l > sizeof(buf) || solv_hex2bin(&str, buf, l) != l)
    return;
  repodata_set_bin_checksum(data, solvid, keyname, type, buf);
}

const char *
repodata_chk2str(Repodata *data, Id type, const unsigned char *buf)
{
  int l;

  if (!(l = solv_chksum_len(type)))
    return "";
  return pool_bin2hex(data->repo->pool, buf, l);
}

/* rpm filenames don't contain the epoch, so strip it */
static inline const char *
evrid2vrstr(Pool *pool, Id evrid)
{
  const char *p, *evr = pool_id2str(pool, evrid);
  if (!evr)
    return evr;
  for (p = evr; *p >= '0' && *p <= '9'; p++)
    ;
  return p != evr && *p == ':' && p[1] ? p + 1 : evr;
}

static inline void
repodata_set_poolstrn(Repodata *data, Id solvid, Id keyname, const char *str, int l)
{
  Id id;
  if (data->localpool)
    id = stringpool_strn2id(&data->spool, str, l, 1);
  else
    id = pool_strn2id(data->repo->pool, str, l, 1);
  repodata_set_id(data, solvid, keyname, id);
}

static inline void
repodata_set_strn(Repodata *data, Id solvid, Id keyname, const char *str, int l)
{
  if (!str[l])
    repodata_set_str(data, solvid, keyname, str);
  else
    {
      char *s = solv_strdup(str);
      s[l] = 0;
      repodata_set_str(data, solvid, keyname, s);
      free(s);
    }
}

void
repodata_set_location(Repodata *data, Id solvid, int medianr, const char *dir, const char *file)
{
  Pool *pool = data->repo->pool;
  Solvable *s;
  const char *str, *fp;
  int l = 0;

  if (medianr)
    repodata_set_constant(data, solvid, SOLVABLE_MEDIANR, medianr);
  if (!dir)
    {
      if ((dir = strrchr(file, '/')) != 0)
	{
          l = dir - file;
	  dir = file;
	  file = dir + l + 1;
	  if (!l)
	    l++;
	}
    }
  else
    l = strlen(dir);
  if (l >= 2 && dir[0] == '.' && dir[1] == '/' && (l == 2 || dir[2] != '/'))
    {
      dir += 2;
      l -= 2;
    }
  if (l == 1 && dir[0] == '.')
    l = 0;
  s = pool->solvables + solvid;
  if (dir && l)
    {
      str = pool_id2str(pool, s->arch);
      if (!strncmp(dir, str, l) && !str[l])
	repodata_set_void(data, solvid, SOLVABLE_MEDIADIR);
      else
	repodata_set_strn(data, solvid, SOLVABLE_MEDIADIR, dir, l);
    }
  fp = file;
  str = pool_id2str(pool, s->name);
  l = strlen(str);
  if ((!l || !strncmp(fp, str, l)) && fp[l] == '-')
    {
      fp += l + 1;
      str = evrid2vrstr(pool, s->evr);
      l = strlen(str);
      if ((!l || !strncmp(fp, str, l)) && fp[l] == '.')
	{
	  fp += l + 1;
	  str = pool_id2str(pool, s->arch);
	  l = strlen(str);
	  if ((!l || !strncmp(fp, str, l)) && !strcmp(fp + l, ".rpm"))
	    {
	      repodata_set_void(data, solvid, SOLVABLE_MEDIAFILE);
	      return;
	    }
	}
    }
  repodata_set_str(data, solvid, SOLVABLE_MEDIAFILE, file);
}

/* XXX: medianr is currently not stored */
void
repodata_set_deltalocation(Repodata *data, Id handle, int medianr, const char *dir, const char *file)
{
  int l = 0;
  const char *evr, *suf, *s;

  if (!dir)
    {
      if ((dir = strrchr(file, '/')) != 0)
	{
          l = dir - file;
	  dir = file;
	  file = dir + l + 1;
	  if (!l)
	    l++;
	}
    }
  else
    l = strlen(dir);
  if (l >= 2 && dir[0] == '.' && dir[1] == '/' && (l == 2 || dir[2] != '/'))
    {
      dir += 2;
      l -= 2;
    }
  if (l == 1 && dir[0] == '.')
    l = 0;
  if (dir && l)
    repodata_set_poolstrn(data, handle, DELTA_LOCATION_DIR, dir, l);
  evr = strchr(file, '-');
  if (evr)
    {
      for (s = evr - 1; s > file; s--)
	if (*s == '-')
	  {
	    evr = s;
	    break;
	  }
    }
  suf = strrchr(file, '.');
  if (suf)
    {
      for (s = suf - 1; s > file; s--)
	if (*s == '.')
	  {
	    suf = s;
	    break;
	  }
      if (!strcmp(suf, ".delta.rpm") || !strcmp(suf, ".patch.rpm"))
	{
	  /* We accept one more item as suffix.  */
	  for (s = suf - 1; s > file; s--)
	    if (*s == '.')
	      {
		suf = s;
	        break;
	      }
	}
    }
  if (!evr)
    suf = 0;
  if (suf && evr && suf < evr)
    suf = 0;
  repodata_set_poolstrn(data, handle, DELTA_LOCATION_NAME, file, evr ? evr - file : strlen(file));
  if (evr)
    repodata_set_poolstrn(data, handle, DELTA_LOCATION_EVR, evr + 1, suf ? suf - evr - 1: strlen(evr + 1));
  if (suf)
    repodata_set_poolstr(data, handle, DELTA_LOCATION_SUFFIX, suf + 1);
}

void
repodata_set_sourcepkg(Repodata *data, Id solvid, const char *sourcepkg)
{
  Pool *pool = data->repo->pool;
  Solvable *s = pool->solvables + solvid;
  const char *p, *sevr, *sarch, *name, *evr;

  p = strrchr(sourcepkg, '.');
  if (!p || strcmp(p, ".rpm") != 0)
    {
      if (*sourcepkg)
        repodata_set_str(data, solvid, SOLVABLE_SOURCENAME, sourcepkg);
      return;
    }
  p--;
  while (p > sourcepkg && *p != '.')
    p--;
  if (*p != '.' || p == sourcepkg)
    return;
  sarch = p-- + 1;
  while (p > sourcepkg && *p != '-')
    p--;
  if (*p != '-' || p == sourcepkg)
    return;
  p--;
  while (p > sourcepkg && *p != '-')
    p--;
  if (*p != '-' || p == sourcepkg)
    return;
  sevr = p + 1;
  pool = s->repo->pool;

  name = pool_id2str(pool, s->name);
  if (name && !strncmp(sourcepkg, name, sevr - sourcepkg - 1) && name[sevr - sourcepkg - 1] == 0)
    repodata_set_void(data, solvid, SOLVABLE_SOURCENAME);
  else
    repodata_set_id(data, solvid, SOLVABLE_SOURCENAME, pool_strn2id(pool, sourcepkg, sevr - sourcepkg - 1, 1));

  evr = evrid2vrstr(pool, s->evr);
  if (evr && !strncmp(sevr, evr, sarch - sevr - 1) && evr[sarch - sevr - 1] == 0)
    repodata_set_void(data, solvid, SOLVABLE_SOURCEEVR);
  else
    repodata_set_id(data, solvid, SOLVABLE_SOURCEEVR, pool_strn2id(pool, sevr, sarch - sevr - 1, 1));

  if (!strcmp(sarch, "src.rpm"))
    repodata_set_constantid(data, solvid, SOLVABLE_SOURCEARCH, ARCH_SRC);
  else if (!strcmp(sarch, "nosrc.rpm"))
    repodata_set_constantid(data, solvid, SOLVABLE_SOURCEARCH, ARCH_NOSRC);
  else
    repodata_set_constantid(data, solvid, SOLVABLE_SOURCEARCH, pool_strn2id(pool, sarch, strlen(sarch) - 4, 1));
}

void
repodata_set_idarray(Repodata *data, Id solvid, Id keyname, Queue *q)
{
  Repokey key;
  int i;

  key.name = keyname;
  key.type = REPOKEY_TYPE_IDARRAY;
  key.size = 0;
  key.storage = KEY_STORAGE_INCORE;
  repodata_set(data, solvid, &key, data->attriddatalen);
  data->attriddata = solv_extend(data->attriddata, data->attriddatalen, q->count + 1, sizeof(Id), REPODATA_ATTRIDDATA_BLOCK);
  for (i = 0; i < q->count; i++)
    data->attriddata[data->attriddatalen++] = q->elements[i];
  data->attriddata[data->attriddatalen++] = 0;
}

void
repodata_add_dirnumnum(Repodata *data, Id solvid, Id keyname, Id dir, Id num, Id num2)
{
  assert(dir);
#if 0
fprintf(stderr, "repodata_add_dirnumnum %d %d %d %d (%d)\n", solvid, dir, num, num2, data->attriddatalen);
#endif
  repodata_add_array(data, solvid, keyname, REPOKEY_TYPE_DIRNUMNUMARRAY, 3);
  data->attriddata[data->attriddatalen++] = dir;
  data->attriddata[data->attriddatalen++] = num;
  data->attriddata[data->attriddatalen++] = num2;
  data->attriddata[data->attriddatalen++] = 0;
}

void
repodata_add_dirstr(Repodata *data, Id solvid, Id keyname, Id dir, const char *str)
{
  Id stroff;
  int l;

  assert(dir);
  l = strlen(str) + 1;
  data->attrdata = solv_extend(data->attrdata, data->attrdatalen, l, 1, REPODATA_ATTRDATA_BLOCK);
  memcpy(data->attrdata + data->attrdatalen, str, l);
  stroff = data->attrdatalen;
  data->attrdatalen += l;

#if 0
fprintf(stderr, "repodata_add_dirstr %d %d %s (%d)\n", solvid, dir, str,  data->attriddatalen);
#endif
  repodata_add_array(data, solvid, keyname, REPOKEY_TYPE_DIRSTRARRAY, 2);
  data->attriddata[data->attriddatalen++] = dir;
  data->attriddata[data->attriddatalen++] = stroff;
  data->attriddata[data->attriddatalen++] = 0;
}

void
repodata_add_idarray(Repodata *data, Id solvid, Id keyname, Id id)
{
#if 0
fprintf(stderr, "repodata_add_idarray %d %d (%d)\n", solvid, id, data->attriddatalen);
#endif
  repodata_add_array(data, solvid, keyname, REPOKEY_TYPE_IDARRAY, 1);
  data->attriddata[data->attriddatalen++] = id;
  data->attriddata[data->attriddatalen++] = 0;
}

void
repodata_add_poolstr_array(Repodata *data, Id solvid, Id keyname,
			   const char *str)
{
  Id id;
  if (data->localpool)
    id = stringpool_str2id(&data->spool, str, 1);
  else
    id = pool_str2id(data->repo->pool, str, 1);
  repodata_add_idarray(data, solvid, keyname, id);
}

void
repodata_add_fixarray(Repodata *data, Id solvid, Id keyname, Id handle)
{
  repodata_add_array(data, solvid, keyname, REPOKEY_TYPE_FIXARRAY, 1);
  data->attriddata[data->attriddatalen++] = handle;
  data->attriddata[data->attriddatalen++] = 0;
}

void
repodata_add_flexarray(Repodata *data, Id solvid, Id keyname, Id handle)
{
  repodata_add_array(data, solvid, keyname, REPOKEY_TYPE_FLEXARRAY, 1);
  data->attriddata[data->attriddatalen++] = handle;
  data->attriddata[data->attriddatalen++] = 0;
}

void
repodata_set_kv(Repodata *data, Id solvid, Id keyname, Id keytype, KeyValue *kv)
{
  switch (keytype)
    {
    case REPOKEY_TYPE_ID:
      repodata_set_id(data, solvid, keyname, kv->id);
      break;
    case REPOKEY_TYPE_CONSTANTID:
      repodata_set_constantid(data, solvid, keyname, kv->id);
      break;
    case REPOKEY_TYPE_IDARRAY:
      repodata_add_idarray(data, solvid, keyname, kv->id);
      break;
    case REPOKEY_TYPE_STR:
      repodata_set_str(data, solvid, keyname, kv->str);
      break;
    case REPOKEY_TYPE_VOID:
      repodata_set_void(data, solvid, keyname);
      break;
    case REPOKEY_TYPE_NUM:
      repodata_set_num(data, solvid, keyname, SOLV_KV_NUM64(kv));
      break;
    case REPOKEY_TYPE_CONSTANT:
      repodata_set_constant(data, solvid, keyname, kv->num);
      break;
    case REPOKEY_TYPE_DIRNUMNUMARRAY:
      if (kv->id)
        repodata_add_dirnumnum(data, solvid, keyname, kv->id, kv->num, kv->num2);
      break;
    case REPOKEY_TYPE_DIRSTRARRAY:
      repodata_add_dirstr(data, solvid, keyname, kv->id, kv->str);
      break;
    case_CHKSUM_TYPES:
      repodata_set_bin_checksum(data, solvid, keyname, keytype, (const unsigned char *)kv->str);
      break;
    default:
      break;
    }
}

void
repodata_unset_uninternalized(Repodata *data, Id solvid, Id keyname)
{
  Id *pp, *ap, **app;
  app = repodata_get_attrp(data, solvid);
  ap = *app;
  if (!ap)
    return;
  if (!keyname)
    {
      *app = 0;		/* delete all attributes */
      return;
    }
  for (; *ap; ap += 2)
    if (data->keys[*ap].name == keyname)
      break;
  if (!*ap)
    return;
  pp = ap;
  ap += 2;
  for (; *ap; ap += 2)
    {
      if (data->keys[*ap].name == keyname)
	continue;
      *pp++ = ap[0];
      *pp++ = ap[1];
    }
  *pp = 0;
}

void
repodata_unset(Repodata *data, Id solvid, Id keyname)
{
  Repokey key;
  key.name = keyname;
  key.type = REPOKEY_TYPE_DELETED;
  key.size = 0;
  key.storage = KEY_STORAGE_INCORE;
  repodata_set(data, solvid, &key, 0);
}

/* add all (uninternalized) attrs from src to dest */
void
repodata_merge_attrs(Repodata *data, Id dest, Id src)
{
  Id *keyp;
  if (dest == src || !data->attrs || !(keyp = data->attrs[src - data->start]))
    return;
  for (; *keyp; keyp += 2)
    repodata_insert_keyid(data, dest, keyp[0], keyp[1], 0);
}

/* add some (uninternalized) attrs from src to dest */
void
repodata_merge_some_attrs(Repodata *data, Id dest, Id src, Map *keyidmap, int overwrite)
{
  Id *keyp;
  if (dest == src || !data->attrs || !(keyp = data->attrs[src - data->start]))
    return;
  for (; *keyp; keyp += 2)
    if (!keyidmap || MAPTST(keyidmap, keyp[0]))
      repodata_insert_keyid(data, dest, keyp[0], keyp[1], overwrite);
}

/* swap (uninternalized) attrs from src and dest */
void
repodata_swap_attrs(Repodata *data, Id dest, Id src)
{
  Id *tmpattrs;
  if (!data->attrs || dest == src)
    return;
  if (dest < data->start || dest >= data->end)
    repodata_extend(data, dest);
  if (src < data->start || src >= data->end)
    repodata_extend(data, src);
  tmpattrs = data->attrs[dest - data->start];
  data->attrs[dest - data->start] = data->attrs[src - data->start];
  data->attrs[src - data->start] = tmpattrs;
  if (data->lasthandle == src || data->lasthandle == dest)
    data->lasthandle = 0;
}


/**********************************************************************/

/* TODO: unify with repo_write and repo_solv! */

#define EXTDATA_BLOCK 1023

struct extdata {
  unsigned char *buf;
  int len;
};

static void
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

static void
data_addid64(struct extdata *xd, unsigned long long x)
{
  if (x >= 0x100000000)
    {
      if ((x >> 35) != 0)
	{
	  data_addid(xd, (Id)(x >> 35));
	  xd->buf[xd->len - 1] |= 128;
	}
      data_addid(xd, (Id)((unsigned int)x | 0x80000000));
      xd->buf[xd->len - 5] = (x >> 28) | 128;
    }
  else
    data_addid(xd, (Id)x);
}

static void
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

static void
data_addblob(struct extdata *xd, unsigned char *blob, int len)
{
  xd->buf = solv_extend(xd->buf, xd->len, len, 1, EXTDATA_BLOCK);
  memcpy(xd->buf + xd->len, blob, len);
  xd->len += len;
}

/*********************************/

/* this is to reduct memory usage when internalizing oversized repos */
static void
compact_attrdata(Repodata *data, int entry, int nentry)
{
  int i;
  unsigned int attrdatastart = data->attrdatalen;
  unsigned int attriddatastart = data->attriddatalen;
  if (attrdatastart < 1024 * 1024 * 4 && attriddatastart < 1024 * 1024)
    return;
  for (i = entry; i < nentry; i++)
    {
      Id v, *attrs = data->attrs[i];
      if (!attrs)
	continue;
      for (; *attrs; attrs += 2)
	{
	  switch (data->keys[*attrs].type)
	    {
	    case REPOKEY_TYPE_STR:
	    case REPOKEY_TYPE_BINARY:
	    case_CHKSUM_TYPES:
	      if ((unsigned int)attrs[1] < attrdatastart)
		 attrdatastart = attrs[1];
	      break;
	    case REPOKEY_TYPE_DIRSTRARRAY:
	      for (v = attrs[1]; data->attriddata[v] ; v += 2)
		if ((unsigned int)data->attriddata[v + 1] < attrdatastart)
		  attrdatastart = data->attriddata[v + 1];
	      /* FALLTHROUGH */
	    case REPOKEY_TYPE_IDARRAY:
	    case REPOKEY_TYPE_DIRNUMNUMARRAY:
	      if ((unsigned int)attrs[1] < attriddatastart)
		attriddatastart = attrs[1];
	      break;
	    case REPOKEY_TYPE_FIXARRAY:
	    case REPOKEY_TYPE_FLEXARRAY:
	      return;
	    default:
	      break;
	    }
	}
    }
#if 0
  printf("compact_attrdata %d %d\n", entry, nentry);
  printf("attrdatastart: %d\n", attrdatastart);
  printf("attriddatastart: %d\n", attriddatastart);
#endif
  if (attrdatastart < 1024 * 1024 * 4 && attriddatastart < 1024 * 1024)
    return;
  for (i = entry; i < nentry; i++)
    {
      Id v, *attrs = data->attrs[i];
      if (!attrs)
	continue;
      for (; *attrs; attrs += 2)
	{
	  switch (data->keys[*attrs].type)
	    {
	    case REPOKEY_TYPE_STR:
	    case REPOKEY_TYPE_BINARY:
	    case_CHKSUM_TYPES:
	      attrs[1] -= attrdatastart;
	      break;
	    case REPOKEY_TYPE_DIRSTRARRAY:
	      for (v = attrs[1]; data->attriddata[v] ; v += 2)
		data->attriddata[v + 1] -= attrdatastart;
	      /* FALLTHROUGH */
	    case REPOKEY_TYPE_IDARRAY:
	    case REPOKEY_TYPE_DIRNUMNUMARRAY:
	      attrs[1] -= attriddatastart;
	      break;
	    default:
	      break;
	    }
	}
    }
  if (attrdatastart)
    {
      data->attrdatalen -= attrdatastart;
      memmove(data->attrdata, data->attrdata + attrdatastart, data->attrdatalen);
      data->attrdata = solv_extend_resize(data->attrdata, data->attrdatalen, 1, REPODATA_ATTRDATA_BLOCK);
    }
  if (attriddatastart)
    {
      data->attriddatalen -= attriddatastart;
      memmove(data->attriddata, data->attriddata + attriddatastart, data->attriddatalen * sizeof(Id));
      data->attriddata = solv_extend_resize(data->attriddata, data->attriddatalen, sizeof(Id), REPODATA_ATTRIDDATA_BLOCK);
    }
}

/* internalalize some key into incore/vincore data */

static void
repodata_serialize_key(Repodata *data, struct extdata *newincore,
		       struct extdata *newvincore,
		       Id *schema,
		       Repokey *key, Id val)
{
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
    case REPOKEY_TYPE_DELETED:
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
    case REPOKEY_TYPE_SHA224:
      data_addblob(xd, data->attrdata + val, SIZEOF_SHA224);
      break;
    case REPOKEY_TYPE_SHA256:
      data_addblob(xd, data->attrdata + val, SIZEOF_SHA256);
      break;
    case REPOKEY_TYPE_SHA384:
      data_addblob(xd, data->attrdata + val, SIZEOF_SHA384);
      break;
    case REPOKEY_TYPE_SHA512:
      data_addblob(xd, data->attrdata + val, SIZEOF_SHA512);
      break;
    case REPOKEY_TYPE_NUM:
      if (val & 0x80000000)
	{
	  data_addid64(xd, data->attrnum64data[val ^ 0x80000000]);
	  break;
	}
      /* FALLTHROUGH */
    case REPOKEY_TYPE_ID:
    case REPOKEY_TYPE_DIR:
      data_addid(xd, val);
      break;
    case REPOKEY_TYPE_BINARY:
      {
	Id len;
	unsigned char *dp = data_read_id(data->attrdata + val, &len);
	dp += (unsigned int)len;
	data_addblob(xd, data->attrdata + val, dp - (data->attrdata + val));
      }
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
	    Id *kp;
	    sp = schema;
	    kp = data->xattrs[-*ida];
	    if (!kp)
	      continue;		/* ignore empty elements */
	    num++;
	    for (; *kp; kp += 2)
	      *sp++ = *kp;
	    *sp = 0;
	    if (!schemaid)
	      schemaid = repodata_schema2id(data, schema, 1);
	    else if (schemaid != repodata_schema2id(data, schema, 0))
	      {
	 	pool_debug(data->repo->pool, SOLV_ERROR, "repodata_serialize_key: fixarray substructs with different schemas\n");
		num = 0;
		break;
	      }
	  }
	data_addid(xd, num);
	if (!num)
	  break;
	data_addid(xd, schemaid);
	for (ida = data->attriddata + val; *ida; ida++)
	  {
	    Id *kp = data->xattrs[-*ida];
	    if (!kp)
	      continue;
	    for (; *kp; kp += 2)
	      repodata_serialize_key(data, newincore, newvincore, schema, data->keys + *kp, kp[1]);
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
	      repodata_serialize_key(data, newincore, newvincore, schema, data->keys + *kp, kp[1]);
	  }
	break;
      }
    default:
      pool_debug(data->repo->pool, SOLV_FATAL, "repodata_serialize_key: don't know how to handle type %d\n", key->type);
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

/* create a circular linked list of all keys that share
 * the same keyname */
static Id *
calculate_keylink(Repodata *data)
{
  int i, j;
  Id *link;
  Id maxkeyname = 0, *keytable = 0;
  link = solv_calloc(data->nkeys, sizeof(Id));
  if (data->nkeys <= 2)
    return link;
  for (i = 1; i < data->nkeys; i++)
    {
      Id n = data->keys[i].name;
      if (n >= maxkeyname)
	{
	  keytable = solv_realloc2(keytable, n + 128, sizeof(Id));
	  memset(keytable + maxkeyname, 0, (n + 128 - maxkeyname) * sizeof(Id));
	  maxkeyname = n + 128;
	}
      j = keytable[n];
      if (j)
	link[i] = link[j];
      else
	j = i;
      link[j] = i;
      keytable[n] = i;
    }
  /* remove links that just point to themselfs */
  for (i = 1; i < data->nkeys; i++)
    if (link[i] == i)
      link[i] = 0;
  solv_free(keytable);
  return link;
}

void
repodata_internalize(Repodata *data)
{
  Repokey *key, solvkey;
  Id entry, nentry;
  Id schemaid, keyid, *schema, *sp, oldschemaid, *keyp, *seen;
  Offset *oldincoreoffs = 0;
  int schemaidx;
  unsigned char *dp, *ndp;
  int neednewschema;
  struct extdata newincore;
  struct extdata newvincore;
  Id solvkeyid;
  Id *keylink;
  int haveoldkl;

  if (!data->attrs && !data->xattrs)
    return;

#if 0
  printf("repodata_internalize %d\n", data->repodataid);
  printf("  attr data: %d K\n", data->attrdatalen / 1024);
  printf("  attrid data: %d K\n", data->attriddatalen / (1024 / 4));
#endif
  newvincore.buf = data->vincore;
  newvincore.len = data->vincorelen;

  /* find the solvables key, create if needed */
  memset(&solvkey, 0, sizeof(solvkey));
  solvkey.name = REPOSITORY_SOLVABLES;
  solvkey.type = REPOKEY_TYPE_FLEXARRAY;
  solvkey.size = 0;
  solvkey.storage = KEY_STORAGE_INCORE;
  solvkeyid = repodata_key2id(data, &solvkey, data->end != data->start ? 1 : 0);

  schema = solv_malloc2(data->nkeys, sizeof(Id));
  seen = solv_malloc2(data->nkeys, sizeof(Id));

  /* Merge the data already existing (in data->schemata, ->incoredata and
     friends) with the new attributes in data->attrs[].  */
  nentry = data->end - data->start;
  memset(&newincore, 0, sizeof(newincore));
  data_addid(&newincore, 0);	/* start data at offset 1 */

  data->mainschema = 0;
  data->mainschemaoffsets = solv_free(data->mainschemaoffsets);

  keylink = calculate_keylink(data);
  /* join entry data */
  /* we start with the meta data, entry -1 */
  for (entry = -1; entry < nentry; entry++)
    {
      oldschemaid = 0;
      dp = data->incoredata;
      if (dp)
	{
	  dp += entry >= 0 ? data->incoreoffset[entry] : 1;
          dp = data_read_id(dp, &oldschemaid);
	}
      memset(seen, 0, data->nkeys * sizeof(Id));
#if 0
fprintf(stderr, "oldschemaid %d\n", oldschemaid);
fprintf(stderr, "schemata %d\n", data->schemata[oldschemaid]);
fprintf(stderr, "schemadata %p\n", data->schemadata);
#endif

      /* seen: -1: old data,  0: skipped,  >0: id + 1 */
      neednewschema = 0;
      sp = schema;
      haveoldkl = 0;
      for (keyp = data->schemadata + data->schemata[oldschemaid]; *keyp; keyp++)
	{
	  if (seen[*keyp])
	    {
	      /* oops, should not happen */
	      neednewschema = 1;
	      continue;
	    }
	  seen[*keyp] = -1;	/* use old marker */
	  *sp++ = *keyp;
	  if (keylink[*keyp])
	    haveoldkl = 1;	/* potential keylink conflict */
	}

      /* strip solvables key */
      if (entry < 0 && solvkeyid && seen[solvkeyid])
	{
	  *sp = 0;
	  for (sp = keyp = schema; *sp; sp++)
	    if (*sp != solvkeyid)
	      *keyp++ = *sp;
	  sp = keyp;
	  seen[solvkeyid] = 0;
	  neednewschema = 1;
	}

      /* add new entries */
      if (entry >= 0)
	keyp = data->attrs ? data->attrs[entry] : 0;
      else
        keyp = data->xattrs ? data->xattrs[1] : 0;
      if (keyp)
        for (; *keyp; keyp += 2)
	  {
	    if (!seen[*keyp])
	      {
	        neednewschema = 1;
	        *sp++ = *keyp;
		if (haveoldkl && keylink[*keyp])		/* this should be pretty rare */
		  {
		    Id kl;
		    for (kl = keylink[*keyp]; kl != *keyp; kl = keylink[kl])
		      if (seen[kl] == -1)
		        {
			  /* replacing old key kl, remove from schema and seen */
			  Id *osp;
			  for (osp = schema; osp < sp; osp++)
			    if (*osp == kl)
			      {
			        memmove(osp, osp + 1, (sp - osp) * sizeof(Id));
			        sp--;
			        seen[kl] = 0;
				break;
			      }
		        }
		  }
	      }
	    seen[*keyp] = keyp[1] + 1;
	  }

      /* add solvables key if needed */
      if (entry < 0 && data->end != data->start)
	{
	  *sp++ = solvkeyid;	/* always last in schema */
	  neednewschema = 1;
	}

      /* commit schema */
      *sp = 0;
      if (neednewschema)
        /* Ideally we'd like to sort the new schema here, to ensure
	   schema equality independend of the ordering. */
	schemaid = repodata_schema2id(data, schema, 1);
      else
	schemaid = oldschemaid;

      if (entry < 0)
	{
	  data->mainschemaoffsets = solv_calloc(sp - schema, sizeof(Id));
	  data->mainschema = schemaid;
	}

      /* find offsets in old incore data */
      if (oldschemaid)
	{
	  Id *lastneeded = 0;
	  for (sp = data->schemadata + data->schemata[oldschemaid]; *sp; sp++)
	    if (seen[*sp] == -1)
	      lastneeded = sp + 1;
	  if (lastneeded)
	    {
	      if (!oldincoreoffs)
	        oldincoreoffs = solv_malloc2(data->nkeys, 2 * sizeof(Offset));
	      for (sp = data->schemadata + data->schemata[oldschemaid]; sp != lastneeded; sp++)
		{
		  /* Skip the data associated with this old key.  */
		  key = data->keys + *sp;
		  ndp = dp;
		  if (key->storage == KEY_STORAGE_VERTICAL_OFFSET)
		    {
		      ndp = data_skip(ndp, REPOKEY_TYPE_ID);
		      ndp = data_skip(ndp, REPOKEY_TYPE_ID);
		    }
		  else if (key->storage == KEY_STORAGE_INCORE)
		    ndp = data_skip_key(data, ndp, key);
		  oldincoreoffs[*sp * 2] = dp - data->incoredata;
		  oldincoreoffs[*sp * 2 + 1] = ndp - dp;
		  dp = ndp;
		}
	    }
	}

      /* just copy over the complete old entry (including the schemaid) if there was no new data */
      if (entry >= 0 && !neednewschema && oldschemaid && (!data->attrs || !data->attrs[entry]) && dp)
	{
	  ndp = data->incoredata + data->incoreoffset[entry];
	  data->incoreoffset[entry] = newincore.len;
	  data_addblob(&newincore, ndp, dp - ndp);
	  goto entrydone;
	}

      /* Now create data blob.  We walk through the (possibly new) schema
	 and either copy over old data, or insert the new.  */
      if (entry >= 0)
        data->incoreoffset[entry] = newincore.len;
      data_addid(&newincore, schemaid);

      /* we don't use a pointer to the schemadata here as repodata_serialize_key
       * may call repodata_schema2id() which might realloc our schemadata */
      for (schemaidx = data->schemata[schemaid]; (keyid = data->schemadata[schemaidx]) != 0; schemaidx++)
	{
	  if (entry < 0)
	    {
	      data->mainschemaoffsets[schemaidx - data->schemata[schemaid]] = newincore.len;
	      if (keyid == solvkeyid)
		{
		  /* add flexarray entry count */
		  data_addid(&newincore, data->end - data->start);
		  break;	/* always the last entry */
		}
	    }
	  if (seen[keyid] == -1)
	    {
	      if (oldincoreoffs[keyid * 2 + 1])
		data_addblob(&newincore, data->incoredata + oldincoreoffs[keyid * 2], oldincoreoffs[keyid * 2 + 1]);
	    }
	  else if (seen[keyid])
	    repodata_serialize_key(data, &newincore, &newvincore, schema, data->keys + keyid, seen[keyid] - 1);
	}

entrydone:
      /* free memory */
      if (entry >= 0 && data->attrs)
	{
	  if (data->attrs[entry])
	    data->attrs[entry] = solv_free(data->attrs[entry]);
	  if (entry && entry % 4096 == 0 && data->nxattrs <= 2 && entry + 64 < nentry)
	    {
	      compact_attrdata(data, entry + 1, nentry);	/* try to free some memory */
#if 0
	      printf("  attr data: %d K\n", data->attrdatalen / 1024);
	      printf("  attrid data: %d K\n", data->attriddatalen / (1024 / 4));
	      printf("  incore data: %d K\n", newincore.len / 1024);
	      printf("  sum: %d K\n", (newincore.len + data->attrdatalen + data->attriddatalen * 4) / 1024);
	      /* malloc_stats(); */
#endif
	    }
	}
    }
  /* free all xattrs */
  for (entry = 0; entry < data->nxattrs; entry++)
    if (data->xattrs[entry])
      solv_free(data->xattrs[entry]);
  data->xattrs = solv_free(data->xattrs);
  data->nxattrs = 0;

  data->lasthandle = 0;
  data->lastkey = 0;
  data->lastdatalen = 0;
  solv_free(schema);
  solv_free(seen);
  solv_free(keylink);
  solv_free(oldincoreoffs);
  repodata_free_schemahash(data);

  solv_free(data->incoredata);
  data->incoredata = newincore.buf;
  data->incoredatalen = newincore.len;
  data->incoredatafree = 0;

  data->vincore = newvincore.buf;
  data->vincorelen = newvincore.len;

  data->attrs = solv_free(data->attrs);
  data->attrdata = solv_free(data->attrdata);
  data->attriddata = solv_free(data->attriddata);
  data->attrnum64data = solv_free(data->attrnum64data);
  data->attrdatalen = 0;
  data->attriddatalen = 0;
  data->attrnum64datalen = 0;
#if 0
  printf("repodata_internalize %d done\n", data->repodataid);
  printf("  incore data: %d K\n", data->incoredatalen / 1024);
#endif
}

void
repodata_disable_paging(Repodata *data)
{
  if (maybe_load_repodata(data, 0))
    {
      repopagestore_disable_paging(&data->store);
      data->storestate++;
    }
}

/* call the pool's loadcallback to load a stub repodata */
static void
repodata_stub_loader(Repodata *data)
{
  Repo *repo = data->repo;
  Pool *pool = repo->pool;
  int r, i;
  struct s_Pool_tmpspace oldtmpspace;
  Datapos oldpos;

  if (!pool->loadcallback)
    {
      data->state = REPODATA_ERROR;
      return;
    }
  data->state = REPODATA_LOADING;

  /* save tmp space and pos */
  oldtmpspace = pool->tmpspace;
  memset(&pool->tmpspace, 0, sizeof(pool->tmpspace));
  oldpos = pool->pos;

  r = pool->loadcallback(pool, data, pool->loadcallbackdata);

  /* restore tmp space and pos */
  for (i = 0; i < POOL_TMPSPACEBUF; i++)
    solv_free(pool->tmpspace.buf[i]);
  pool->tmpspace = oldtmpspace;
  if (r && oldpos.repo == repo && oldpos.repodataid == data->repodataid)
    memset(&oldpos, 0, sizeof(oldpos));
  pool->pos = oldpos;

  data->state = r ? REPODATA_AVAILABLE : REPODATA_ERROR;
}

static inline void
repodata_add_stubkey(Repodata *data, Id keyname, Id keytype)
{
  Repokey xkey;

  xkey.name = keyname;
  xkey.type = keytype;
  xkey.storage = KEY_STORAGE_INCORE;
  xkey.size = 0;
  repodata_key2id(data, &xkey, 1);
}

static Repodata *
repodata_add_stub(Repodata **datap)
{
  Repodata *data = *datap;
  Repo *repo = data->repo;
  Id repodataid = data - repo->repodata;
  Repodata *sdata = repo_add_repodata(repo, 0);
  data = repo->repodata + repodataid;
  if (data->end > data->start)
    repodata_extend_block(sdata, data->start, data->end - data->start);
  sdata->state = REPODATA_STUB;
  sdata->loadcallback = repodata_stub_loader;
  *datap = data;
  return sdata;
}

Repodata *
repodata_create_stubs(Repodata *data)
{
  Repo *repo = data->repo;
  Pool *pool = repo->pool;
  Repodata *sdata;
  int *stubdataids;
  Dataiterator di;
  Id xkeyname = 0;
  int i, cnt = 0;

  dataiterator_init(&di, pool, repo, SOLVID_META, REPOSITORY_EXTERNAL, 0, 0);
  while (dataiterator_step(&di))
    if (di.data == data)
      cnt++;
  dataiterator_free(&di);
  if (!cnt)
    return data;
  stubdataids = solv_calloc(cnt, sizeof(*stubdataids));
  for (i = 0; i < cnt; i++)
    {
      sdata = repodata_add_stub(&data);
      stubdataids[i] = sdata - repo->repodata;
    }
  i = 0;
  dataiterator_init(&di, pool, repo, SOLVID_META, REPOSITORY_EXTERNAL, 0, 0);
  sdata = 0;
  while (dataiterator_step(&di))
    {
      if (di.data != data)
	continue;
      if (di.key->name == REPOSITORY_EXTERNAL && !di.nparents)
	{
	  dataiterator_entersub(&di);
	  sdata = repo->repodata + stubdataids[i++];
	  xkeyname = 0;
	  continue;
	}
      repodata_set_kv(sdata, SOLVID_META, di.key->name, di.key->type, &di.kv);
      if (di.key->name == REPOSITORY_KEYS && di.key->type == REPOKEY_TYPE_IDARRAY)
	{
	  if (!xkeyname)
	    {
	      if (!di.kv.eof)
		xkeyname = di.kv.id;
	    }
	  else
	    {
	      repodata_add_stubkey(sdata, xkeyname, di.kv.id);
	      if (xkeyname == SOLVABLE_FILELIST)
	        repodata_set_filelisttype(sdata, REPODATA_FILELIST_EXTENSION);
	      xkeyname = 0;
	    }
	}
    }
  dataiterator_free(&di);
  for (i = 0; i < cnt; i++)
    repodata_internalize(repo->repodata + stubdataids[i]);
  solv_free(stubdataids);
  return data;
}

void
repodata_set_filelisttype(Repodata *data, int type)
{
  data->filelisttype = type;
}

unsigned int
repodata_memused(Repodata *data)
{
  return data->incoredatalen + data->vincorelen;
}

