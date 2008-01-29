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

#include <stdio.h>
#include <stdlib.h>
#include <unistd.h>
#include <assert.h>

#include "repo.h"
#include "pool.h"
#include "poolid_private.h"
#include "util.h"

#include "fastlz.c"

unsigned char *
data_read_id(unsigned char *dp, Id *idp)
{
  Id x = 0;
  unsigned char c;
  for (;;)
    {
      c = *dp++;
      if (!(c & 0x80))
	{
	  *idp = (x << 7) ^ c;
          return dp;
	}
      x = (x << 7) ^ c ^ 128;
    }
}

static unsigned char *
data_read_ideof(unsigned char *dp, Id *idp, int *eof)
{
  Id x = 0;
  unsigned char c;
  for (;;)
    {
      c = *dp++;
      if (!(c & 0x80))
	{
	  if (c & 0x40)
	    {
	      c ^= 0x40;
	      *eof = 0;
	    }
	  else
	    *eof = 1;
	  *idp = (x << 6) ^ c;
          return dp;
	}
      x = (x << 7) ^ c ^ 128;
    }
}

static unsigned char *
data_skip(unsigned char *dp, int type)
{
  unsigned char x;
  switch (type)
    {
    case TYPE_VOID:
    case TYPE_CONSTANT:
      return dp;
    case TYPE_ID:
    case TYPE_NUM:
    case TYPE_DIR:
      while ((*dp & 0x80) != 0)
	dp++;
      return dp + 1;
    case TYPE_IDARRAY:
      while ((*dp & 0xc0) != 0)
	dp++;
      return dp + 1;
    case TYPE_STR:
      while ((*dp) != 0)
	dp++;
      return dp + 1;
    case TYPE_DIRSTRARRAY:
      for (;;)
	{
          while ((*dp & 0x80) != 0)
	    dp++;
	  x = *dp++;
          while ((*dp) != 0)
	    dp++;
	  dp++;
	  if (!(x & 0x40))
	    return dp;
	}
    case TYPE_DIRNUMNUMARRAY:
      for (;;)
	{
	  while ((*dp & 0x80) != 0)
	    dp++;
	  dp++;
	  while ((*dp & 0x80) != 0)
	    dp++;
	  dp++;
	  while ((*dp & 0x80) != 0)
	    dp++;
	  if (!(*dp & 0x40))
	    return dp + 1;
	  dp++;
	}
    default:
      fprintf(stderr, "unknown type in data_skip\n");
      exit(1);
    }
}

static unsigned char *
data_fetch(unsigned char *dp, KeyValue *kv, Repokey *key)
{
  kv->eof = 1;
  if (!dp)
    return 0;
  switch (key->type)
    {
    case TYPE_VOID:
      return dp;
    case TYPE_CONSTANT:
      kv->num = key->size;
      return dp;
    case TYPE_STR:
      kv->str = (const char *)dp;
      return dp + strlen(kv->str) + 1;
    case TYPE_ID:
      return data_read_id(dp, &kv->id);
    case TYPE_NUM:
      return data_read_id(dp, &kv->num);
    case TYPE_IDARRAY:
      return data_read_ideof(dp, &kv->id, &kv->eof);
    case TYPE_DIR:
      return data_read_id(dp, &kv->id);
    case TYPE_DIRSTRARRAY:
      dp = data_read_ideof(dp, &kv->id, &kv->eof);
      kv->str = (const char *)dp;
      return dp + strlen(kv->str) + 1;
    case TYPE_DIRNUMNUMARRAY:
      dp = data_read_id(dp, &kv->id);
      dp = data_read_id(dp, &kv->num);
      return data_read_ideof(dp, &kv->num2, &kv->eof);
    default:
      return 0;
    }
}

static unsigned char *
forward_to_key(Repodata *data, Id key, Id schemaid, unsigned char *dp)
{
  Id k, *keyp;

  keyp = data->schemadata + data->schemata[schemaid];
  while ((k = *keyp++) != 0)
    {
      if (k == key)
	return dp;
      if (data->keys[k].storage == KEY_STORAGE_VERTICAL_OFFSET)
	{
	  dp = data_skip(dp, TYPE_ID);	/* skip that offset */
	  dp = data_skip(dp, TYPE_ID);	/* skip that length */
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
	  /* Not mapped, so read in this page.  */
	  if (fseek(data->fp, p->file_offset, SEEK_SET) < 0)
	    {
	      perror ("mapping fseek");
	      exit (1);
	    }
	  if (fread(compressed ? buf : dest, in_len, 1, data->fp) != 1)
	    {
	      perror ("mapping fread");
	      exit (1);
	    }
	  if (compressed)
	    {
	      unsigned int out_len;
	      out_len = unchecked_decompress_buf(buf, in_len,
						  dest, BLOB_PAGESIZE);
	      if (out_len != BLOB_PAGESIZE
	          && i < data->num_pages - 1)
	        {
	          fprintf (stderr, "can't decompress\n");
	          exit (1);
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
  if (key->type == TYPE_VOID)
    return 0;
  if (off >= data->lastverticaloffset)
    {
      off -= data->lastverticaloffset;
      if (off + len > data->vincorelen)
	return 0;
      return data->vincore + off;
    }
  if (!data->fp)
    return 0;
  if (off + len > key->size)
    return 0;
  /* we now have the offset, go into vertical */
  off += data->verticaloffset[key - data->keys];
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


const char *
repodata_lookup_str(Repodata *data, Id entry, Id keyid)
{
  Id schema;
  Repokey *key;
  Id id, *keyp;
  unsigned char *dp;

  if (data->entryschemau8)
    schema = data->entryschemau8[entry];
  else
    schema = data->entryschema[entry];
  /* make sure the schema of this solvable contains the key */
  for (keyp = data->schemadata + data->schemata[schema]; *keyp != keyid; keyp++)
    if (!*keyp)
      return 0;
  dp = forward_to_key(data, keyid, schema, data->incoredata + data->incoreoffset[entry]);
  key = data->keys + keyid;
  dp = get_data(data, key, &dp);
  if (!dp)
    return 0;
  if (key->type == TYPE_STR)
    return (const char *)dp;
  if (key->type != TYPE_ID)
    return 0;
  /* id type, must either use global or local string strore*/
  dp = data_read_id(dp, &id);
  if (data->localpool)
    return data->spool.stringspace + data->spool.strings[id];
  return id2str(data->repo->pool, id);
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

  if (data->entryschemau8)
    schema = data->entryschemau8[entry];
  else
    schema = data->entryschema[entry];
  keyp = data->schemadata + data->schemata[schema];
  dp = data->incoredata + data->incoreoffset[entry];
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
      if (data->entryschemau8)
	{
	  data->entryschemau8 = sat_realloc(data->entryschemau8, old + new);
	  memset(data->entryschemau8 + old, 0, new);
	}
      if (data->entryschema)
	{
	  data->entryschema = sat_realloc2(data->entryschema, old + new, sizeof(Id));
	  memset(data->entryschema + old, 0, new * sizeof(Id));
	}
      if (data->attrs)
	{
	  data->attrs = sat_realloc2(data->attrs, old + new, sizeof(Id *));
	  memset(data->attrs + old, 0, new * sizeof(Id *));
	}
      data->incoreoffset = sat_realloc2(data->incoreoffset, old + new, sizeof(Id));
      memset(data->incoreoffset + old, 0, new * sizeof(Id));
      data->end = p + 1;
    }
  if (p < data->start)
    {
      int old = data->end - data->start;
      int new = data->start - p;
      if (data->entryschemau8)
	{
	  data->entryschemau8 = sat_realloc(data->entryschemau8, old + new);
	  memmove(data->entryschemau8 + new, data->entryschemau8, old);
	  memset(data->entryschemau8, 0, new);
	}
      if (data->entryschema)
	{
	  data->entryschema = sat_realloc2(data->entryschema, old + new, sizeof(Id));
	  memmove(data->entryschema + new, data->entryschema, old * sizeof(Id));
	  memset(data->entryschema, 0, new * sizeof(Id));
	}
      if (data->attrs)
	{
	  data->attrs = sat_realloc2(data->attrs, old + new, sizeof(Id *));
	  memmove(data->attrs + new, data->attrs, old * sizeof(Id *));
	  memset(data->attrs, 0, new * sizeof(Id *));
	}
      data->incoreoffset = sat_realloc2(data->incoreoffset, old + new, sizeof(Id));
      memmove(data->incoreoffset + new, data->incoreoffset, old * sizeof(Id));
      memset(data->incoreoffset, 0, new * sizeof(Id));
      data->start = p;
    }
}

void
repodata_set(Repodata *data, Id entry, Repokey *key, Id val)
{
  Id keyid, *pp;
  int i;

  /* find key in keys */
  for (keyid = 1; keyid < data->nkeys; keyid++)
    if (data->keys[keyid].name == key->name && data->keys[keyid].type == key->type)
      {
        if (key->type == TYPE_CONSTANT && key->size != data->keys[keyid].size)
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
  key = data->keys + keyid;
  if (!data->attrs)
    data->attrs = sat_calloc(data->end - data->start + 1, sizeof(Id *));
  i = 0;
  if (data->attrs[entry])
    {
      for (pp = data->attrs[entry]; *pp; pp += 2)
        if (*pp == keyid)
          break;
      if (*pp)
        {
          pp[1] = val;
          return;
        }
      i = pp - data->attrs[entry];
    }
  data->attrs[entry] = sat_realloc2(data->attrs[entry], i + 3, sizeof(Id));
  pp = data->attrs[entry] + i;
  *pp++ = keyid;
  *pp++ = val;
  *pp = 0;
}

void
repodata_set_id(Repodata *data, Id entry, Id keyname, Id id)
{
  Repokey key;
  key.name = keyname;
  key.type = TYPE_ID;
  key.size = 0;
  key.storage = KEY_STORAGE_INCORE;
  repodata_set(data, entry, &key, id);
}

void
repodata_set_num(Repodata *data, Id entry, Id keyname, Id num)
{
  Repokey key;
  key.name = keyname;
  key.type = TYPE_NUM;
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
  key.type = TYPE_ID;
  key.size = 0;
  key.storage = KEY_STORAGE_INCORE;
  repodata_set(data, entry, &key, id);
}

void
repodata_set_constant(Repodata *data, Id entry, Id keyname, Id constant)
{
  Repokey key;
  key.name = keyname;
  key.type = TYPE_CONSTANT;
  key.size = constant;
  key.storage = KEY_STORAGE_INCORE;
  repodata_set(data, entry, &key, 0);
}

void
repodata_set_void(Repodata *data, Id entry, Id keyname)
{
  Repokey key;
  key.name = keyname;
  key.type = TYPE_VOID;
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
  key.type = TYPE_STR;
  key.size = 0;
  key.storage = KEY_STORAGE_INCORE;
  data->attrdata = sat_realloc(data->attrdata, data->attrdatalen + l);
  memcpy(data->attrdata + data->attrdatalen, str, l);
  repodata_set(data, entry, &key, data->attrdatalen);
  data->attrdatalen += l;
}

void
repodata_add_dirnumnum(Repodata *data, Id entry, Id keyname, Id dir, Id num, Id num2)
{
  Id *ida, *pp;
  Repokey key;

#if 0
fprintf(stderr, "repodata_add_dirnumnum %d %d %d %d (%d)\n", entry, dir, num, num2, data->attriddatalen);
#endif
  if (data->attrs[entry])
    {
      for (pp = data->attrs[entry]; *pp; pp += 2)
        if (data->keys[*pp].name == keyname && data->keys[*pp].type == TYPE_DIRNUMNUMARRAY)
	  break;
      if (*pp)
	{
	  int oldsize = 0;
	  for (ida = data->attriddata + pp[1]; *ida; ida += 3)
	    oldsize += 3;
	  if (ida + 1 == data->attriddata + data->attriddatalen)
	    {
	      /* this was the last entry, just append it */
	      data->attriddata = sat_realloc2(data->attriddata, data->attriddatalen + 3, sizeof(Id));
	      data->attriddatalen--;	/* overwrite terminating 0  */
	    }
	  else
	    {
	      /* too bad. move to back. */
	      data->attriddata = sat_realloc2(data->attriddata, data->attriddatalen + oldsize + 4, sizeof(Id));
	      memcpy(data->attriddata + data->attriddatalen, data->attriddata + pp[1], oldsize * sizeof(Id));
	      pp[1] = data->attriddatalen;
	      data->attriddatalen += oldsize;
	    }
	  data->attriddata[data->attriddatalen++] = dir;
	  data->attriddata[data->attriddatalen++] = num;
	  data->attriddata[data->attriddatalen++] = num2;
	  data->attriddata[data->attriddatalen++] = 0;
	  return;
	}
    }
  key.name = keyname;
  key.type = TYPE_DIRNUMNUMARRAY;
  key.size = 0;
  key.storage = KEY_STORAGE_INCORE;
  data->attriddata = sat_realloc2(data->attriddata, data->attriddatalen + 4, sizeof(Id));
  repodata_set(data, entry, &key, data->attriddatalen);
  data->attriddata[data->attriddatalen++] = dir;
  data->attriddata[data->attriddatalen++] = num;
  data->attriddata[data->attriddatalen++] = num2;
  data->attriddata[data->attriddatalen++] = 0;
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
  int i;
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

  nentry = data->end - data->start;
  addschema_prepare(data, schematacache);
  memset(&newincore, 0, sizeof(newincore));
  for (entry = 0; entry < nentry; entry++)
    {
      memset(seen, 0, data->nkeys * sizeof(Id));
      sp = schema;
      if (data->entryschemau8)
	oldschema = data->entryschemau8[entry];
      else
	oldschema = data->entryschema[entry];
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
	      newschema = 1;
	      continue;
	    }
	  seen[*keyp] = -1;
	  *sp++ = *keyp;
	  oldcount++;
	}
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
	{
	  schemaid = addschema(data, schema, schematacache);
	  if (schemaid > 255 && data->entryschemau8)
	    {
	      data->entryschema = sat_malloc2(nentry, sizeof(Id));
	      for (i = 0; i < nentry; i++)
		data->entryschema[i] = data->entryschemau8[i];
	      data->entryschemau8 = sat_free(data->entryschemau8);
	    }
	  if (data->entryschemau8)
	    data->entryschemau8[entry] = schemaid;
	  else
	    data->entryschema[entry] = schemaid;
	}
      else
	schemaid = oldschema;


      /* now create data blob */
      dp = data->incoredata + data->incoreoffset[entry];
      data->incoreoffset[entry] = newincore.len;
      for (keyp = data->schemadata + data->schemata[schemaid]; *keyp; keyp++)
	{
	  key = data->keys + *keyp;
	  ndp = dp;
	  if (oldcount)
	    {
	      if (key->storage == KEY_STORAGE_VERTICAL_OFFSET)
		{
		  ndp = data_skip(dp, TYPE_ID);
		  ndp = data_skip(ndp, TYPE_ID);
		}
	      else if (key->storage == KEY_STORAGE_INCORE)
		ndp = data_skip(dp, key->type);
	      oldcount--;
	    }
	  if (seen[*keyp] == -1)
	    {
	      if (dp != ndp)
		data_addblob(&newincore, dp, ndp - dp);
	      seen[*keyp] = 0;
	    }
	  else if (seen[*keyp])
	    {
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
		case TYPE_VOID:
		case TYPE_CONSTANT:
		  break;
		case TYPE_STR:
		  data_addblob(xd, data->attrdata + id, strlen((char *)(data->attrdata + id)) + 1);
		  break;
		case TYPE_ID:
		case TYPE_NUM:
		case TYPE_DIR:
		  data_addid(xd, id);
		  break;
		case TYPE_DIRNUMNUMARRAY:
		  for (ida = data->attriddata + id; *ida; ida += 3)
		    {
		      data_addid(xd, ida[0]);
		      data_addid(xd, ida[1]);
		      data_addideof(xd, ida[2], ida[3] ? 0 : 1);
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
    }
  data->incoredata = newincore.buf;
  data->incoredatalen = newincore.len;
  data->incoredatafree = 0;
  
  data->vincore = newvincore.buf;
  data->vincorelen = newvincore.len;

  data->attrs = sat_free(data->attrs);
  data->attrdata = sat_free(data->attrdata);
  data->attrdatalen = 0;
}

Id
repodata_str2dir(Repodata *data, const char *dir, int create)
{
  Id id, parent;
  const char *dire;

  parent = 0;
  while (*dir == '/' && dir[1] == '/')
    dir++;
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

unsigned int
repodata_compress_page(unsigned char *page, unsigned int len, unsigned char *cpage, unsigned int max)
{
  return compress_buf(page, len, cpage, max);
}

/* Try to either setup on-demand paging (using FP as backing
   file), or in case that doesn't work (FP not seekable) slurps in
   all pages and deactivates paging.  */

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
      fprintf (stderr, "non matching page size\n");
      exit (1);
    }
  can_seek = 1;
  if ((cur_file_ofs = ftell(fp)) < 0)
    can_seek = 0;
  clearerr (fp);
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
	      exit (1);
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
	      perror ("fread");
	      exit (1);
	    }
	  if (compressed)
	    {
	      out_len = unchecked_decompress_buf(buf, in_len, dest, BLOB_PAGESIZE);
	      if (out_len != BLOB_PAGESIZE
	          && i < npages - 1)
	        {
	          fprintf (stderr, "can't decompress\n");
	          exit (1);
	        }
	    }
	}
    }

  if (can_seek)
    {
      /* If we are here we were able to seek to all page
         positions, so activate paging by copying FP into our structure.
	 We dup() the file, so that our callers can fclose() it and we
	 still have it open.  But this means that we share file positions
	 with the input filedesc.  So in case our caller reads it after us,
	 and calls back into us we might change the file position unexpectedly
	 to him.  */
      int fd = dup (fileno (fp));
      if (fd < 0)
        {
	  /* Jeez!  What a bloody system, we can't dup() anymore.  */
	  perror ("dup");
	  exit (1);
	}
      /* XXX we don't close this yet anywhere.  */
      data->fp = fdopen (fd, "r");
      if (!data->fp)
        {
	  /* My God!  What happened now?  */
	  perror ("fdopen");
	  exit (1);
	}
    }
}
