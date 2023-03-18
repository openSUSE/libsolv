/*
 * Copyright (c) 2007, Novell Inc.
 *
 * This program is licensed under the BSD license, read LICENSE.BSD
 * for further information
 */

/*
 * repo_solv.c
 *
 * Add a repo in solv format
 *
 */



#include <stdio.h>
#include <stdlib.h>
#include <unistd.h>
#include <string.h>

#include "repo_solv.h"
#include "util.h"

#include "repopack.h"
#include "repopage.h"

#include "poolid_private.h"	/* WHATPROVIDES_BLOCK */

#define INTERESTED_START	SOLVABLE_NAME
#define INTERESTED_END		SOLVABLE_ENHANCES

#define SOLV_ERROR_NOT_SOLV	1
#define SOLV_ERROR_UNSUPPORTED	2
#define SOLV_ERROR_EOF		3
#define SOLV_ERROR_ID_RANGE	4
#define SOLV_ERROR_OVERFLOW	5
#define SOLV_ERROR_CORRUPT	6



/*******************************************************************************
 * functions to extract data from a file handle
 */

/*
 * read u32
 */

static unsigned int
read_u32(Repodata *data)
{
  int c, i;
  unsigned int x = 0;

  if (data->error)
    return 0;
  for (i = 0; i < 4; i++)
    {
      c = getc(data->fp);
      if (c == EOF)
	{
	  data->error = pool_error(data->repo->pool, SOLV_ERROR_EOF, "unexpected EOF");
	  return 0;
	}
      x = (x << 8) | c;
    }
  return x;
}


/*
 * read u8
 */

static unsigned int
read_u8(Repodata *data)
{
  int c;

  if (data->error)
    return 0;
  c = getc(data->fp);
  if (c == EOF)
    {
      data->error = pool_error(data->repo->pool, SOLV_ERROR_EOF, "unexpected EOF");
      return 0;
    }
  return c;
}


/*
 * read Id
 */

static Id
read_id(Repodata *data, Id max)
{
  unsigned int x = 0;
  int c, i;

  if (data->error)
    return 0;
  for (i = 0; i < 5; i++)
    {
      c = getc(data->fp);
      if (c == EOF)
	{
	  data->error = pool_error(data->repo->pool, SOLV_ERROR_EOF, "unexpected EOF");
	  return 0;
	}
      if (!(c & 128))
	{
	  x = (x << 7) | c;
	  if (max && x >= (unsigned int)max)
	    {
	      data->error = pool_error(data->repo->pool, SOLV_ERROR_ID_RANGE, "read_id: id too large (%u/%u)", x, max);
	      return 0;
	    }
	  return x;
	}
      x = (x << 7) ^ c ^ 128;
    }
  data->error = pool_error(data->repo->pool, SOLV_ERROR_CORRUPT, "read_id: id too long");
  return 0;
}


static Id *
read_idarray(Repodata *data, Id max, Id *map, Id *store, Id *end)
{
  unsigned int x = 0;
  int c;

  if (data->error)
    return 0;
  for (;;)
    {
      c = getc(data->fp);
      if (c == EOF)
	{
	  data->error = pool_error(data->repo->pool, SOLV_ERROR_EOF, "unexpected EOF");
	  return 0;
	}
      if ((c & 128) != 0)
	{
	  x = (x << 7) ^ c ^ 128;
	  continue;
	}
      x = (x << 6) | (c & 63);
      if (max && x >= (unsigned int)max)
	{
	  data->error = pool_error(data->repo->pool, SOLV_ERROR_ID_RANGE, "read_idarray: id too large (%u/%u)", x, max);
	  return 0;
	}
      if (map)
	x = map[x];
      if (store == end)
	{
	  data->error = pool_error(data->repo->pool, SOLV_ERROR_OVERFLOW, "read_idarray: array overflow");
	  return 0;
	}
      *store++ = x;
      if ((c & 64) == 0)
	{
	  if (x == 0)	/* already have trailing zero? */
	    return store;
	  if (store == end)
	    {
	      data->error = pool_error(data->repo->pool, SOLV_ERROR_OVERFLOW, "read_idarray: array overflow");
	      return 0;
	    }
	  *store++ = 0;
	  return store;
	}
      x = 0;
    }
}

static void
read_idarray_block(Repodata *data, Id *block, int size)
{
  unsigned char buf[65536 + 5 + 1], *bp = buf, *oldbp;
  unsigned char cbuf[65536 + 4];	/* can overshoot 4 bytes */
  int left = 0;
  int eof = 0;
  int clen, flags;
  Id x;
  for (;;)
    {
      if (left < 5 && !eof)
	{
	  if (left)
	    memmove(buf, bp, left);
	  bp = buf + left;
	  flags = read_u8(data);
          clen = read_u8(data);
          clen = (clen << 8) | read_u8(data);
	  if (data->error)
	    return;
	  if (!clen)
	    clen = 65536;
	  eof = flags & 0x80;
	  if (fread(flags & 0x40 ? cbuf : bp, clen, 1, data->fp) != 1)
	    {
	      data->error = pool_error(data->repo->pool, SOLV_ERROR_EOF, "unexpected EOF");
	      return;
	    }
	  if (flags & 0x40)	/* compressed block */
	    clen = repopagestore_decompress_page(cbuf, clen, bp, 65536);
	  bp = buf;
	  left += clen;
	  bp[left] = 0;		/* make data_read_id return */
	  continue;
	}
      if (size < 2)
	{
	  data->error = pool_error(data->repo->pool, SOLV_ERROR_EOF, "idarray data overrun in block decompression");
	  return;
	}
      oldbp = bp;
      bp = data_read_id(bp, &x);
      left -= bp - oldbp;
      if (left < 0)
	{
	  data->error = pool_error(data->repo->pool, SOLV_ERROR_EOF, "compression buffer underrun");
	  return;
	}
      size--;
      *block++ = (x & 63) + (((unsigned int)x & ~127) >> 1) + 1;
      if ((x & 64) == 0)
	{
          *block++ = 0;
	  if (--size == 0)
	    break;
	}
    }
  if (left || !eof)
    data->error = pool_error(data->repo->pool, SOLV_ERROR_EOF, "idarray size overrun in block decompression");
}

/*******************************************************************************
 * functions to extract data from memory
 */

/*
 * read array of Ids
 */

static inline unsigned char *
data_read_id_max(unsigned char *dp, Id *ret, Id *map, int max, Repodata *data)
{
  Id x;
  dp = data_read_id(dp, &x);
  if (x < 0 || (max && x >= max))
    {
      data->error = pool_error(data->repo->pool, SOLV_ERROR_ID_RANGE, "data_read_id_max: id too large (%u/%u)", x, max);
      x = 0;
    }
  *ret = map ? map[x] : x;
  return dp;
}

static unsigned char *
data_read_idarray(unsigned char *dp, Id **storep, Id *map, int max, Repodata *data)
{
  Id *store = *storep;
  unsigned int x = 0;
  int c;

  for (;;)
    {
      c = *dp++;
      if ((c & 128) != 0)
	{
	  x = (x << 7) ^ c ^ 128;
	  continue;
	}
      x = (x << 6) | (c & 63);
      if (max && x >= (unsigned int)max)
	{
	  data->error = pool_error(data->repo->pool, SOLV_ERROR_ID_RANGE, "data_read_idarray: id too large (%u/%u)", x, max);
	  data->error = SOLV_ERROR_ID_RANGE;
	  break;
	}
      *store++ = map ? map[x] : x;
      if ((c & 64) == 0)
        break;
      x = 0;
    }
  *store++ = 0;
  *storep = store;
  return dp;
}

static unsigned char *
data_read_rel_idarray(unsigned char *dp, Id **storep, Id *map, int max, Repodata *data, Id keyid)
{
  Id marker = 0;
  Id *store = *storep;
  Id old = 0;
  unsigned int x = 0;
  int c;

  if (keyid == SOLVABLE_REQUIRES)
    marker = SOLVABLE_PREREQMARKER;
  if (keyid == SOLVABLE_PROVIDES)
    marker = SOLVABLE_FILEMARKER;
  for (;;)
    {
      c = *dp++;
      if ((c & 128) != 0)
	{
	  x = (x << 7) ^ c ^ 128;
	  continue;
	}
      x = (x << 6) | (c & 63);
      if (x == 0)
	{
	  if (!(c & 64))
	    break;
          if (marker)
	    *store++ = marker;
	  old = 0;
	  continue;
	}
      x = old + (x - 1);
      old = x;
      if (max && x >= (unsigned int)max)
	{
	  data->error = pool_error(data->repo->pool, SOLV_ERROR_ID_RANGE, "data_read_rel_idarray: id too large (%u/%u)", x, max);
	  break;
	}
      *store++ = map ? map[x] : x;
      if (!(c & 64))
        break;
      x = 0;
    }
  *store++ = 0;
  *storep = store;
  return dp;
}




/*******************************************************************************
 * functions to add data to our incore memory space
 */

#define INCORE_ADD_CHUNK 8192
#define DATA_READ_CHUNK 8192

static void
incore_add_id(Repodata *data, Id sx)
{
  unsigned int x = (unsigned int)sx;
  unsigned char *dp;
  /* make sure we have at least 5 bytes free */
  if (data->incoredatafree < 5)
    {
      data->incoredata = solv_realloc(data->incoredata, data->incoredatalen + INCORE_ADD_CHUNK);
      data->incoredatafree = INCORE_ADD_CHUNK;
    }
  dp = data->incoredata + data->incoredatalen;
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
  data->incoredatafree -= dp - (data->incoredata + data->incoredatalen);
  data->incoredatalen = dp - data->incoredata;
}

static void
incore_add_sizek(Repodata *data, unsigned int sx)
{
  if (sx < (1 << 22))
    incore_add_id(data, (Id)(sx << 10));
  else
    {
      if ((sx >> 25) != 0)
	{
	  incore_add_id(data, (Id)(sx >> 25));
	  data->incoredata[data->incoredatalen - 1] |= 128;
	}
      incore_add_id(data, (Id)((sx << 10) | 0x80000000));
      data->incoredata[data->incoredatalen - 5] = (sx >> 18) | 128;
    }
}

static void
incore_add_ideof(Repodata *data, Id sx, int eof)
{
  unsigned int x = (unsigned int)sx;
  unsigned char *dp;
  /* make sure we have at least 5 bytes free */
  if (data->incoredatafree < 5)
    {
      data->incoredata = solv_realloc(data->incoredata, data->incoredatalen + INCORE_ADD_CHUNK);
      data->incoredatafree = INCORE_ADD_CHUNK;
    }
  dp = data->incoredata + data->incoredatalen;
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
  data->incoredatafree -= dp - (data->incoredata + data->incoredatalen);
  data->incoredatalen = dp - data->incoredata;
}

static void
incore_add_blob(Repodata *data, unsigned char *buf, int len)
{
  if (data->incoredatafree < (unsigned int)len)
    {
      data->incoredata = solv_realloc(data->incoredata, data->incoredatalen + INCORE_ADD_CHUNK + len);
      data->incoredatafree = INCORE_ADD_CHUNK + len;
    }
  memcpy(data->incoredata + data->incoredatalen, buf, len);
  data->incoredatafree -= len;
  data->incoredatalen += len;
}

static void
incore_map_idarray(Repodata *data, unsigned char *dp, Id *map, Id max)
{
  /* We have to map the IDs, which might also change
     the necessary number of bytes, so we can't just copy
     over the blob and adjust it.  */
  for (;;)
    {
      Id id;
      int eof;
      dp = data_read_ideof(dp, &id, &eof);
      if (id < 0 || (max && id >= max))
	{
	  data->error = pool_error(data->repo->pool, SOLV_ERROR_ID_RANGE, "incore_map_idarray: id too large (%u/%u)", id, max);
	  break;
	}
      if (map)
        id = map[id];
      incore_add_ideof(data, id, eof);
      if (eof)
	break;
    }
}

static int
convert_idarray_block(Repodata *data, Id *block, Id *map, Id max)
{
  int cnt = 0;
  int old = 0;
  for (;;)
    {
      Id id = *block;
      cnt++;
      if (!id)
	return cnt;
      id--;	/* idarray_block unpacking added 1 */
      if (id < 2 * old)
	{
	  if (id & 1)
	    id = old - (id >> 1) - 1;
	  else
	    id = old + (id >> 1);
	}
      old = id;
      if (id < 0 || (max && id >= max))
	{
	  data->error = pool_error(data->repo->pool, SOLV_ERROR_ID_RANGE, "convert_idarray_block: id too large (%u/%u)", id, max);
	  return cnt;
	}
      if (map)
        id = map[id];
      *block++ = id;
    }
}

#if 0
static void
incore_add_u32(Repodata *data, unsigned int x)
{
  unsigned char *dp;
  /* make sure we have at least 4 bytes free */
  if (data->incoredatafree < 4)
    {
      data->incoredata = solv_realloc(data->incoredata, data->incoredatalen + INCORE_ADD_CHUNK);
      data->incoredatafree = INCORE_ADD_CHUNK;
    }
  dp = data->incoredata + data->incoredatalen;
  *dp++ = x >> 24;
  *dp++ = x >> 16;
  *dp++ = x >> 8;
  *dp++ = x;
  data->incoredatafree -= 4;
  data->incoredatalen += 4;
}

static void
incore_add_u8(Repodata *data, unsigned int x)
{
  unsigned char *dp;
  /* make sure we have at least 1 byte free */
  if (data->incoredatafree < 1)
    {
      data->incoredata = solv_realloc(data->incoredata, data->incoredatalen + 1024);
      data->incoredatafree = 1024;
    }
  dp = data->incoredata + data->incoredatalen;
  *dp++ = x;
  data->incoredatafree--;
  data->incoredatalen++;
}
#endif


/*******************************************************************************
 * our main function
 */

/*
 * read repo from .solv file and add it to pool
 */

int
repo_add_solv(Repo *repo, FILE *fp, int flags)
{
  Pool *pool = repo->pool;
  int i, l;
  int numid, numrel, numdir, numsolv;
  int numkeys, numschemata;

  Offset sizeid;
  Offset *str;			       /* map Id -> Offset into string space */
  char *strsp;			       /* repo string space */
  char *sp;			       /* pointer into string space */
  Id *idmap;			       /* map of repo Ids to pool Ids */
  Id id, type;
  Hashval hashmask, h, hh;
  Hashtable hashtbl;
  Id name, evr, did;
  int relflags;
  Reldep *ran;
  unsigned int size_idarray;
  Id *idarraydatap, *idarraydataend;
  Offset ido;
  Solvable *s;
  unsigned int solvflags;
  unsigned int solvversion;
  Repokey *keys;
  Id *schemadata, *schemadatap, *schemadataend;
  Id *schemata, key, *keyp;
  int nentries;
  int have_incoredata;
  int maxsize, allsize;
  unsigned char *buf, *bufend, *dp, *dps;
  Id stack[3 * 5];
  int keydepth;
  int needchunk;	/* need a new chunk of data */
  unsigned int now;
  int oldnstrings = pool->ss.nstrings;
  int oldnrels = pool->nrels;

  struct s_Stringpool *spool;

  Repodata *parent = 0;
  Repodata data;

  int extendstart = 0, extendend = 0;	/* set in case we're extending */
  int idarray_block_offset = 0;
  int idarray_block_end = 0;

  now = solv_timems(0);

  if ((flags & REPO_USE_LOADING) != 0)
    {
      /* this is a stub replace operation */
      flags |= REPO_EXTEND_SOLVABLES;
      /* use REPO_REUSE_REPODATA hack so that the old repodata is kept */
      parent = repo_add_repodata(repo, flags | REPO_REUSE_REPODATA);
      extendstart = parent->start;
      extendend = parent->end;
    }
  else if (flags & REPO_EXTEND_SOLVABLES)
    {
      /* extend all solvables of this repo */
      extendstart = repo->start;
      extendend = repo->end;
    }

  memset(&data, 0, sizeof(data));
  data.repo = repo;
  data.fp = fp;
  repopagestore_init(&data.store);

  if (read_u32(&data) != ('S' << 24 | 'O' << 16 | 'L' << 8 | 'V'))
     return pool_error(pool, SOLV_ERROR_NOT_SOLV, "not a SOLV file");
  solvversion = read_u32(&data);
  switch (solvversion)
    {
      case SOLV_VERSION_8:
      case SOLV_VERSION_9:
	break;
      default:
        return pool_error(pool, SOLV_ERROR_UNSUPPORTED, "unsupported SOLV version");
    }

  numid = (int)read_u32(&data);
  numrel = (int)read_u32(&data);
  numdir = (int)read_u32(&data);
  numsolv = (int)read_u32(&data);
  numkeys = (int)read_u32(&data);
  numschemata = (int)read_u32(&data);
  solvflags = read_u32(&data);

  if (numid < 0 || numid >= 0x20000000)
    return pool_error(pool, SOLV_ERROR_CORRUPT, "bad number of ids");
  if (numrel < 0 || numrel >= 0x20000000)
    return pool_error(pool, SOLV_ERROR_CORRUPT, "bad number of rels");
  if (numdir && (numdir < 2 || numdir >= 0x20000000))
    return pool_error(pool, SOLV_ERROR_CORRUPT, "bad number of dirs");
  if (numsolv < 0 || numsolv >= 0x20000000)
    return pool_error(pool, SOLV_ERROR_CORRUPT, "bad number of solvables");
  if (numkeys < 0 || numkeys >= 0x20000000)
    return pool_error(pool, SOLV_ERROR_CORRUPT, "bad number of keys");
  if (numschemata < 0 || numschemata >= 0x20000000)
    return pool_error(pool, SOLV_ERROR_CORRUPT, "bad number of schematas");

  if (numrel && (flags & REPO_LOCALPOOL) != 0)
    return pool_error(pool, SOLV_ERROR_CORRUPT, "relations are forbidden in a local pool");
  if ((flags & REPO_EXTEND_SOLVABLES) && numsolv)
    {
      /* make sure that we exactly replace the stub repodata */
      if (extendend - extendstart != numsolv)
	return pool_error(pool, SOLV_ERROR_CORRUPT, "sub-repository solvable number does not match main repository (%d - %d)", extendend - extendstart, numsolv);
      for (i = 0; i < numsolv; i++)
	if (pool->solvables[extendstart + i].repo != repo)
	  return pool_error(pool, SOLV_ERROR_CORRUPT, "main repository contains holes, cannot extend");
    }

  /*******  Part 0: skip optional userdata ******************************/

  if (solvflags & SOLV_FLAG_USERDATA)
    {
      unsigned int userdatalen = read_u32(&data);
      if (userdatalen >= 65536)
        return pool_error(pool, SOLV_ERROR_CORRUPT, "illegal userdata length");
      while (userdatalen--)
	if (getc(data.fp) == EOF)
	  return pool_error(pool, SOLV_ERROR_EOF, "unexpected EOF");
    }

  /*******  Part 1: string IDs  *****************************************/

  sizeid = read_u32(&data);	       /* size of string space */

  /*
   * read strings and Ids
   *
   */


  /*
   * alloc buffers
   */

  if (!(flags & REPO_LOCALPOOL))
    {
      spool = &pool->ss;
      /* alloc max needed string buffer and string pointers, will shrink again later */
#if 0
      spool->stringspace = solv_realloc(spool->stringspace, spool->sstrings + sizeid + 1);
      spool->strings = solv_realloc2(spool->strings, spool->nstrings + numid, sizeof(Offset));
#else
      spool->sstrings += sizeid + 1;
      spool->nstrings += numid;
      stringpool_shrink(spool);		/* we misuse stringpool_shrink so that the correct BLOCK factor is used */
      spool->sstrings -= sizeid + 1;
      spool->nstrings -= numid;
#endif
    }
  else
    {
      data.localpool = 1;
      spool = &data.spool;
      spool->stringspace = solv_malloc(7 + sizeid + 1);
      spool->strings = solv_malloc2(numid < 2 ?  2 : numid, sizeof(Offset));
      strcpy(spool->stringspace, "<NULL>");
      spool->sstrings = 7;
      spool->nstrings = 1;
      spool->strings[0] = 0;	/* <NULL> */
    }


  /*
   * read string data and append to old string space
   */

  strsp = spool->stringspace + spool->sstrings;	/* append new entries */
  if ((solvflags & SOLV_FLAG_PREFIX_POOL) == 0)
    {
      if (sizeid && fread(strsp, sizeid, 1, fp) != 1)
	{
	  repodata_freedata(&data);
	  return pool_error(pool, SOLV_ERROR_EOF, "read error while reading strings");
	}
    }
  else
    {
      unsigned int pfsize = read_u32(&data);
      char *prefix = solv_malloc(pfsize);
      char *pp = prefix;
      char *old_str = strsp;
      char *dest = strsp;
      int freesp = sizeid;

      if (pfsize && fread(prefix, pfsize, 1, fp) != 1)
	{
	  solv_free(prefix);
	  repodata_freedata(&data);
	  return pool_error(pool, SOLV_ERROR_EOF, "read error while reading strings");
	}
      for (i = 1; i < numid; i++)
        {
	  int same = (unsigned char)*pp++;
	  size_t len = strlen(pp) + 1;
	  freesp -= same + len;
	  if (freesp < 0)
	    {
	      solv_free(prefix);
	      repodata_freedata(&data);
	      return pool_error(pool, SOLV_ERROR_OVERFLOW, "overflow while expanding strings");
	    }
	  if (same)
	    memcpy(dest, old_str, same);
	  memcpy(dest + same, pp, len);
	  pp += len;
	  old_str = dest;
	  dest += same + len;
	}
      solv_free(prefix);
      if (freesp != 0)
	{
	  repodata_freedata(&data);
	  return pool_error(pool, SOLV_ERROR_CORRUPT, "expanding strings size mismatch");
	}
    }
  strsp[sizeid] = 0;		       /* make string space \0 terminated */
  sp = strsp;

  /* now merge */
  str = spool->strings;			/* array of offsets into strsp, indexed by Id */
  if ((flags & REPO_LOCALPOOL) != 0)
    {
      /* no shared pool, thus no idmap and no unification needed */
      idmap = 0;
      spool->nstrings = numid < 2 ? 2 : numid;	/* make sure we have at least id 0 and 1 */
      if (*sp)
	{
	  /* we need id 1 to be '' for directories */
	  repodata_freedata(&data);
	  return pool_error(pool, SOLV_ERROR_CORRUPT, "store strings don't start with an empty string");
	}
      for (i = 1; i < spool->nstrings; i++)
	{
	  if (sp >= strsp + sizeid && numid >= 2)
	    {
	      repodata_freedata(&data);
	      return pool_error(pool, SOLV_ERROR_OVERFLOW, "not enough strings");
	    }
	  str[i] = sp - spool->stringspace;
	  sp += strlen(sp) + 1;
	}
      spool->sstrings = sp - spool->stringspace;
    }
  else
    {
      Offset oldsstrings = spool->sstrings;

      /* alloc id map for name and rel Ids. this maps ids in the solv files
       * to the ids in our pool */
      idmap = solv_calloc(numid + numrel, sizeof(Id));
      stringpool_resize_hash(spool, numid);
      hashtbl = spool->stringhashtbl;
      hashmask = spool->stringhashmask;
#if 0
      POOL_DEBUG(SOLV_DEBUG_STATS, "read %d strings\n", numid);
      POOL_DEBUG(SOLV_DEBUG_STATS, "string hash buckets: %d\n", hashmask + 1);
#endif
      /*
       * run over strings and merge with pool.
       * we could use stringpool_str2id, but this is faster.
       * also populate id map (maps solv Id -> pool Id)
       */
      for (i = 1; i < numid; i++)
	{
	  if (sp >= strsp + sizeid)
	    {
	      solv_free(idmap);
	      spool->nstrings = oldnstrings;
	      spool->sstrings = oldsstrings;
	      stringpool_freehash(spool);
	      repodata_freedata(&data);
	      return pool_error(pool, SOLV_ERROR_OVERFLOW, "not enough strings %d %d", i, numid);
	    }
	  if (!*sp)			       /* empty string */
	    {
	      idmap[i] = ID_EMPTY;
	      sp++;
	      continue;
	    }

	  /* find hash slot */
	  h = strhash(sp) & hashmask;
	  hh = HASHCHAIN_START;
	  for (;;)
	    {
	      id = hashtbl[h];
	      if (!id)
		break;
	      if (!strcmp(spool->stringspace + spool->strings[id], sp))
		break;		/* already in pool */
	      h = HASHCHAIN_NEXT(h, hh, hashmask);
	    }

	  /* length == offset to next string */
	  l = strlen(sp) + 1;
	  if (!id)	       /* end of hash chain -> new string */
	    {
	      id = spool->nstrings++;
	      hashtbl[h] = id;
	      str[id] = spool->sstrings;	/* save offset */
	      if (sp != spool->stringspace + spool->sstrings)
		memmove(spool->stringspace + spool->sstrings, sp, l);
	      spool->sstrings += l;
	    }
	  idmap[i] = id;       /* repo relative -> pool relative */
	  sp += l;	       /* next string */
	}
      stringpool_shrink(spool);		/* vacuum */
    }


  /*******  Part 2: Relation IDs  ***************************************/

  /*
   * read RelDeps
   *
   */

  if (numrel)
    {
      /* extend rels */
      pool->rels = solv_realloc2(pool->rels, pool->nrels + numrel, sizeof(Reldep));
      ran = pool->rels;

      pool_resize_rels_hash(pool, numrel);
      hashtbl = pool->relhashtbl;
      hashmask = pool->relhashmask;
#if 0
      POOL_DEBUG(SOLV_DEBUG_STATS, "read %d rels\n", numrel);
      POOL_DEBUG(SOLV_DEBUG_STATS, "rel hash buckets: %d\n", hashmask + 1);
#endif

      /*
       * read RelDeps from repo
       */
      for (i = 0; i < numrel; i++)
	{
	  name = read_id(&data, i + numid);	/* read (repo relative) Ids */
	  evr = read_id(&data, i + numid);
	  relflags = read_u8(&data);
	  name = idmap[name];		/* map to (pool relative) Ids */
	  evr = idmap[evr];
	  h = relhash(name, evr, relflags) & hashmask;
	  hh = HASHCHAIN_START;
	  for (;;)
	    {
	      id = hashtbl[h];
	      if (!id)		/* end of hash chain reached */
		break;
	      if (ran[id].name == name && ran[id].evr == evr && ran[id].flags == relflags)
		break;
	      h = HASHCHAIN_NEXT(h, hh, hashmask);
	    }
	  if (!id)		/* new RelDep */
	    {
	      id = pool->nrels++;
	      hashtbl[h] = id;
	      ran[id].name = name;
	      ran[id].evr = evr;
	      ran[id].flags = relflags;
	    }
	  idmap[i + numid] = MAKERELDEP(id);   /* fill Id map */
	}
      pool_shrink_rels(pool);		/* vacuum */
    }

  /* if we added ids/rels, make room in our whatprovide arrays */
  if (!(flags & REPO_LOCALPOOL))
    {
      if (pool->whatprovides && oldnstrings != pool->ss.nstrings)
	{
	  int newlen = (pool->ss.nstrings + WHATPROVIDES_BLOCK) & ~WHATPROVIDES_BLOCK;
	  pool->whatprovides = solv_realloc2(pool->whatprovides, newlen, sizeof(Offset));
	  memset(pool->whatprovides + oldnstrings, 0, (newlen - oldnstrings) * sizeof(Offset));
	}
      if (pool->whatprovides_rel && oldnrels != pool->nrels)
	{
	  int newlen = (pool->nrels + WHATPROVIDES_BLOCK) & ~WHATPROVIDES_BLOCK;
	  pool->whatprovides_rel = solv_realloc2(pool->whatprovides_rel, newlen, sizeof(Offset));
	  memset(pool->whatprovides_rel + oldnrels, 0, (newlen - oldnrels) * sizeof(Offset));
	}
    }

  /*******  Part 3: Dirs  ***********************************************/
  if (numdir)
    {
      data.dirpool.dirs = solv_malloc2(numdir, sizeof(Id));
      data.dirpool.ndirs = numdir;
      data.dirpool.dirs[0] = 0;		/* dir 0: virtual root */
      data.dirpool.dirs[1] = 1;		/* dir 1: / */
      for (i = 2; i < numdir; i++)
	{
	  id = read_id(&data, i + numid);
	  if (id >= numid)
	    {
	      data.dirpool.dirs[i++] = -(id - numid);
	      if (i >= numdir)
		{
		  data.error = pool_error(pool, SOLV_ERROR_CORRUPT, "last dir entry is not a component");
		  break;
		}
	      id = read_id(&data, numid);
	    }
	  if (idmap)
	    id = idmap[id];
	  data.dirpool.dirs[i] = id;
	  if (id <= 0)
            data.error = pool_error(pool, SOLV_ERROR_CORRUPT, "bad dir component");
	}
    }

  /*******  Part 4: Keys  ***********************************************/

  keys = solv_calloc(numkeys, sizeof(*keys));
  /* keys start at 1 */
  for (i = 1; i < numkeys; i++)
    {
      Repokey *key;
      if (data.error)
	break;
      id = read_id(&data, numid);
      if (idmap)
	id = idmap[id];
      else if ((flags & REPO_LOCALPOOL) != 0)
        id = pool_str2id(pool, stringpool_id2str(spool, id), 1);
      type = read_id(&data, numid);
      if (idmap)
	type = idmap[type];
      else if ((flags & REPO_LOCALPOOL) != 0)
        type = pool_str2id(pool, stringpool_id2str(spool, type), 1);
      if (type < REPOKEY_TYPE_VOID || type > REPOKEY_TYPE_DELETED)
	{
	  data.error = pool_error(pool, SOLV_ERROR_UNSUPPORTED, "unsupported data type '%s'", pool_id2str(pool, type));
	  type = REPOKEY_TYPE_VOID;
	}
      key = keys + i;
      key->name = id;
      key->type = type;
      key->size = read_id(&data, type == REPOKEY_TYPE_CONSTANTID ? numid + numrel : 0);
      key->storage = read_id(&data, 0);
      /* old versions used SOLVABLE for main solvable data */
      if (key->storage != KEY_STORAGE_INCORE && key->storage != KEY_STORAGE_VERTICAL_OFFSET && key->storage != KEY_STORAGE_SOLVABLE && key->storage != KEY_STORAGE_IDARRAYBLOCK)
	data.error = pool_error(pool, SOLV_ERROR_UNSUPPORTED, "unsupported storage type %d", key->storage);
      /* change KEY_STORAGE_SOLVABLE to KEY_STORAGE_INCORE */
      if (key->storage == KEY_STORAGE_SOLVABLE)
	key->storage = KEY_STORAGE_INCORE;
      if (key->storage == KEY_STORAGE_IDARRAYBLOCK && type != REPOKEY_TYPE_IDARRAY)
	data.error = pool_error(pool, SOLV_ERROR_UNSUPPORTED, "typr %d does not support idarrayblock storage\n", type);
      if (id >= SOLVABLE_NAME && id <= RPM_RPMDBID)
	{
	  /* we will put those directly into the storable */
	  if (key->storage != KEY_STORAGE_INCORE && key->storage != KEY_STORAGE_IDARRAYBLOCK)
	    data.error = pool_error(pool, SOLV_ERROR_UNSUPPORTED, "main solvable data must use incore storage, not %d", key->storage);
	}
      if ((type == REPOKEY_TYPE_FIXARRAY || type == REPOKEY_TYPE_FLEXARRAY) && key->storage != KEY_STORAGE_INCORE)
	data.error = pool_error(pool, SOLV_ERROR_UNSUPPORTED, "flex/fixarrays must use incore storage\n");
      /* cannot handle rel idarrays in incore/vertical */
      if (type == REPOKEY_TYPE_REL_IDARRAY && keys[i].storage != KEY_STORAGE_INCORE)
	data.error = pool_error(pool, SOLV_ERROR_UNSUPPORTED, "type REL_IDARRAY is only supported for STORAGE_INCORE");
      /* cannot handle mapped ids in vertical */
      if (!(flags & REPO_LOCALPOOL) && key->storage == KEY_STORAGE_VERTICAL_OFFSET && (type == REPOKEY_TYPE_ID || type == REPOKEY_TYPE_IDARRAY))
	data.error = pool_error(pool, SOLV_ERROR_UNSUPPORTED, "mapped ids are not supported for STORAGE_VERTICAL_OFFSET");

      if (type == REPOKEY_TYPE_CONSTANTID && idmap)
	key->size = idmap[key->size];
#if 0
      fprintf(stderr, "key %d %s %s %d %d\n", i, pool_id2str(pool, id), pool_id2str(pool, type), key->size, key->storage);
#endif
    }

  have_incoredata = 0;
  for (i = 1; i < numkeys; i++)
    {
      id = keys[i].name;
      if (id == REPOSITORY_SOLVABLES && keys[i].type == REPOKEY_TYPE_FLEXARRAY)
	continue;
      if (id >= SOLVABLE_NAME && id <= RPM_RPMDBID)
	continue;
      have_incoredata = 1;
    }

  data.keys = keys;
  data.nkeys = numkeys;
  for (i = 1; i < numkeys; i++)
    {
      id = keys[i].name;
      data.keybits[(id >> 3) & (sizeof(data.keybits) - 1)] |= 1 << (id & 7);
    }

  /*******  Part 5: Schemata ********************************************/

  id = read_id(&data, 0);
  schemadata = solv_calloc(id + 1, sizeof(Id));
  schemadatap = schemadata + 1;
  schemadataend = schemadatap + id;
  schemata = solv_calloc(numschemata, sizeof(Id));
  for (i = 1; i < numschemata; i++)
    {
      schemata[i] = schemadatap - schemadata;
      schemadatap = read_idarray(&data, numid, 0, schemadatap, schemadataend);
#if 0
      Id *sp = schemadata + schemata[i];
      fprintf(stderr, "schema %d:", i);
      for (; *sp; sp++)
        fprintf(stderr, " %d", *sp);
      fprintf(stderr, "\n");
#endif
    }
  data.schemata = schemata;
  data.nschemata = numschemata;
  data.schemadata = schemadata;
  data.schemadatalen = schemadataend - data.schemadata;

  /*******  Part 6: Idarray block ***********************************/
  if ((solvflags & SOLV_FLAG_IDARRAYBLOCK) != 0)
    {
      unsigned int idarray_block_size = read_id(&data, 0x30000000);
      repo_reserve_ids(repo, 0, idarray_block_size + 1);
      idarray_block_offset = repo->idarraysize;
      repo->idarraysize += idarray_block_size;
      idarray_block_end = repo->idarraysize;
      repo->idarraydata[repo->idarraysize++] = 0;
      if (idarray_block_size)
        read_idarray_block(&data, repo->idarraydata + idarray_block_offset, idarray_block_size);
    }

  /*******  Part 7: Data ********************************************/

  idarraydatap = idarraydataend = 0;
  size_idarray = 0;

  maxsize = read_id(&data, 0);
  allsize = read_id(&data, 0);
  maxsize += 5;	/* so we can read the next schema of an array */
  if (maxsize > allsize)
    maxsize = allsize;

  buf = solv_calloc(maxsize + DATA_READ_CHUNK + 4, 1);	/* 4 extra bytes to detect overflows */
  bufend = buf;
  dp = buf;

  l = maxsize;
  if (l < DATA_READ_CHUNK)
    l = DATA_READ_CHUNK;
  if (l > allsize)
    l = allsize;
  if (!l || fread(buf, l, 1, data.fp) != 1)
    {
      if (!data.error)
        data.error = pool_error(pool, SOLV_ERROR_EOF, "unexpected EOF");
      id = 0;
    }
  else
    {
      bufend = buf + l;
      allsize -= l;
      dp = data_read_id_max(dp, &id, 0, numschemata, &data);
    }

  incore_add_id(&data, 0);	/* so that incoreoffset 0 means schema 0 */
  incore_add_id(&data, id);	/* main schema id */
  keyp = schemadata + schemata[id];
  data.mainschema = id;
  for (i = 0; keyp[i]; i++)
    ;
  if (i)
    data.mainschemaoffsets = solv_calloc(i, sizeof(Id));

  nentries = 0;
  keydepth = 0;
  s = 0;
  needchunk = 1;
  for(;;)
    {
      /* make sure we have enough room */
      if (keydepth == 0 || needchunk)
	{
	  int left = bufend - dp;
	  /* read data chunk to dp */
	  if (data.error)
	    break;
	  if (left < 0)
	    {
              data.error = pool_error(pool, SOLV_ERROR_EOF, "buffer overrun");
	      break;
	    }
	  if (left < maxsize)
	    {
	      if (left)
		memmove(buf, dp, left);
	      l = maxsize - left;
	      if (l < DATA_READ_CHUNK)
		l = DATA_READ_CHUNK;
	      if (l > allsize)
		l = allsize;
	      if (l && fread(buf + left, l, 1, data.fp) != 1)
		{
		  data.error = pool_error(pool, SOLV_ERROR_EOF, "unexpected EOF");
		  break;
		}
	      allsize -= l;
	      left += l;
	      bufend = buf + left;
	      if (allsize + left < maxsize)
		maxsize = allsize + left;
	      dp = buf;
	    }
	  needchunk = 0;
	}

      key = *keyp++;
#if 0
printf("key %d at %d\n", key, (int)(keyp - 1 - schemadata));
#endif
      if (!key)
	{
	  if (keydepth <= 3)
	    needchunk = 1;
	  if (nentries)
	    {
	      if (s && keydepth == 3)
		{
		  s++;	/* next solvable */
	          if (have_incoredata)
		    data.incoreoffset[(s - pool->solvables) - data.start] = data.incoredatalen;
		}
	      id = stack[keydepth - 1];
	      if (!id)
		{
		  dp = data_read_id_max(dp, &id, 0, numschemata, &data);
		  incore_add_id(&data, id);
		}
	      keyp = schemadata + schemata[id];
	      nentries--;
	      continue;
	    }
	  if (!keydepth)
	    break;
	  --keydepth;
	  keyp = schemadata + stack[--keydepth];
	  nentries = stack[--keydepth];
#if 0
printf("pop flexarray %d %d\n", keydepth, nentries);
#endif
	  if (!keydepth && s)
	    s = 0;	/* back from solvables */
	  continue;
	}

      if (keydepth == 0)
	data.mainschemaoffsets[keyp - 1 - (schemadata + schemata[data.mainschema])] = data.incoredatalen;

#if 0
printf("=> %s %s %p\n", pool_id2str(pool, keys[key].name), pool_id2str(pool, keys[key].type), s);
#endif
      id = keys[key].name;
      if (keys[key].storage == KEY_STORAGE_VERTICAL_OFFSET)
	{
	  dps = dp;
	  dp = data_skip(dp, REPOKEY_TYPE_ID);
	  dp = data_skip(dp, REPOKEY_TYPE_ID);
	  incore_add_blob(&data, dps, dp - dps);	/* just record offset/size */
	  continue;
	}
      switch (keys[key].type)
	{
	case REPOKEY_TYPE_ID:
	  dp = data_read_id_max(dp, &did, idmap, numid + numrel, &data);
	  if (s && id == SOLVABLE_NAME)
	    s->name = did;
	  else if (s && id == SOLVABLE_ARCH)
	    s->arch = did;
	  else if (s && id == SOLVABLE_EVR)
	    s->evr = did;
	  else if (s && id == SOLVABLE_VENDOR)
	    s->vendor = did;
	  else if (keys[key].storage == KEY_STORAGE_INCORE)
	    incore_add_id(&data, did);
#if 0
	  POOL_DEBUG(SOLV_DEBUG_STATS, "%s -> %s\n", pool_id2str(pool, id), pool_id2str(pool, did));
#endif
	  break;
	case REPOKEY_TYPE_IDARRAY:
	case REPOKEY_TYPE_REL_IDARRAY:
	  if (keys[key].storage == KEY_STORAGE_IDARRAYBLOCK)
	    {
	      int cnt = convert_idarray_block(&data, repo->idarraydata + idarray_block_offset, idmap, numid + numrel);
	      ido = idarray_block_offset;
	      idarray_block_offset += cnt;
	      if (idarray_block_offset > idarray_block_end)
		{
		  data.error = pool_error(pool, SOLV_ERROR_OVERFLOW, "idarray block underflow");
		  idarray_block_offset = idarray_block_end;
		  break;
		}
	      if (!s || id < INTERESTED_START || id > INTERESTED_END)
		{
		  do
		    incore_add_ideof(&data, repo->idarraydata[ido++], --cnt > 1 ? 0 : 1);
		  while (cnt > 1);
		  break;
		}
	    }
	  else
	    {
	      if (!s || id < INTERESTED_START || id > INTERESTED_END)
		{
		  dps = dp;
		  dp = data_skip(dp, REPOKEY_TYPE_IDARRAY);
		  if (keys[key].storage != KEY_STORAGE_INCORE)
		    break;
		  if (idmap)
		    incore_map_idarray(&data, dps, idmap, numid + numrel);
		  else
		    incore_add_blob(&data, dps, dp - dps);
		  break;
		}
	      ido = idarraydatap - repo->idarraydata;
	      if (keys[key].type == REPOKEY_TYPE_IDARRAY)
		dp = data_read_idarray(dp, &idarraydatap, idmap, numid + numrel, &data);
	      else
		dp = data_read_rel_idarray(dp, &idarraydatap, idmap, numid + numrel, &data, id);
	      if (idarraydatap > idarraydataend)
		{
		  data.error = pool_error(pool, SOLV_ERROR_OVERFLOW, "idarray overflow");
		  break;
		}
	    }
	  if (id == SOLVABLE_PROVIDES)
	    s->provides = ido;
	  else if (id == SOLVABLE_OBSOLETES)
	    s->obsoletes = ido;
	  else if (id == SOLVABLE_CONFLICTS)
	    s->conflicts = ido;
	  else if (id == SOLVABLE_REQUIRES)
	    s->requires = ido;
	  else if (id == SOLVABLE_RECOMMENDS)
	    s->recommends= ido;
	  else if (id == SOLVABLE_SUPPLEMENTS)
	    s->supplements = ido;
	  else if (id == SOLVABLE_SUGGESTS)
	    s->suggests = ido;
	  else if (id == SOLVABLE_ENHANCES)
	    s->enhances = ido;
#if 0
	  POOL_DEBUG(SOLV_DEBUG_STATS, "%s ->\n", pool_id2str(pool, id));
	  for (; repo->idarraydata[ido]; ido++)
	    POOL_DEBUG(SOLV_DEBUG_STATS,"  %s\n", pool_dep2str(pool, repo->idarraydata[ido]));
#endif
	  break;
	case REPOKEY_TYPE_FIXARRAY:
	case REPOKEY_TYPE_FLEXARRAY:
	  if (!keydepth)
	    needchunk = 1;
          if (keydepth == sizeof(stack)/sizeof(*stack))
	    {
	      data.error = pool_error(pool, SOLV_ERROR_OVERFLOW, "array stack overflow");
	      break;
	    }
	  stack[keydepth++] = nentries;
	  stack[keydepth++] = keyp - schemadata;
	  stack[keydepth++] = 0;
	  dp = data_read_id_max(dp, &nentries, 0, 0, &data);
	  incore_add_id(&data, nentries);
	  if (!nentries)
	    {
	      /* zero size array? */
	      keydepth -= 2;
	      nentries = stack[--keydepth];
	      break;
	    }
	  if (keydepth == 3 && id == REPOSITORY_SOLVABLES)
	    {
	      /* horray! here come the solvables */
	      if (nentries != numsolv)
		{
		  data.error = pool_error(pool, SOLV_ERROR_CORRUPT, "inconsistent number of solvables: %d %d", nentries, numsolv);
		  break;
		}
	      if (idarraydatap)
		{
		  data.error = pool_error(pool, SOLV_ERROR_CORRUPT, "more than one solvable block");
		  break;
		}
	      if ((flags & REPO_EXTEND_SOLVABLES) != 0)
		s = pool_id2solvable(pool, extendstart);
	      else
		s = pool_id2solvable(pool, repo_add_solvable_block(repo, numsolv));
	      data.start = s - pool->solvables;
	      data.end = data.start + numsolv;
	      repodata_extend_block(&data, data.start, numsolv);
	      for (i = 1; i < numkeys; i++)
		{
		  id = keys[i].name;
		  if ((keys[i].type == REPOKEY_TYPE_IDARRAY || keys[i].type == REPOKEY_TYPE_REL_IDARRAY)
		      && id >= INTERESTED_START && id <= INTERESTED_END)
		    size_idarray += keys[i].size;
		}
	      /* allocate needed space in repo */
	      /* we add maxsize because it is an upper limit for all idarrays, thus we can't overflow */
	      repo_reserve_ids(repo, 0, size_idarray + maxsize + 1);
	      idarraydatap = repo->idarraydata + repo->idarraysize;
	      repo->idarraysize += size_idarray;
	      idarraydataend = idarraydatap + size_idarray;
	      repo->lastoff = 0;
	      if (have_incoredata)
		data.incoreoffset[(s - pool->solvables) - data.start] = data.incoredatalen;
	    }
	  nentries--;
	  dp = data_read_id_max(dp, &id, 0, numschemata, &data);
	  incore_add_id(&data, id);
	  if (keys[key].type == REPOKEY_TYPE_FIXARRAY)
	    {
	      if (!id)
		data.error = pool_error(pool, SOLV_ERROR_CORRUPT, "illegal fixarray");
	      stack[keydepth - 1] = id;
	    }
	  keyp = schemadata + schemata[id];
	  break;
	case REPOKEY_TYPE_NUM:
	  if (!(solvflags & SOLV_FLAG_SIZE_BYTES) && keys[key].storage == KEY_STORAGE_INCORE &&
		(id == SOLVABLE_INSTALLSIZE || id == SOLVABLE_DOWNLOADSIZE || id == DELTA_DOWNLOADSIZE))
	    {
	      /* old solv file with sizes in kilos. transcode. */
	      dp = data_read_id(dp, &id);
	      incore_add_sizek(&data, (unsigned int)id);
	      break;
	    }
	  if (s && id == RPM_RPMDBID)
	    {
	      dp = data_read_id(dp, &id);
	      if (!repo->rpmdbid)
		repo->rpmdbid = repo_sidedata_create(repo, sizeof(Id));
	      repo->rpmdbid[(s - pool->solvables) - repo->start] = id;
	      break;
	    }
	  /* FALLTHROUGH */
	default:
	  dps = dp;
	  dp = data_skip(dp, keys[key].type);
	  if (keys[key].storage == KEY_STORAGE_INCORE)
	    incore_add_blob(&data, dps, dp - dps);
	  break;
	}
    }
  /* should shrink idarraydata again */

  if (keydepth)
    data.error = pool_error(pool, SOLV_ERROR_EOF, "unexpected EOF, depth = %d", keydepth);
  if (!data.error)
    {
      if (dp > bufend)
	data.error = pool_error(pool, SOLV_ERROR_EOF, "buffer overrun");
      else if (idarray_block_offset != idarray_block_end)
	data.error = pool_error(pool, SOLV_ERROR_EOF, "unconsumed idarray block entries");
    }
  solv_free(buf);

  if (data.error)
    {
      /* free solvables */
      repo_free_solvable_block(repo, data.start, data.end - data.start, 1);
      /* free id array */
      repo->idarraysize -= size_idarray;
      /* free incore data */
      data.incoredata = solv_free(data.incoredata);
      data.incoredatalen = data.incoredatafree = 0;
    }

  if (data.incoredatafree)
    {
      /* shrink excess size */
      data.incoredata = solv_realloc(data.incoredata, data.incoredatalen);
      data.incoredatafree = 0;
    }
  solv_free(idmap);

  /* fixup key data */
  for (i = 1; i < numkeys; i++)
    {
      if (keys[i].type == REPOKEY_TYPE_REL_IDARRAY)
        keys[i].type = REPOKEY_TYPE_IDARRAY;
      if (keys[i].storage == KEY_STORAGE_IDARRAYBLOCK)
        keys[i].storage = KEY_STORAGE_INCORE;
      if (keys[i].name >= SOLVABLE_NAME && keys[i].name <= RPM_RPMDBID)
        keys[i].storage = KEY_STORAGE_SOLVABLE;
    }

  for (i = 1; i < numkeys; i++)
    if (keys[i].storage == KEY_STORAGE_VERTICAL_OFFSET && keys[i].size)
      break;
  if (i < numkeys && !data.error)
    {
      Id fileoffset = 0;
      unsigned int pagesize;

      /* we have vertical data, make it available */
      data.verticaloffset = solv_calloc(numkeys, sizeof(Id));
      for (i = 1; i < numkeys; i++)
        if (keys[i].storage == KEY_STORAGE_VERTICAL_OFFSET)
	  {
	    data.verticaloffset[i] = fileoffset;
	    fileoffset += keys[i].size;
	  }
      data.lastverticaloffset = fileoffset;
      pagesize = read_u32(&data);
      if (!data.error)
	{
	  data.error = repopagestore_read_or_setup_pages(&data.store, data.fp, pagesize, fileoffset);
	  if (data.error == SOLV_ERROR_EOF)
	    pool_error(pool, data.error, "repopagestore setup: unexpected EOF");
	  else if (data.error)
	    pool_error(pool, data.error, "repopagestore setup failed");
	}
    }
  data.fp = 0; /* no longer needed */

  if (data.error)
    {
      i = data.error;
      repodata_freedata(&data);
      return i;
    }

  if (parent)
    {
      /* overwrite stub repodata */
      repodata_freedata(parent);
      data.repodataid = parent->repodataid;
      data.loadcallback = parent->loadcallback;
      *parent = data;
    }
  else
    {
      /* make it available as new repodata */
      if (!repo->nrepodata)
	{
	  repo->nrepodata = 1;
	  repo->repodata = solv_calloc(2, sizeof(data));
	}
      else
        repo->repodata = solv_realloc2(repo->repodata, repo->nrepodata + 1, sizeof(data));
      data.repodataid = repo->nrepodata;
      repo->repodata[repo->nrepodata++] = data;
    }

  if ((flags & REPO_EXTEND_SOLVABLES) != 0)
    {
      if (repodata_has_keyname(&data, SOLVABLE_FILELIST))
	repodata_set_filelisttype(repo->repodata + data.repodataid, REPODATA_FILELIST_EXTENSION);
    }
  else
    {
      if (repodata_lookup_type(&data, SOLVID_META, REPOSITORY_FILTEREDFILELIST))
        repodata_set_filelisttype(repo->repodata + data.repodataid, REPODATA_FILELIST_FILTERED);
    }

  /* create stub repodata entries for all external */
  if (!(flags & SOLV_ADD_NO_STUBS) && !parent)
    {
      for (key = 1 ; key < data.nkeys; key++)
	if (data.keys[key].name == REPOSITORY_EXTERNAL && data.keys[key].type == REPOKEY_TYPE_FLEXARRAY)
	  break;
      if (key < data.nkeys)
	repodata_create_stubs(repo->repodata + data.repodataid);
    }

  POOL_DEBUG(SOLV_DEBUG_STATS, "repo_add_solv took %d ms\n", solv_timems(now));
  POOL_DEBUG(SOLV_DEBUG_STATS, "repo size: %d solvables\n", repo->nsolvables);
  POOL_DEBUG(SOLV_DEBUG_STATS, "repo memory used: %d K incore, %d K idarray\n", data.incoredatalen/1024, repo->idarraysize / (int)(1024/sizeof(Id)));
  return 0;
}

int
solv_read_userdata(FILE *fp, unsigned char **datap, int *lenp)
{
  unsigned char d[4 * 10], *ud = 0;
  unsigned int n;
  if (fread(d, sizeof(d), 1, fp) != 1)
    return SOLV_ERROR_EOF;
  n = d[0] << 24 | d[1] << 16 | d[2] << 8 | d[3];
  if (n != ('S' << 24 | 'O' << 16 | 'L' << 8 | 'V'))
    return SOLV_ERROR_NOT_SOLV;
  n = d[4] << 24 | d[5] << 16 | d[6] << 8 | d[7];
  switch(n)
    {
    case SOLV_VERSION_8:
    case SOLV_VERSION_9:
      break;
    default:
      return SOLV_ERROR_UNSUPPORTED;
    }
  n = d[32] << 24 | d[33] << 16 | d[34] << 8 | d[35];
  if (!(n & SOLV_FLAG_USERDATA))
    n = 0;
  else
    n = d[36] << 24 | d[37] << 16 | d[38] << 8 | d[39];
  if (n >= 65536)
    return SOLV_ERROR_CORRUPT;
  if (n)
    {
      ud = solv_malloc(n + 1);
      if (fread(ud, n, 1, fp) != 1)
	{
	  solv_free(ud);
	  return SOLV_ERROR_EOF;
	}
      ud[n] = 0;
    }
  *datap = ud;
  if (lenp)
    *lenp = (int)n;
  return 0;
}
