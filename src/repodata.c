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
  data->start = repo->start;
  data->end = repo->end;
  data->incoreoffset = sat_extend_resize(0, data->end - data->start, sizeof(Id), REPODATA_BLOCK);
  data->pagefd = -1;
}

void
repodata_free(Repodata *data)
{
  sat_free(data->keys);
  sat_free(data->schemata);
  sat_free(data->schemadata);

  sat_free(data->spool.strings);
  sat_free(data->spool.stringspace);
  sat_free(data->spool.stringhashtbl);

  sat_free(data->dirpool.dirs);
  sat_free(data->dirpool.dirtraverse);

  sat_free(data->incoredata);
  sat_free(data->incoreoffset);
  sat_free(data->verticaloffset);

  sat_free(data->blob_store);
  sat_free(data->pages);
  sat_free(data->mapped);

  sat_free(data->vincore);

  sat_free(data->attrs);
  sat_free(data->attrdata);
  sat_free(data->attriddata);
  
  sat_free(data->location);
  sat_free(data->addedfileprovides);

  if (data->pagefd != -1)
    close(data->pagefd);
}

static unsigned char *
forward_to_key(Repodata *data, Id keyid, Id schemaid, unsigned char *dp)
{
  Id k, *keyp;

  keyp = data->schemadata + data->schemata[schemaid];
  while ((k = *keyp++) != 0)
    {
      if (k == keyid)
	return dp;
      if (data->keys[k].storage == KEY_STORAGE_VERTICAL_OFFSET)
	{
	  dp = data_skip(dp, REPOKEY_TYPE_ID);	/* skip that offset */
	  dp = data_skip(dp, REPOKEY_TYPE_ID);	/* skip that length */
	  continue;
	}
      if (data->keys[k].storage != KEY_STORAGE_INCORE)
	continue;
      dp = data_skip(dp, data->keys[k].type);
    }
  return 0;
}

#define BLOB_PAGEBITS 15
#define BLOB_PAGESIZE (1 << BLOB_PAGEBITS)

static unsigned char *
load_page_range(Repodata *data, unsigned int pstart, unsigned int pend)
{
/* Make sure all pages from PSTART to PEND (inclusive) are loaded,
   and are consecutive.  Return a pointer to the mapping of PSTART.  */
  unsigned char buf[BLOB_PAGESIZE];
  unsigned int i;

  /* Quick check in case all pages are there already and consecutive.  */
  for (i = pstart; i <= pend; i++)
    if (data->pages[i].mapped_at == -1
        || (i > pstart
	    && data->pages[i].mapped_at
	       != data->pages[i-1].mapped_at + BLOB_PAGESIZE))
      break;
  if (i > pend)
    return data->blob_store + data->pages[pstart].mapped_at;

  if (data->pagefd == -1)
    return 0;

  /* Ensure that we can map the numbers of pages we need at all.  */
  if (pend - pstart + 1 > data->ncanmap)
    {
      unsigned int oldcan = data->ncanmap;
      data->ncanmap = pend - pstart + 1;
      if (data->ncanmap < 4)
        data->ncanmap = 4;
      data->mapped = sat_realloc2(data->mapped, data->ncanmap, sizeof(data->mapped[0]));
      memset (data->mapped + oldcan, 0, (data->ncanmap - oldcan) * sizeof (data->mapped[0]));
      data->blob_store = sat_realloc2(data->blob_store, data->ncanmap, BLOB_PAGESIZE);
#ifdef DEBUG_PAGING
      fprintf (stderr, "PAGE: can map %d pages\n", data->ncanmap);
#endif
    }

  /* Now search for "cheap" space in our store.  Space is cheap if it's either
     free (very cheap) or contains pages we search for anyway.  */

  /* Setup cost array.  */
  unsigned int cost[data->ncanmap];
  for (i = 0; i < data->ncanmap; i++)
    {
      unsigned int pnum = data->mapped[i];
      if (pnum == 0)
        cost[i] = 0;
      else
        {
	  pnum--;
	  Attrblobpage *p = data->pages + pnum;
	  assert (p->mapped_at != -1);
	  if (pnum >= pstart && pnum <= pend)
	    cost[i] = 1;
	  else
	    cost[i] = 3;
	}
    }

  /* And search for cheapest space.  */
  unsigned int best_cost = -1;
  unsigned int best = 0;
  unsigned int same_cost = 0;
  for (i = 0; i + pend - pstart < data->ncanmap; i++)
    {
      unsigned int c = cost[i];
      unsigned int j;
      for (j = 0; j < pend - pstart + 1; j++)
        c += cost[i+j];
      if (c < best_cost)
        best_cost = c, best = i;
      else if (c == best_cost)
        same_cost++;
      /* A null cost won't become better.  */
      if (c == 0)
        break;
    }
  /* If all places have the same cost we would thrash on slot 0.  Avoid
     this by doing a round-robin strategy in this case.  */
  if (same_cost == data->ncanmap - pend + pstart - 1)
    best = data->rr_counter++ % (data->ncanmap - pend + pstart);

  /* So we want to map our pages from [best] to [best+pend-pstart].
     Use a very simple strategy, which doesn't make the best use of
     our resources, but works.  Throw away all pages in that range
     (even ours) then copy around ours (in case they were outside the 
     range) or read them in.  */
  for (i = best; i < best + pend - pstart + 1; i++)
    {
      unsigned int pnum = data->mapped[i];
      if (pnum--
          /* If this page is exactly at the right place already,
	     no need to evict it.  */
          && pnum != pstart + i - best)
	{
	  /* Evict this page.  */
#ifdef DEBUG_PAGING
	  fprintf (stderr, "PAGE: evict page %d from %d\n", pnum, i);
#endif
	  cost[i] = 0;
	  data->mapped[i] = 0;
	  data->pages[pnum].mapped_at = -1;
	}
    }

  /* Everything is free now.  Read in the pages we want.  */
  for (i = pstart; i <= pend; i++)
    {
      Attrblobpage *p = data->pages + i;
      unsigned int pnum = i - pstart + best;
      void *dest = data->blob_store + pnum * BLOB_PAGESIZE;
      if (p->mapped_at != -1)
        {
	  if (p->mapped_at != pnum * BLOB_PAGESIZE)
	    {
#ifdef DEBUG_PAGING
	      fprintf (stderr, "PAGECOPY: %d to %d\n", i, pnum);
#endif
	      /* Still mapped somewhere else, so just copy it from there.  */
	      memcpy (dest, data->blob_store + p->mapped_at, BLOB_PAGESIZE);
	      data->mapped[p->mapped_at / BLOB_PAGESIZE] = 0;
	    }
	}
      else
        {
	  unsigned int in_len = p->file_size;
	  unsigned int compressed = in_len & 1;
	  in_len >>= 1;
#ifdef DEBUG_PAGING
	  fprintf (stderr, "PAGEIN: %d to %d", i, pnum);
#endif
          if (pread(data->pagefd, compressed ? buf : dest, in_len, p->file_offset) != in_len)
	    {
	      perror ("mapping pread");
	      return 0;
	    }
	  if (compressed)
	    {
	      unsigned int out_len;
	      out_len = unchecked_decompress_buf(buf, in_len,
						  dest, BLOB_PAGESIZE);
	      if (out_len != BLOB_PAGESIZE && i < data->num_pages - 1)
	        {
	          fprintf(stderr, "can't decompress\n");
		  return 0;
		}
#ifdef DEBUG_PAGING
	      fprintf (stderr, " (expand %d to %d)", in_len, out_len);
#endif
	    }
#ifdef DEBUG_PAGING
	  fprintf (stderr, "\n");
#endif
	}
      p->mapped_at = pnum * BLOB_PAGESIZE;
      data->mapped[pnum] = i + 1;
    }
  return data->blob_store + best * BLOB_PAGESIZE;
}

static unsigned char *
make_vertical_available(Repodata *data, Repokey *key, Id off, Id len)
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
  dp = load_page_range(data, off / BLOB_PAGESIZE, (off + len - 1) / BLOB_PAGESIZE);
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
      *dpp = data_skip(dp, key->type);
      return dp;
    }
  else if (key->storage == KEY_STORAGE_VERTICAL_OFFSET)
    {
      Id off, len;
      dp = data_read_id(dp, &off);
      dp = data_read_id(dp, &len);
      *dpp = dp;
      return make_vertical_available(data, key, off, len);
    }
  return 0;
}

static inline int
maybe_load_repodata(Repodata *data, Id *keyid)
{
  if (data->state == REPODATA_STUB)
    {
      if (data->loadcallback)
	{
	  if (keyid)
	    {
	      /* key order may change when loading */
	      int i;
	      Id name = data->keys[*keyid].name;
	      Id type = data->keys[*keyid].type;
	      data->loadcallback(data);
	      if (data->state == REPODATA_AVAILABLE)
		{
		  for (i = 1; i < data->nkeys; i++)
		    if (data->keys[i].name == name && data->keys[i].type == type)
		      break;
		  if (i < data->nkeys)
		    *keyid = i;
		  else
		    return 0;
		}
	    }
	  else
	    data->loadcallback(data);
	}
      else
	data->state = REPODATA_ERROR;
    }
  if (data->state == REPODATA_AVAILABLE)
    return 1;
  data->state = REPODATA_ERROR;
  return 0;
}

Id
repodata_lookup_id(Repodata *data, Id entry, Id keyid)
{
  Id schema;
  Repokey *key;
  Id id, *keyp;
  unsigned char *dp;

  if (!maybe_load_repodata(data, &keyid))
    return 0;
  dp = data->incoredata + data->incoreoffset[entry];
  dp = data_read_id(dp, &schema);
  /* make sure the schema of this solvable contains the key */
  for (keyp = data->schemadata + data->schemata[schema]; *keyp != keyid; keyp++)
    if (!*keyp)
      return 0;
  dp = forward_to_key(data, keyid, schema, dp);
  key = data->keys + keyid;
  dp = get_data(data, key, &dp);
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
repodata_lookup_str(Repodata *data, Id entry, Id keyid)
{
  Id schema;
  Repokey *key;
  Id id, *keyp;
  unsigned char *dp;

  if (!maybe_load_repodata(data, &keyid))
    return 0;

  dp = data->incoredata + data->incoreoffset[entry];
  dp = data_read_id(dp, &schema);
  /* make sure the schema of this solvable contains the key */
  for (keyp = data->schemadata + data->schemata[schema]; *keyp != keyid; keyp++)
    if (!*keyp)
      return 0;
  dp = forward_to_key(data, keyid, schema, dp);
  key = data->keys + keyid;
  dp = get_data(data, key, &dp);
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
repodata_lookup_num(Repodata *data, Id entry, Id keyid, unsigned int *value)
{
  Id schema;
  Repokey *key;
  Id *keyp;
  KeyValue kv;
  unsigned char *dp;

  *value = 0;

  if (!maybe_load_repodata(data, &keyid))
    return 0;

  dp = data->incoredata + data->incoreoffset[entry];
  dp = data_read_id(dp, &schema);
  /* make sure the schema of this solvable contains the key */
  for (keyp = data->schemadata + data->schemata[schema]; *keyp != keyid; keyp++)
    if (!*keyp)
      return 0;
  dp = forward_to_key(data, keyid, schema, dp);
  key = data->keys + keyid;
  dp = get_data(data, key, &dp);
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
repodata_lookup_void(Repodata *data, Id entry, Id keyid)
{
  Id schema;
  Id *keyp;
  unsigned char *dp;
  if (!maybe_load_repodata(data, &keyid))
    return 0;
  dp = data->incoredata + data->incoreoffset[entry];
  dp = data_read_id(dp, &schema);
  for (keyp = data->schemadata + data->schemata[schema]; *keyp != keyid; keyp++)
    if (!*keyp)
      return 0;
  return 1;
}

const unsigned char *
repodata_lookup_bin_checksum(Repodata *data, Id entry, Id keyid, Id *typep)
{
  Id schema;
  Id *keyp;
  Repokey *key;
  unsigned char *dp;

  if (!maybe_load_repodata(data, &keyid))
    return 0;
  dp = data->incoredata + data->incoreoffset[entry];
  dp = data_read_id(dp, &schema);
  for (keyp = data->schemadata + data->schemata[schema]; *keyp != keyid; keyp++)
    if (!*keyp)
      return 0;
  dp = forward_to_key(data, keyid, schema, dp);
  key = data->keys + keyid;
  *typep = key->type;
  return get_data(data, key, &dp);
}

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

  if (!maybe_load_repodata(data, 0))
    return;

  dp = data->incoredata + data->incoreoffset[entry];
  dp = data_read_id(dp, &schema);
  keyp = data->schemadata + data->schemata[schema];
  if (keyname)
    {
      /* search in a specific key */
      for (kp = keyp; (k = *kp++) != 0; )
	if (data->keys[k].name == keyname)
	  break;
      if (k == 0)
	return;
      dp = forward_to_key(data, k, schema, dp);
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
      do
	{
	  ddp = data_fetch(ddp, &kv, key);
	  if (!ddp)
	    break;
	  stop = callback(cbdata, data->repo->pool->solvables + data->start + entry, data, key, &kv);
	}
      while (!kv.eof && !stop);
      if (onekey || stop > SEARCH_NEXT_KEY)
	return;
    }
}

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
  unsigned char *dp = data->incoredata + data->incoreoffset[di->solvid - data->start];
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
      dp = forward_to_key(data, k, schema, dp);
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
  if (p)
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
      di->data = repo->repodata + repo->nrepodata - 1;
      di->state = 0;
    }
  di->match = match;
  di->keyname = keyname;
  static Id zeroid = 0;
  di->keyp = &zeroid;
  di->kv.eof = 1;
  di->repo = repo;
  di->idp = 0;
}

/* FIXME factor and merge with repo_matchvalue */
static int
dataiterator_match(Dataiterator *di, KeyValue *kv)
{
  int flags = di->flags;

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
	default:
	  return 0;
	}
      switch ((flags & SEARCH_STRINGMASK))
	{
	  case SEARCH_SUBSTRING:
	    if (flags & SEARCH_NOCASE)
	      {
	        if (!strcasestr(kv->str, di->match))
		  return 0;
	      }
	    else
	      {
	        if (!strstr(kv->str, di->match))
		  return 0;
	      }
	    break;
	  case SEARCH_STRING:
	    if (flags & SEARCH_NOCASE)
	      {
	        if (strcasecmp(di->match, kv->str))
		  return 0;
	      }
	    else
	      {
	        if (strcmp(di->match, kv->str))
		  return 0;
	      }
	    break;
	  case SEARCH_GLOB:
	    if (fnmatch(di->match, kv->str, (flags & SEARCH_NOCASE) ? FNM_CASEFOLD : 0))
	      return 0;
	    break;
#if 0
	  case SEARCH_REGEX:
	    if (regexec(&di->regexp, kv->str, 0, NULL, 0))
	      return 0;
#endif
	  default:
	    return 0;
	}
    }
  return 1;
}

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
  { SOLVABLE_FRESHENS,    REPOKEY_TYPE_IDARRAY, 0, KEY_STORAGE_SOLVABLE },
  { RPM_RPMDBID,          REPOKEY_TYPE_U32, 0, KEY_STORAGE_SOLVABLE },
};

int
dataiterator_step(Dataiterator *di)
{
restart:
  while (1)
    {
      if (di->state)
	{
	  if (di->idp)
	    {
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
		break;
	      case SOLVABLE_ARCH:
		if (!s->arch)
		  continue;
		di->kv.id = s->arch;
		break;
	      case SOLVABLE_EVR:
		if (!s->evr)
		  continue;
		di->kv.id = s->evr;
		break;
	      case SOLVABLE_VENDOR:
		if (!s->vendor)
		  continue;
		di->kv.id = s->vendor;
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
	      case SOLVABLE_FRESHENS:
		di->idp = s->freshens
		    ? di->repo->idarraydata + s->freshens : 0;
		continue;
	      case RPM_RPMDBID:
		if (!di->repo->rpmdbid)
		  continue;
		di->kv.num = di->repo->rpmdbid[di->solvid - di->repo->start];
		break;
	      default:
		di->data = di->repo->repodata - 1;
		di->kv.eof = 1;
		di->state = 0;
		continue;
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
			  while (++di->solvid < repo->end)
			    if (repo->pool->solvables[di->solvid].repo == repo)
			      break;
			  if (di->solvid >= repo->end)
			    return 0;
			  di->data = repo->repodata - 1;
			  if (di->flags & SEARCH_NO_STORAGE_SOLVABLE)
			    continue;
			  static Id zeroid = 0;
			  di->keyp = &zeroid;
			  di->state = 1;
			  goto restart;
			}
		      if (di->solvid >= data->start && di->solvid < data->end)
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
	}
weg2:
      if (!di->match
	  || dataiterator_match(di, &di->kv))
	break;
    }
  return 1;
}

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
	  data->attrs = sat_extend(data->attrs, old, new, sizeof(Id *), REPODATA_BLOCK);
	  memset(data->attrs + old, 0, new * sizeof(Id *));
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
	  data->attrs = sat_extend_resize(data->attrs, old + new, sizeof(Id *), REPODATA_BLOCK);
	  memmove(data->attrs + new, data->attrs, old * sizeof(Id *));
	  memset(data->attrs, 0, new * sizeof(Id *));
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

static void
repodata_insert_keyid(Repodata *data, Id entry, Id keyid, Id val, int overwrite)
{
  Id *pp;
  int i;
  if (!data->attrs)
    {
      data->attrs = sat_calloc_block(data->end - data->start, sizeof(Id *),
				     REPODATA_BLOCK);
    }
  i = 0;
  if (data->attrs[entry])
    {
      for (pp = data->attrs[entry]; *pp; pp += 2)
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
      i = pp - data->attrs[entry];
    }
  data->attrs[entry] = sat_extend(data->attrs[entry], i, 3, sizeof(Id), REPODATA_ATTRS_BLOCK);
  pp = data->attrs[entry] + i;
  *pp++ = keyid;
  *pp++ = val;
  *pp = 0;
}

void
repodata_set(Repodata *data, Id entry, Repokey *key, Id val)
{
  Id keyid;

  /* find key in keys */
  for (keyid = 1; keyid < data->nkeys; keyid++)
    if (data->keys[keyid].name == key->name && data->keys[keyid].type == key->type)
      {
        if ((key->type == REPOKEY_TYPE_CONSTANT || key->type == REPOKEY_TYPE_CONSTANTID) && key->size != data->keys[keyid].size)
          continue;
        break;
      }
  if (keyid == data->nkeys)
    {
      /* allocate new key */
      data->keys = sat_realloc2(data->keys, data->nkeys + 1, sizeof(Repokey));
      data->keys[data->nkeys++] = *key;
      if (data->verticaloffset)
	{
	  data->verticaloffset = sat_realloc2(data->verticaloffset, data->nkeys, sizeof(Id));
	  data->verticaloffset[data->nkeys - 1] = 0;
	}
    }
  repodata_insert_keyid(data, entry, keyid, val, 1);
}

void
repodata_set_id(Repodata *data, Id entry, Id keyname, Id id)
{
  Repokey key;
  key.name = keyname;
  key.type = REPOKEY_TYPE_ID;
  key.size = 0;
  key.storage = KEY_STORAGE_INCORE;
  repodata_set(data, entry, &key, id);
}

void
repodata_set_num(Repodata *data, Id entry, Id keyname, Id num)
{
  Repokey key;
  key.name = keyname;
  key.type = REPOKEY_TYPE_NUM;
  key.size = 0;
  key.storage = KEY_STORAGE_INCORE;
  repodata_set(data, entry, &key, num);
}

void
repodata_set_poolstr(Repodata *data, Id entry, Id keyname, const char *str)
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
  repodata_set(data, entry, &key, id);
}

void
repodata_set_constant(Repodata *data, Id entry, Id keyname, Id constant)
{
  Repokey key;
  key.name = keyname;
  key.type = REPOKEY_TYPE_CONSTANT;
  key.size = constant;
  key.storage = KEY_STORAGE_INCORE;
  repodata_set(data, entry, &key, 0);
}

void
repodata_set_constantid(Repodata *data, Id entry, Id keyname, Id id)
{
  Repokey key;
  key.name = keyname;
  key.type = REPOKEY_TYPE_CONSTANTID;
  key.size = id;
  key.storage = KEY_STORAGE_INCORE;
  repodata_set(data, entry, &key, 0);
}

void
repodata_set_void(Repodata *data, Id entry, Id keyname)
{
  Repokey key;
  key.name = keyname;
  key.type = REPOKEY_TYPE_VOID;
  key.size = 0;
  key.storage = KEY_STORAGE_INCORE;
  repodata_set(data, entry, &key, 0);
}

void
repodata_set_str(Repodata *data, Id entry, Id keyname, const char *str)
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
  repodata_set(data, entry, &key, data->attrdatalen);
  data->attrdatalen += l;
}

static void
repoadata_add_array(Repodata *data, Id entry, Id keyname, Id keytype, int entrysize)
{
  int oldsize;
  Id *ida, *pp;

  pp = 0;
  if (data->attrs && data->attrs[entry])
    for (pp = data->attrs[entry]; *pp; pp += 2)
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
      repodata_set(data, entry, &key, data->attriddatalen);
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
repodata_set_bin_checksum(Repodata *data, Id entry, Id keyname, Id type,
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
  repodata_set(data, entry, &key, data->attrdatalen);
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
repodata_set_checksum(Repodata *data, Id entry, Id keyname, Id type,
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
  repodata_set_bin_checksum(data, entry, keyname, type, buf);
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

Id repodata_globalize_id(Repodata *data, Id id)
{ 
  if (!data || !data->localpool)
    return id;
  return str2id(data->repo->pool, stringpool_id2str(&data->spool, id), 1);
}

void
repodata_add_dirnumnum(Repodata *data, Id entry, Id keyname, Id dir, Id num, Id num2)
{

#if 0
fprintf(stderr, "repodata_add_dirnumnum %d %d %d %d (%d)\n", entry, dir, num, num2, data->attriddatalen);
#endif
  repoadata_add_array(data, entry, keyname, REPOKEY_TYPE_DIRNUMNUMARRAY, 3);
  data->attriddata[data->attriddatalen++] = dir;
  data->attriddata[data->attriddatalen++] = num;
  data->attriddata[data->attriddatalen++] = num2;
  data->attriddata[data->attriddatalen++] = 0;
}

void
repodata_add_dirstr(Repodata *data, Id entry, Id keyname, Id dir, const char *str)
{
  Id stroff;
  int l;

  l = strlen(str) + 1;
  data->attrdata = sat_extend(data->attrdata, data->attrdatalen, l, 1, REPODATA_ATTRDATA_BLOCK);
  memcpy(data->attrdata + data->attrdatalen, str, l);
  stroff = data->attrdatalen;
  data->attrdatalen += l;

#if 0
fprintf(stderr, "repodata_add_dirstr %d %d %s (%d)\n", entry, dir, str,  data->attriddatalen);
#endif
  repoadata_add_array(data, entry, keyname, REPOKEY_TYPE_DIRSTRARRAY, 2);
  data->attriddata[data->attriddatalen++] = dir;
  data->attriddata[data->attriddatalen++] = stroff;
  data->attriddata[data->attriddatalen++] = 0;
}

void
repodata_add_idarray(Repodata *data, Id entry, Id keyname, Id id)
{
#if 0
fprintf(stderr, "repodata_add_idarray %d %d (%d)\n", entry, id, data->attriddatalen);
#endif
  repoadata_add_array(data, entry, keyname, REPOKEY_TYPE_IDARRAY, 1);
  data->attriddata[data->attriddatalen++] = id;
  data->attriddata[data->attriddatalen++] = 0;
}

void
repodata_add_poolstr_array(Repodata *data, Id entry, Id keyname,
			   const char *str)
{
  Id id;
  if (data->localpool)
    id = stringpool_str2id(&data->spool, str, 1);
  else
    id = str2id(data->repo->pool, str, 1);
  repodata_add_idarray(data, entry, keyname, id);
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

/*********************************/

/* unify with repo_write! */

#define EXTDATA_BLOCK 1023
#define SCHEMATA_BLOCK 31
#define SCHEMATADATA_BLOCK 255

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
addschema_prepare(Repodata *data, Id *schematacache)
{
  int h, len, i;
  Id *sp;

  memset(schematacache, 0, 256 * sizeof(Id));
  for (i = 0; i < data->nschemata; i++)
    {
      for (sp = data->schemadata + data->schemata[i], h = 0; *sp; len++)
        h = h * 7 + *sp++;
      h &= 255;
      schematacache[h] = i + 1;
    }
  data->schemadata = sat_extend_resize(data->schemadata, data->schemadatalen, sizeof(Id), SCHEMATADATA_BLOCK); 
  data->schemata = sat_extend_resize(data->schemata, data->nschemata, sizeof(Id), SCHEMATA_BLOCK);
}

static Id
addschema(Repodata *data, Id *schema, Id *schematacache)
{
  int h, len; 
  Id *sp, cid; 

  for (sp = schema, len = 0, h = 0; *sp; len++)
    h = h * 7 + *sp++;
  h &= 255; 
  len++;

  cid = schematacache[h];
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
  /* a new one. make room. */
  data->schemadata = sat_extend(data->schemadata, data->schemadatalen, len, sizeof(Id), SCHEMATADATA_BLOCK); 
  data->schemata = sat_extend(data->schemata, data->nschemata, 1, sizeof(Id), SCHEMATA_BLOCK);
  /* add schema */
  memcpy(data->schemadata + data->schemadatalen, schema, len * sizeof(Id));
  data->schemata[data->nschemata] = data->schemadatalen;
  data->schemadatalen += len;
  schematacache[h] = data->nschemata + 1;
#if 0
fprintf(stderr, "addschema: new schema\n");
#endif
  return data->nschemata++; 
}


void
repodata_internalize(Repodata *data)
{
  Repokey *key;
  Id id, entry, nentry, *ida;
  Id schematacache[256];
  Id schemaid, *schema, *sp, oldschema, *keyp, *seen;
  unsigned char *dp, *ndp;
  int newschema, oldcount;
  struct extdata newincore;
  struct extdata newvincore;

  if (!data->attrs)
    return;

  newvincore.buf = data->vincore;
  newvincore.len = data->vincorelen;

  schema = sat_malloc2(data->nkeys, sizeof(Id));
  seen = sat_malloc2(data->nkeys, sizeof(Id));

  /* Merge the data already existing (in data->schemata, ->incoredata and
     friends) with the new attributes in data->attrs[].  */
  nentry = data->end - data->start;
  addschema_prepare(data, schematacache);
  memset(&newincore, 0, sizeof(newincore));
  data_addid(&newincore, 0);
  for (entry = 0; entry < nentry; entry++)
    {
      memset(seen, 0, data->nkeys * sizeof(Id));
      sp = schema;
      dp = data->incoredata + data->incoreoffset[entry];
      if (data->incoredata)
        dp = data_read_id(dp, &oldschema);
      else
	oldschema = 0;
#if 0
fprintf(stderr, "oldschema %d\n", oldschema);
fprintf(stderr, "schemata %d\n", data->schemata[oldschema]);
fprintf(stderr, "schemadata %p\n", data->schemadata);
#endif
      /* seen: -1: old data  0: skipped  >0: id + 1 */
      newschema = 0;
      oldcount = 0;
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
      if (data->attrs[entry])
        for (keyp = data->attrs[entry]; *keyp; keyp += 2)
	  {
	    if (!seen[*keyp])
	      {
	        newschema = 1;
	        *sp++ = *keyp;
	      }
	    seen[*keyp] = keyp[1] + 1;
	  }
      *sp++ = 0;
      if (newschema)
        /* Ideally we'd like to sort the new schema here, to ensure
	   schema equality independend of the ordering.  We can't do that
	   yet.  For once see below (old ids need to come before new ids).
	   An additional difficulty is that we also need to move
	   the values with the keys.  */
	schemaid = addschema(data, schema, schematacache);
      else
	schemaid = oldschema;


      /* Now create data blob.  We walk through the (possibly new) schema
	 and either copy over old data, or insert the new.  */
      /* XXX Here we rely on the fact that the (new) schema has the form
	 o1 o2 o3 o4 ... | n1 n2 n3 ...
	 (oX being the old keyids (possibly overwritten), and nX being
	  the new keyids).  This rules out sorting the keyids in order
	 to ensure a small schema count.  */
      data->incoreoffset[entry] = newincore.len;
      data_addid(&newincore, schemaid);
      for (keyp = data->schemadata + data->schemata[schemaid]; *keyp; keyp++)
	{
	  key = data->keys + *keyp;
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
		ndp = data_skip(dp, key->type);
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
	      struct extdata *xd;
	      unsigned int oldvincorelen = 0;

	      xd = &newincore;
	      if (key->storage == KEY_STORAGE_VERTICAL_OFFSET)
		{
		  xd = &newvincore;
		  oldvincorelen = xd->len;
		}
	      id = seen[*keyp] - 1;
	      switch (key->type)
		{
		case REPOKEY_TYPE_VOID:
		case REPOKEY_TYPE_CONSTANT:
		case REPOKEY_TYPE_CONSTANTID:
		  break;
		case REPOKEY_TYPE_STR:
		  data_addblob(xd, data->attrdata + id, strlen((char *)(data->attrdata + id)) + 1);
		  break;
		case REPOKEY_TYPE_MD5:
		  data_addblob(xd, data->attrdata + id, SIZEOF_MD5);
		  break;
		case REPOKEY_TYPE_SHA1:
		  data_addblob(xd, data->attrdata + id, SIZEOF_SHA1);
		  break;
		case REPOKEY_TYPE_ID:
		case REPOKEY_TYPE_NUM:
		case REPOKEY_TYPE_DIR:
		  data_addid(xd, id);
		  break;
		case REPOKEY_TYPE_IDARRAY:
		  for (ida = data->attriddata + id; *ida; ida++)
		    data_addideof(xd, ida[0], ida[1] ? 0 : 1);
		  break;
		case REPOKEY_TYPE_DIRNUMNUMARRAY:
		  for (ida = data->attriddata + id; *ida; ida += 3)
		    {
		      data_addid(xd, ida[0]);
		      data_addid(xd, ida[1]);
		      data_addideof(xd, ida[2], ida[3] ? 0 : 1);
		    }
		  break;
		case REPOKEY_TYPE_DIRSTRARRAY:
		  for (ida = data->attriddata + id; *ida; ida += 2)
		    {
		      data_addideof(xd, ida[0], ida[2] ? 0 : 1);
		      data_addblob(xd, data->attrdata + ida[1], strlen((char *)(data->attrdata + ida[1])) + 1);
		    }
		  break;
		default:
		  fprintf(stderr, "don't know how to handle type %d\n", key->type);
		  exit(1);
		}
	      if (key->storage == KEY_STORAGE_VERTICAL_OFFSET)
		{
		  /* put offset/len in incore */
		  data_addid(&newincore, data->lastverticaloffset + oldvincorelen);
		  oldvincorelen = xd->len - oldvincorelen;
		  data_addid(&newincore, oldvincorelen);
		}
	    }
	  dp = ndp;
	}
      if (data->attrs[entry])
        sat_free(data->attrs[entry]);
    }
  sat_free(schema);
  sat_free(seen);

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

unsigned int
repodata_compress_page(unsigned char *page, unsigned int len, unsigned char *cpage, unsigned int max)
{
  return compress_buf(page, len, cpage, max);
}

#define SOLV_ERROR_EOF              3

static inline unsigned int
read_u32(FILE *fp)
{
  int c, i;
  unsigned int x = 0; 

  for (i = 0; i < 4; i++) 
    {    
      c = getc(fp);
      if (c == EOF) 
        return 0;
      x = (x << 8) | c; 
    }    
  return x;
}

#define SOLV_ERROR_EOF		3
#define SOLV_ERROR_CORRUPT	6

/* Try to either setup on-demand paging (using FP as backing
   file), or in case that doesn't work (FP not seekable) slurps in
   all pages and deactivates paging.  */
void
repodata_read_or_setup_pages(Repodata *data, unsigned int pagesz, unsigned int blobsz)
{
  FILE *fp = data->fp;
  unsigned int npages;
  unsigned int i;
  unsigned int can_seek;
  long cur_file_ofs;
  unsigned char buf[BLOB_PAGESIZE];

  if (pagesz != BLOB_PAGESIZE)
    {
      /* We could handle this by slurping in everything.  */
      data->error = SOLV_ERROR_CORRUPT;
      return;
    }
  can_seek = 1;
  if ((cur_file_ofs = ftell(fp)) < 0)
    can_seek = 0;
  clearerr(fp);
  if (can_seek)
    data->pagefd = dup(fileno(fp));
  if (data->pagefd == -1)
    can_seek = 0;

#ifdef DEBUG_PAGING
  fprintf (stderr, "can %sseek\n", can_seek ? "" : "NOT ");
#endif
  npages = (blobsz + BLOB_PAGESIZE - 1) / BLOB_PAGESIZE;

  data->num_pages = npages;
  data->pages = sat_malloc2(npages, sizeof(data->pages[0]));

  /* If we can't seek on our input we have to slurp in everything.  */
  if (!can_seek)
    data->blob_store = sat_malloc(npages * BLOB_PAGESIZE);
  for (i = 0; i < npages; i++)
    {
      unsigned int in_len = read_u32(fp);
      unsigned int compressed = in_len & 1;
      Attrblobpage *p = data->pages + i;
      in_len >>= 1;
#ifdef DEBUG_PAGING
      fprintf (stderr, "page %d: len %d (%scompressed)\n",
      	       i, in_len, compressed ? "" : "not ");
#endif
      if (can_seek)
        {
          cur_file_ofs += 4;
	  p->mapped_at = -1;
	  p->file_offset = cur_file_ofs;
	  p->file_size = in_len * 2 + compressed;
	  if (fseek(fp, in_len, SEEK_CUR) < 0)
	    {
	      perror ("fseek");
	      fprintf (stderr, "can't seek after we thought we can\n");
	      /* We can't fall back to non-seeking behaviour as we already
	         read over some data pages without storing them away.  */
	      data->error = SOLV_ERROR_EOF;
	      close(data->pagefd);
	      data->pagefd = -1;
	      return;
	    }
	  cur_file_ofs += in_len;
	}
      else
        {
	  unsigned int out_len;
	  void *dest = data->blob_store + i * BLOB_PAGESIZE;
          p->mapped_at = i * BLOB_PAGESIZE;
	  p->file_offset = 0;
	  p->file_size = 0;
	  /* We can't seek, so suck everything in.  */
	  if (fread(compressed ? buf : dest, in_len, 1, fp) != 1)
	    {
	      perror("fread");
	      data->error = SOLV_ERROR_EOF;
	      return;
	    }
	  if (compressed)
	    {
	      out_len = unchecked_decompress_buf(buf, in_len, dest, BLOB_PAGESIZE);
	      if (out_len != BLOB_PAGESIZE && i < npages - 1)
	        {
		  data->error = SOLV_ERROR_CORRUPT;
		  return;
	        }
	    }
	}
    }
}

void
repodata_disable_paging(Repodata *data)
{
  if (maybe_load_repodata(data, 0)
      && data->num_pages)
    load_page_range (data, 0, data->num_pages - 1);
}
/*
vim:cinoptions={.5s,g0,p5,t0,(0,^-0.5s,n-0.5s:tw=78:cindent:sw=4:
*/
