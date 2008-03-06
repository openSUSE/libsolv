/*
 * Copyright (c) 2007, Novell Inc.
 *
 * This program is licensed under the BSD license, read LICENSE.BSD
 * for further information
 */

/*
 * repo_solv.c
 * 
 * Read the binary dump of a Repo and create a Repo * from it
 * 
 *  See
 *   Repo *pool_addrepo_solv(Pool *pool, FILE *fp)
 * below
 * 
 */



#include <stdio.h>
#include <stdlib.h>
#include <unistd.h>
#include <string.h>

#include "repo_solv.h"
#include "util.h"

#include "repopack.h"

#define INTERESTED_START	SOLVABLE_NAME
#define INTERESTED_END		SOLVABLE_FRESHENS

#define SOLV_ERROR_NOT_SOLV	1
#define SOLV_ERROR_UNSUPPORTED	2
#define SOLV_ERROR_EOF		3
#define SOLV_ERROR_ID_RANGE	4
#define SOLV_ERROR_OVERFLOW	5
#define SOLV_ERROR_CORRUPT	6

static Pool *mypool;		/* for pool_debug... */

/*-----------------------------------------------------------------*/
/* .solv read functions */

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
	  pool_debug(mypool, SAT_ERROR, "unexpected EOF\n");
	  data->error = SOLV_ERROR_EOF;
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
      pool_debug(mypool, SAT_ERROR, "unexpected EOF\n");
      data->error = SOLV_ERROR_EOF;
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
          pool_debug(mypool, SAT_ERROR, "unexpected EOF\n");
	  data->error = SOLV_ERROR_EOF;
	  return 0;
	}
      if (!(c & 128))
	{
	  x = (x << 7) | c;
	  if (max && x >= max)
	    {
              pool_debug(mypool, SAT_ERROR, "read_id: id too large (%u/%u)\n", x, max);
	      data->error = SOLV_ERROR_ID_RANGE;
	      return 0;
	    }
	  return x;
	}
      x = (x << 7) ^ c ^ 128;
    }
  pool_debug(mypool, SAT_ERROR, "read_id: id too long\n");
  data->error = SOLV_ERROR_CORRUPT;
  return 0;
}


/*
 * read array of Ids
 */

#if 0
static Id *
read_rel_idarray(Repodata *data, Id max, Id *map, Id *store, Id *end, Id marker)
{
  unsigned int x = 0;
  int c;
  Id old = 0;

  if (data->error)
    return 0;
  for (;;)
    {
      c = getc(data->fp);
      if (c == EOF)
	{
	  pool_debug(mypool, SAT_ERROR, "unexpected EOF\n");
	  data->error = SOLV_ERROR_EOF;
	  return 0;
	}
      if ((c & 128) != 0)
	{
	  x = (x << 7) ^ c ^ 128;
	  continue;
	}
      x = (x << 6) | (c & 63);
      if (x == 0)
	{
	  /* marker hack */
	  if (store == end)
	    {
	      pool_debug(mypool, SAT_ERROR, "read_rel_idarray: array overflow\n");
	      data->error = SOLV_ERROR_OVERFLOW;
	      return 0;
	    }
	  if (c != 0x40)
	    {
	      *store++ = 0;
	      return store;
	    }
	  *store++ = marker;	/* do not map! */
	  old = 0;
	  x = 0;
	  continue;
	}
      x = (x - 1) + old;
      old = x;
      if (max && x >= max)
	{
	  pool_debug(mypool, SAT_ERROR, "read_rel_idarray: id too large (%u/%u)\n", x, max);
	  data->error = SOLV_ERROR_ID_RANGE;
	  return 0;
	}
      if (map)
	x = map[x];
      if (store == end)
	{
	  pool_debug(mypool, SAT_ERROR, "read_rel_idarray: array overflow\n");
	  return 0;
	}
      *store++ = x;
      if ((c & 64) == 0)
	{
	  if (x == 0)	/* already have trailing zero? */
	    return store;
	  if (store == end)
	    {
	      pool_debug(mypool, SAT_ERROR, "read_rel_idarray: array overflow\n");
	      data->error = SOLV_ERROR_OVERFLOW;
	      return 0;
	    }
	  *store++ = 0;
	  return store;
	}
      x = 0;
    }
}
#endif

static inline unsigned char *
data_read_id_max(unsigned char *dp, Id *ret, Id *map, int max, int *error)
{
  Id x;
  dp = data_read_id(dp, &x);
  if (max && x >= max)
    {
      pool_debug(mypool, SAT_ERROR, "data_read_idarray: id too large (%u/%u)\n", x, max);
      *error = SOLV_ERROR_ID_RANGE;
      x = 0;
    }
  *ret = map ? map[x] : x;
  return dp;
}

unsigned char *
data_read_idarray(unsigned char *dp, Id **storep, Id *map, int max, int *error)
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
      if (max && x >= max)
	{
	  pool_debug(mypool, SAT_ERROR, "data_read_idarray: id too large (%u/%u)\n", x, max);
	  *error = SOLV_ERROR_ID_RANGE;
	  break;
	}
      *store++ = x;
      if ((c & 64) == 0)
        break;
      x = 0;
    }
  *store++ = 0;
  *storep = store;
  return dp;
}

unsigned char *
data_read_rel_idarray(unsigned char *dp, Id **storep, Id *map, int max, int *error, Id marker)
{
  Id *store = *storep;
  Id old = 0;
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
      if (max && x >= max)
	{
	  pool_debug(mypool, SAT_ERROR, "data_read_rel_idarray: id too large (%u/%u)\n", x, max);
	  *error = SOLV_ERROR_ID_RANGE;
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
	  pool_debug(mypool, SAT_ERROR, "unexpected EOF\n");
	  data->error = SOLV_ERROR_EOF;
	  return 0;
	}
      if ((c & 128) != 0)
	{
	  x = (x << 7) ^ c ^ 128;
	  continue;
	}
      x = (x << 6) | (c & 63);
      if (max && x >= max)
	{
	  pool_debug(mypool, SAT_ERROR, "read_idarray: id too large (%u/%u)\n", x, max);
	  data->error = SOLV_ERROR_ID_RANGE;
	  return 0;
	}
      if (map)
	x = map[x];
      if (store == end)
	{
	  pool_debug(mypool, SAT_ERROR, "read_idarray: array overflow\n");
	  return 0;
	}
      *store++ = x;
      if ((c & 64) == 0)
	{
	  if (x == 0)	/* already have trailing zero? */
	    return store;
	  if (store == end)
	    {
	      pool_debug(mypool, SAT_ERROR, "read_idarray: array overflow\n");
	      data->error = SOLV_ERROR_OVERFLOW;
	      return 0;
	    }
	  *store++ = 0;
	  return store;
	}
      x = 0;
    }
}

static void
read_str(Repodata *data, char **inbuf, unsigned *len)
{
  unsigned char *buf = (unsigned char*)*inbuf;
  if (!buf)
    {
      buf = sat_malloc(1024);
      *len = 1024;
    }
  int c;
  unsigned ofs = 0;
  while((c = getc(data->fp)) != 0)
    {
      if (c == EOF)
        {
	  pool_debug (mypool, SAT_ERROR, "unexpected EOF\n");
	  data->error = SOLV_ERROR_EOF;
	  return;
        }
      /* Plus 1 as we also want to add the 0.  */
      if (ofs + 1 >= *len)
        {
	  *len += 256;
	  /* Don't realloc on the inbuf, it might be on the stack.  */
	  if (buf == (unsigned char*)*inbuf)
	    {
	      buf = sat_malloc(*len);
	      memcpy(buf, *inbuf, *len - 256);
	    }
	  else
	    buf = sat_realloc(buf, *len);
        }
      buf[ofs++] = c;
    }
  buf[ofs++] = 0;
  *inbuf = (char*)buf;
}

static void
skip_item(Repodata *data, unsigned type, unsigned numid, unsigned numrel)
{
  switch (type)
    {
      case REPOKEY_TYPE_VOID:
      case REPOKEY_TYPE_CONSTANT:
      case REPOKEY_TYPE_CONSTANTID:
	break;
      case REPOKEY_TYPE_ID:
	read_id(data, numid + numrel);		/* just check Id */
	break;
      case REPOKEY_TYPE_DIR:
	read_id(data, numid + data->dirpool.ndirs);	/* just check Id */
	break;
      case REPOKEY_TYPE_NUM:
	read_id(data, 0);
	break;
      case REPOKEY_TYPE_U32:
	read_u32(data);
	break;
      case REPOKEY_TYPE_STR:
	while (read_u8(data) != 0)
	  ;
	break;
      case REPOKEY_TYPE_IDARRAY:
      case REPOKEY_TYPE_REL_IDARRAY:
	while ((read_u8(data) & 0xc0) != 0)
	  ;
	break;
      case REPOKEY_TYPE_DIRNUMNUMARRAY:
	for (;;)
	  {
	    read_id(data, numid + data->dirpool.ndirs);	/* just check Id */
	    read_id(data, 0);
	    if (!(read_id(data, 0) & 0x40))
	      break;
	  }
	break;
      case REPOKEY_TYPE_DIRSTRARRAY:
	for (;;)
	  {
	    Id id = read_id(data, 0);
	    while (read_u8(data) != 0)
	      ;
	    if (!(id & 0x40))
	      break;
	  }
	break;
      default:
	pool_debug(mypool, SAT_ERROR, "unknown type %d\n", type);
        data->error = SOLV_ERROR_CORRUPT;
	break;
    }
}

static int
key_cmp (const void *pa, const void *pb)
{
  Repokey *a = (Repokey *)pa;
  Repokey *b = (Repokey *)pb;
  return a->name - b->name;
}

static void repodata_load_solv(Repodata *data);

static void
parse_repodata(Repodata *maindata, Id *keyp, Repokey *keys, Id *idmap, unsigned numid, unsigned numrel, Repo *repo)
{
  Id key, id;
  Id *ida, *ide;
  Repodata *data;
  int i, n;

  repo->repodata = sat_realloc2(repo->repodata, repo->nrepodata + 1, sizeof (*data));
  data = repo->repodata + repo->nrepodata++;
  memset(data, 0, sizeof(*data));
  data->repo = repo;
  data->pagefd = -1;
  data->state = REPODATA_STUB;
  data->loadcallback = repodata_load_solv;

  while ((key = *keyp++) != 0)
    {
      id = keys[key].name;
      switch (keys[key].type)
	{
	case REPOKEY_TYPE_IDARRAY:
	  if (id != REPODATA_KEYS)
	    {
	      skip_item(maindata, REPOKEY_TYPE_IDARRAY, numid, numrel);
	      break;
	    }
	  /* read_idarray writes a terminating 0, that's why the + 1 */
	  ida = sat_calloc(keys[key].size + 1, sizeof(Id));
	  ide = read_idarray(maindata, 0, 0, ida, ida + keys[key].size + 1);
	  n = ide - ida - 1;
	  if (n & 1)
	    {
	      pool_debug (mypool, SAT_ERROR, "invalid attribute data\n");
	      data->error = SOLV_ERROR_CORRUPT;
	      return;
	    }
	  data->nkeys = 1 + (n >> 1);
	  data->keys = sat_malloc2(data->nkeys, sizeof(data->keys[0]));
	  memset(data->keys, 0, sizeof(Repokey));
	  for (i = 1, ide = ida; i < data->nkeys; i++)
	    {
	      if (*ide >= numid)
		{
		  pool_debug (mypool, SAT_ERROR, "invalid attribute data\n");
		  data->error = SOLV_ERROR_CORRUPT;
		  return;
		}
	      data->keys[i].name = idmap ? idmap[*ide++] : *ide++;
	      data->keys[i].type = idmap ? idmap[*ide++] : *ide++;
	      data->keys[i].size = 0;
	      data->keys[i].storage = 0;
	    }
	  sat_free(ida);
	  if (data->nkeys > 2)
	    qsort(data->keys + 1, data->nkeys - 1, sizeof(data->keys[0]), key_cmp);
	  break;
	case REPOKEY_TYPE_STR:
	  if (id != REPODATA_LOCATION)
	    skip_item(maindata, REPOKEY_TYPE_STR, numid, numrel);
	  else
	    {
	      char buf[1024];
	      unsigned len = sizeof(buf);
	      char *filename = buf;
	      read_str(maindata, &filename, &len);
	      data->location = strdup(filename);
	      if (filename != buf)
		free(filename);
	    }
	  break;
	default:
	  skip_item(maindata, keys[key].type, numid, numrel);
	  break;
	}
    }
}

/*-----------------------------------------------------------------*/


static void
skip_schema(Repodata *data, Id *keyp, Repokey *keys, unsigned int numid, unsigned int numrel)
{
  Id key;
  while ((key = *keyp++) != 0)
    skip_item(data, keys[key].type, numid, numrel);
}

/*-----------------------------------------------------------------*/

static void
incore_add_id(Repodata *data, Id x)
{
  unsigned char *dp;
  /* make sure we have at least 5 bytes free */
  if (data->incoredatafree < 5)
    {
      data->incoredata = sat_realloc(data->incoredata, data->incoredatalen + 1024);
      data->incoredatafree = 1024;
    }
  dp = data->incoredata + data->incoredatalen;
  if (x < 0)
    abort();
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
incore_add_blob(Repodata *data, unsigned char *buf, int len)
{
  if (data->incoredatafree < len)
    {
      data->incoredata = sat_realloc(data->incoredata, data->incoredatalen + 1024 + len);
      data->incoredatafree = 1024 + len;
    }
  memcpy(data->incoredata + data->incoredatalen, buf, len);
  data->incoredatafree -= len;
  data->incoredatalen += len;
}

static void
incore_add_u32(Repodata *data, unsigned int x)
{
  unsigned char *dp;
  /* make sure we have at least 4 bytes free */
  if (data->incoredatafree < 4)
    {
      data->incoredata = sat_realloc(data->incoredata, data->incoredatalen + 1024);
      data->incoredatafree = 1024;
    }
  dp = data->incoredata + data->incoredatalen;
  *dp++ = x >> 24;
  *dp++ = x >> 16;
  *dp++ = x >> 8;
  *dp++ = x;
  data->incoredatafree -= 4;
  data->incoredatalen += 4;
}

#if 0
static void
incore_add_u8(Repodata *data, unsigned int x)
{
  unsigned char *dp;
  /* make sure we have at least 1 byte free */
  if (data->incoredatafree < 1)
    {
      data->incoredata = sat_realloc(data->incoredata, data->incoredatalen + 1024);
      data->incoredatafree = 1024;
    }
  dp = data->incoredata + data->incoredatalen;
  *dp++ = x;
  data->incoredatafree--;
  data->incoredatalen++;
}
#endif



// ----------------------------------------------


/*
 * read repo from .solv file
 *  and add it to pool
 */

static int
repo_add_solv_parent(Repo *repo, FILE *fp, Repodata *parent)
{
  Pool *pool = repo->pool;
  int i, l;
  unsigned int numid, numrel, numdir, numsolv;
  unsigned int numkeys, numschemata, numinfo;

  Offset sizeid;
  Offset *str;			       /* map Id -> Offset into string space */
  char *strsp;			       /* repo string space */
  char *sp;			       /* pointer into string space */
  Id *idmap;			       /* map of repo Ids to pool Ids */
  Id id, type;
  unsigned int hashmask, h;
  int hh;
  Id *hashtbl;
  Id name, evr, did;
  int flags;
  Reldep *ran;
  unsigned int size_idarray;
  Id *idarraydatap, *idarraydataend;
  Offset ido;
  Solvable *s;
  unsigned int solvflags;
  unsigned int solvversion;
  Repokey *keys;
  Id *schemadata, *schemadatap, *schemadataend;
  Id *schemata, key;
  int have_xdata;
  unsigned oldnrepodata;
  int maxsize, allsize;
  unsigned char *buf, *dp, *dps;
  int left;

  struct _Stringpool *spool;

  Repodata data;

  memset(&data, 0, sizeof(data));
  data.repo = repo;
  data.fp = fp;
  data.pagefd = -1;

  mypool = pool;

  if (read_u32(&data) != ('S' << 24 | 'O' << 16 | 'L' << 8 | 'V'))
    {
      pool_debug(pool, SAT_ERROR, "not a SOLV file\n");
      return SOLV_ERROR_NOT_SOLV;
    }
  solvversion = read_u32(&data);
  switch (solvversion)
    {
      case SOLV_VERSION_6:
        break;
      default:
        pool_debug(pool, SAT_ERROR, "unsupported SOLV version\n");
        return SOLV_ERROR_UNSUPPORTED;
    }

  pool_freeidhashes(pool);

  numid = read_u32(&data);
  numrel = read_u32(&data);
  numdir = read_u32(&data);
  numsolv = read_u32(&data);
  numkeys = read_u32(&data);
  numschemata = read_u32(&data);
  numinfo = read_u32(&data);
  solvflags = read_u32(&data);

  if (numdir && numdir < 2)
    {
      pool_debug(pool, SAT_ERROR, "bad number of dirs\n");
      return SOLV_ERROR_CORRUPT;
    }

  if (parent)
    {
      if (numrel)
	{
	  pool_debug(pool, SAT_ERROR, "relations are forbidden in a store\n");
	  return SOLV_ERROR_CORRUPT;
	}
      if (parent->end - parent->start != numsolv)
	{
	  pool_debug(pool, SAT_ERROR, "unequal number of solvables in a store\n");
	  return SOLV_ERROR_CORRUPT;
	}
      if (numinfo)
	{
	  pool_debug(pool, SAT_ERROR, "info blocks are forbidden in a store\n");
	  return SOLV_ERROR_CORRUPT;
	}
    }

  /*******  Part 1: string IDs  *****************************************/

  sizeid = read_u32(&data);	       /* size of string+Id space */

  /*
   * read strings and Ids
   * 
   */

  
  /*
   * alloc buffers
   */

  if (!parent)
    spool = &pool->ss;
  else
    {
      data.localpool = 1;
      spool = &data.spool;
      spool->stringspace = sat_malloc(7);
      strcpy(spool->stringspace, "<NULL>");
      spool->sstrings = 7;
      spool->nstrings = 0;
    }

  /* alloc string buffer */
  spool->stringspace = sat_realloc(spool->stringspace, spool->sstrings + sizeid + 1);
  /* alloc string offsets (Id -> Offset into string space) */
  spool->strings = sat_realloc2(spool->strings, spool->nstrings + numid, sizeof(Offset));

  strsp = spool->stringspace;
  str = spool->strings;		       /* array of offsets into strsp, indexed by Id */

  /* point to _BEHIND_ already allocated string/Id space */
  strsp += spool->sstrings;


  /*
   * read new repo at end of pool
   */
  
  if ((solvflags & SOLV_FLAG_PREFIX_POOL) == 0)
    {
      if (sizeid && fread(strsp, sizeid, 1, fp) != 1)
	{
	  pool_debug(pool, SAT_ERROR, "read error while reading strings\n");
	  return SOLV_ERROR_EOF;
	}
    }
  else
    {
      unsigned int pfsize = read_u32(&data);
      char *prefix = sat_malloc(pfsize);
      char *pp = prefix;
      char *old_str = 0;
      char *dest = strsp;
      int freesp = sizeid;

      if (pfsize && fread(prefix, pfsize, 1, fp) != 1)
        {
	  pool_debug(pool, SAT_ERROR, "read error while reading strings\n");
	  sat_free(prefix);
	  return SOLV_ERROR_EOF;
	}
      for (i = 1; i < numid; i++)
        {
	  int same = (unsigned char)*pp++;
	  size_t len = strlen(pp) + 1;
	  freesp -= same + len;
	  if (freesp < 0)
	    {
	      pool_debug(pool, SAT_ERROR, "overflow while expanding strings\n");
	      sat_free(prefix);
	      return SOLV_ERROR_OVERFLOW;
	    }
	  if (same)
	    memcpy(dest, old_str, same);
	  memcpy(dest + same, pp, len);
	  pp += len;
	  old_str = dest;
	  dest += same + len;
	}
      sat_free(prefix);
      if (freesp != 0)
	{
	  pool_debug(pool, SAT_ERROR, "expanding strings size mismatch\n");
	  return SOLV_ERROR_CORRUPT;
	}
    }
  strsp[sizeid] = 0;		       /* make string space \0 terminated */
  sp = strsp;

  if (parent)
    {
      /* no shared pool, thus no idmap and no unification */
      idmap = 0;
      spool->nstrings = numid;
      str[0] = 0;
      if (*sp)
	{
	  /* we need the '' for directories */
	  pool_debug(pool, SAT_ERROR, "store strings don't start with ''\n");
	  return SOLV_ERROR_CORRUPT;
	}
      for (i = 1; i < spool->nstrings; i++)
	{
	  if (sp >= strsp + sizeid)
	    {
	      pool_debug(pool, SAT_ERROR, "not enough strings\n");
	      return SOLV_ERROR_OVERFLOW;
	    }
	  str[i] = sp - spool->stringspace;
	  sp += strlen(sp) + 1;
	}
      spool->sstrings = sp - spool->stringspace;
    }
  else
    {

      /* alloc id map for name and rel Ids. this maps ids in the solv files
       * to the ids in our pool */
      idmap = sat_calloc(numid + numrel, sizeof(Id));

      /*
       * build hashes for all read strings
       * 
       */
      
      hashmask = mkmask(spool->nstrings + numid);

#if 0
      POOL_DEBUG(SAT_DEBUG_STATS, "read %d strings\n", numid);
      POOL_DEBUG(SAT_DEBUG_STATS, "string hash buckets: %d\n", hashmask + 1);
#endif

      /*
       * create hashtable with strings already in pool
       */

      hashtbl = sat_calloc(hashmask + 1, sizeof(Id));
      for (i = 1; i < spool->nstrings; i++)  /* leave out our dummy zero id */
	{
	  h = strhash(spool->stringspace + spool->strings[i]) & hashmask;
	  hh = HASHCHAIN_START;
	  while (hashtbl[h])
	    h = HASHCHAIN_NEXT(h, hh, hashmask);
	  hashtbl[h] = i;
	}

      /*
       * run over string space, calculate offsets
       * 
       * build id map (maps solv Id -> pool Id)
       */
      
      for (i = 1; i < numid; i++)
	{
	  if (sp >= strsp + sizeid)
	    {
	      sat_free(hashtbl);
	      sat_free(idmap);
	      pool_debug(pool, SAT_ERROR, "not enough strings %d %d\n", i, numid);
	      return SOLV_ERROR_OVERFLOW;
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
	      if (id == 0)
		break;
	      if (!strcmp(spool->stringspace + spool->strings[id], sp))
		break;		       /* existing string */
	      h = HASHCHAIN_NEXT(h, hh, hashmask);
	    }

	  /* length == offset to next string */
	  l = strlen(sp) + 1;
	  if (id == ID_NULL)	       /* end of hash chain -> new string */
	    {
	      id = spool->nstrings++;
	      hashtbl[h] = id;
	      str[id] = spool->sstrings;    /* save Offset */
	      if (sp != spool->stringspace + spool->sstrings)   /* not at end-of-buffer */
		memmove(spool->stringspace + spool->sstrings, sp, l);   /* append to pool buffer */
	      spool->sstrings += l;
	    }
	  idmap[i] = id;		       /* repo relative -> pool relative */
	  sp += l;			       /* next string */
	}
      sat_free(hashtbl);
    }
  pool_shrink_strings(pool);	       /* vacuum */

  
  /*******  Part 2: Relation IDs  ***************************************/

  /*
   * read RelDeps
   * 
   */
  
  if (numrel)
    {
      /* extend rels */
      pool->rels = sat_realloc2(pool->rels, pool->nrels + numrel, sizeof(Reldep));
      ran = pool->rels;

      hashmask = mkmask(pool->nrels + numrel);
#if 0
      POOL_DEBUG(SAT_DEBUG_STATS, "read %d rels\n", numrel);
      POOL_DEBUG(SAT_DEBUG_STATS, "rel hash buckets: %d\n", hashmask + 1);
#endif
      /*
       * prep hash table with already existing RelDeps
       */
      
      hashtbl = sat_calloc(hashmask + 1, sizeof(Id));
      for (i = 1; i < pool->nrels; i++)
	{
	  h = relhash(ran[i].name, ran[i].evr, ran[i].flags) & hashmask;
	  hh = HASHCHAIN_START;
	  while (hashtbl[h])
	    h = HASHCHAIN_NEXT(h, hh, hashmask);
	  hashtbl[h] = i;
	}

      /*
       * read RelDeps from repo
       */
      
      for (i = 0; i < numrel; i++)
	{
	  name = read_id(&data, i + numid);	/* read (repo relative) Ids */
	  evr = read_id(&data, i + numid);
	  flags = read_u8(&data);
	  name = idmap[name];		/* map to (pool relative) Ids */
	  evr = idmap[evr];
	  h = relhash(name, evr, flags) & hashmask;
	  hh = HASHCHAIN_START;
	  for (;;)
	    {
	      id = hashtbl[h];
	      if (id == ID_NULL)	/* end of hash chain */
		break;
	      if (ran[id].name == name && ran[id].evr == evr && ran[id].flags == flags)
		break;
	      h = HASHCHAIN_NEXT(h, hh, hashmask);
	    }
	  if (id == ID_NULL)		/* new RelDep */
	    {
	      id = pool->nrels++;
	      hashtbl[h] = id;
	      ran[id].name = name;
	      ran[id].evr = evr;
	      ran[id].flags = flags;
	    }
	  idmap[i + numid] = MAKERELDEP(id);   /* fill Id map */
	}
      sat_free(hashtbl);
      pool_shrink_rels(pool);		/* vacuum */
    }


  /*******  Part 3: Dirs  ***********************************************/
  if (numdir)
    {
      data.dirpool.dirs = sat_malloc2(numdir, sizeof(Id));
      data.dirpool.ndirs = numdir;
      data.dirpool.dirs[0] = 0;		/* dir 0: virtual root */
      data.dirpool.dirs[1] = 1;		/* dir 1: / */
      for (i = 2; i < numdir; i++)
	{
	  id = read_id(&data, i + numid);
	  if (id >= numid)
	    data.dirpool.dirs[i] = -(id - numid);
	  else if (idmap)
	    data.dirpool.dirs[i] = idmap[id];
	  else
	    data.dirpool.dirs[i] = id;
	}
    }

  /*******  Part 4: Keys  ***********************************************/

  keys = sat_calloc(numkeys, sizeof(*keys));
  /* keys start at 1 */
  for (i = 1; i < numkeys; i++)
    {
      id = read_id(&data, numid);
      if (idmap)
	id = idmap[id];
      else if (parent)
        id = str2id(pool, stringpool_id2str(spool, id), 1);
      type = read_id(&data, numid);
      if (idmap)
	type = idmap[type];
      else if (parent)
        type = str2id(pool, stringpool_id2str(spool, type), 1);
      if (type < REPOKEY_TYPE_VOID || type > REPOKEY_TYPE_DIRNUMNUMARRAY)
	{
	  pool_debug(pool, SAT_ERROR, "unsupported data type '%s'\n", id2str(pool, type));
	  data.error = SOLV_ERROR_UNSUPPORTED;
	  type = REPOKEY_TYPE_VOID;
	}
      keys[i].name = id;
      keys[i].type = type;
      keys[i].size = read_id(&data, keys[i].type == REPOKEY_TYPE_CONSTANTID ? numid + numrel : 0);
      keys[i].storage = read_id(&data, 0);
      if (id >= SOLVABLE_NAME && id <= RPM_RPMDBID)
	keys[i].storage = KEY_STORAGE_SOLVABLE;
      else if (keys[i].storage == KEY_STORAGE_SOLVABLE)
	keys[i].storage = KEY_STORAGE_INCORE;
      if (keys[i].type == REPOKEY_TYPE_CONSTANTID)
	{
	  if (idmap)
	    keys[i].size = idmap[keys[i].size];
	  else if (parent)
	    keys[i].size = str2id(pool, stringpool_id2str(spool, keys[i].size), 1);
	}
#if 0
      fprintf(stderr, "key %d %s %s %d %d\n", i, id2str(pool,id), id2str(pool, keys[i].type),
               keys[i].size, keys[i].storage);
#endif
    }

  have_xdata = parent ? 1 : 0;
  for (i = 1; i < numkeys; i++)
    if (keys[i].storage == KEY_STORAGE_INCORE || keys[i].storage == KEY_STORAGE_VERTICAL_OFFSET)
      have_xdata = 1;

  data.keys = keys;
  data.nkeys = numkeys;

  /*******  Part 5: Schemata ********************************************/
  
  id = read_id(&data, 0);
  schemadata = sat_calloc(id + 1, sizeof(Id));
  schemadatap = schemadata + 1;
  schemadataend = schemadatap + id;
  schemata = sat_calloc(numschemata, sizeof(Id));
  for (i = 1; i < numschemata; i++)
    {
      schemata[i] = schemadatap - schemadata;
      schemadatap = read_idarray(&data, numid, 0, schemadatap, schemadataend);
#if 0
      Id *sp = schemadata + schemata[i];
      fprintf (stderr, "schema %d:", i);
      for (; *sp; sp++)
        fprintf (stderr, " %d", *sp);
      fprintf (stderr, "\n");
#endif
    }
  data.schemata = schemata;
  data.nschemata = numschemata;
  data.schemadata = schemadata;
  data.schemadatalen = schemadataend - data.schemadata;


  /*******  Part 6: Info  ***********************************************/
  oldnrepodata = repo->nrepodata;
  if (numinfo)
    {
      id = read_id(&data, 0);
      id = read_id(&data, 0);
    }
  for (i = 0; i < numinfo; i++)
    {
      /* for now we're just interested in data that starts with
       * the repodata_external id
       */
      Id *keyp;
      id = read_id(&data, numschemata);
      keyp = schemadata + schemata[id];
      key = *keyp;
      if (keys[key].name == REPODATA_EXTERNAL && keys[key].type == REPOKEY_TYPE_VOID)
	{
	  /* external data for some ids */
	  parse_repodata(&data, keyp, keys, idmap, numid, numrel, repo);
	}
      else
	skip_schema(&data, keyp, keys, numid, numrel);
    }


  /*******  Part 7: item data *******************************************/

  /* calculate idarray size */
  size_idarray = 0;
  for (i = 1; i < numkeys; i++)
    {
      id = keys[i].name;
      if ((keys[i].type == REPOKEY_TYPE_IDARRAY || keys[i].type == REPOKEY_TYPE_REL_IDARRAY)
          && id >= INTERESTED_START && id <= INTERESTED_END)
	size_idarray += keys[i].size;
    }

  if (numsolv)
    {
      maxsize = read_id(&data, 0);
      allsize = read_id(&data, 0);
      if (maxsize > allsize)
	{
	  pool_debug(pool, SAT_ERROR, "maxsize %d is greater then allsize %d\n", maxsize, allsize);
	  data.error = SOLV_ERROR_CORRUPT;
	}
    }
  else
    maxsize = allsize = 0;

  /* allocate needed space in repo */
  /* we add maxsize because it is an upper limit for all idarrays */
  repo_reserve_ids(repo, 0, size_idarray + maxsize + 1);
  idarraydatap = repo->idarraydata + repo->idarraysize;
  repo->idarraysize += size_idarray;
  idarraydataend = idarraydatap + size_idarray;
  repo->lastoff = 0;

  /* read solvables */
  if (numsolv)
    {
      if (parent)
	s = pool_id2solvable(pool, parent->start);
      else
        s = pool_id2solvable(pool, repo_add_solvable_block(repo, numsolv));
      /* store start and end of our id block */
      data.start = s - pool->solvables;
      data.end = data.start + numsolv;
      /* In case we have info blocks, make them refer to our part of the 
	 repository now.  */
      for (i = oldnrepodata; i < repo->nrepodata; i++)
        {
	  repo->repodata[i].start = data.start;
	  repo->repodata[i].end = data.end;
	}
    }
  else
    s = 0;

  if (have_xdata)
    repodata_extend_block(&data, data.start, numsolv);

  left = 0;
  buf = sat_calloc(maxsize + 4, 1);
  dp = buf;
  for (i = 0; i < numsolv; i++, s++)
    {
      Id *keyp;
      if (data.error)
	break;

      left -= (dp - buf);
      if (left < 0)
        {
	  pool_debug(mypool, SAT_ERROR, "buffer overrun\n");
	  data.error = SOLV_ERROR_EOF;
	  break;
        }
      if (left)
        memmove(buf, dp, left);
      l = maxsize - left;
      if (l > allsize)
        l = allsize;
      if (l && fread(buf + left, l, 1, data.fp) != 1)
        {
	  pool_debug(mypool, SAT_ERROR, "unexpected EOF\n");
	  data.error = SOLV_ERROR_EOF;
	  break;
        }
      allsize -= l;
      left += l;
      dp = buf;

      dp = data_read_id_max(dp, &id, 0, numschemata, &data.error);
      if (have_xdata)
	{
	  data.incoreoffset[i] = data.incoredatalen;
	  incore_add_id(&data, id);
	}
      keyp = schemadata + schemata[id];
      while ((key = *keyp++) != 0)
	{
	  if (data.error)
	    break;

	  id = keys[key].name;
#if 0
fprintf(stderr, "solv %d name %d type %d class %d\n", i, id, keys[key].type, keys[key].storage);
#endif
	  if (keys[key].storage == KEY_STORAGE_VERTICAL_OFFSET)
	    {
	      /* copy offset/length into incore */
	      dps = dp;
	      dp = data_skip(dp, REPOKEY_TYPE_ID);
	      dp = data_skip(dp, REPOKEY_TYPE_ID);
	      incore_add_blob(&data, dps, dp - dps);
	      continue;
	    }
	  switch (keys[key].type)
	    {
	    case REPOKEY_TYPE_ID:
	      dp = data_read_id_max(dp, &did, idmap, numid + numrel, &data.error);
	      if (id == SOLVABLE_NAME)
		s->name = did;
	      else if (id == SOLVABLE_ARCH)
		s->arch = did;
	      else if (id == SOLVABLE_EVR)
		s->evr = did;
	      else if (id == SOLVABLE_VENDOR)
		s->vendor = did;
	      else if (keys[key].storage == KEY_STORAGE_INCORE)
	        incore_add_id(&data, did);
#if 0
	      POOL_DEBUG(SAT_DEBUG_STATS, "%s -> %s\n", id2str(pool, id), id2str(pool, did));
#endif
	      break;
	    case REPOKEY_TYPE_U32:
	      dp = data_read_u32(dp, &h);
#if 0
	      POOL_DEBUG(SAT_DEBUG_STATS, "%s -> %u\n", id2str(pool, id), h);
#endif
	      if (id == RPM_RPMDBID)
		{
		  if (!repo->rpmdbid)
		    repo->rpmdbid = sat_calloc(numsolv, sizeof(Id));
		  repo->rpmdbid[i] = h;
		}
	      else if (keys[key].storage == KEY_STORAGE_INCORE)
		incore_add_u32(&data, h);
	      break;
	    case REPOKEY_TYPE_IDARRAY:
	    case REPOKEY_TYPE_REL_IDARRAY:
	      if (id < INTERESTED_START || id > INTERESTED_END)
		{
		  dps = dp;
		  dp = data_skip(dp, REPOKEY_TYPE_IDARRAY);
		  if (keys[key].storage == KEY_STORAGE_INCORE && idmap)
		    abort();
		  if (keys[key].storage == KEY_STORAGE_INCORE)
		    incore_add_blob(&data, dps, dp - dps);
		  break;
		}
	      ido = idarraydatap - repo->idarraydata;
	      if (keys[key].type == REPOKEY_TYPE_IDARRAY)
	        dp = data_read_idarray(dp, &idarraydatap, idmap, numid + numrel, &data.error);
	      else if (id == SOLVABLE_REQUIRES)
	        dp = data_read_rel_idarray(dp, &idarraydatap, idmap, numid + numrel, &data.error, SOLVABLE_PREREQMARKER);
	      else if (id == SOLVABLE_PROVIDES)
	        dp = data_read_rel_idarray(dp, &idarraydatap, idmap, numid + numrel, &data.error, SOLVABLE_FILEMARKER);
	      else
	        dp = data_read_rel_idarray(dp, &idarraydatap, idmap, numid + numrel, &data.error, 0);
	      if (idarraydatap > idarraydataend)
		{
		  pool_debug(pool, SAT_ERROR, "idarray overflow\n");
		  data.error = SOLV_ERROR_OVERFLOW;
		  break;
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
	      else if (id == SOLVABLE_FRESHENS)
		s->freshens = ido;
#if 0
	      POOL_DEBUG(SAT_DEBUG_STATS, "%s ->\n", id2str(pool, id));
	      for (; repo->idarraydata[ido]; ido++)
	        POOL_DEBUG(SAT_DEBUG_STATS,"  %s\n", dep2str(pool, repo->idarraydata[ido]));
#endif
	      break;
	    default:
	      dps = dp;
	      dp = data_skip(dp, keys[key].type);
	      if (keys[key].storage == KEY_STORAGE_INCORE)
	        incore_add_blob(&data, dps, dp - dps);
	      break;
	    }
	}
    }

  /* should shrink idarraydata again */

  if (!data.error)
    {
      left -= (dp - buf);
      if (left < 0)
        {
	  pool_debug(mypool, SAT_ERROR, "buffer overrun\n");
	  data.error = SOLV_ERROR_EOF;
        }
    }
  sat_free(buf);

  if (data.error)
    {
      /* free solvables */
      repo_free_solvable_block(repo, data.start, data.end - data.start, 1);
      /* free id array */
      repo->idarraysize -= size_idarray;
      /* free incore data */
      data.incoredata = sat_free(data.incoredata);
      data.incoredatalen = data.incoredatafree = 0;
    }

  if (data.incoredatafree)
    {
      /* shrink excess size */
      data.incoredata = sat_realloc(data.incoredata, data.incoredatalen);
      data.incoredatafree = 0;
    }

  for (i = 1; i < numkeys; i++)
    if (keys[i].storage == KEY_STORAGE_VERTICAL_OFFSET)
      break;
  if (i < numkeys && !data.error)
    {
      Id fileoffset = 0;
      unsigned int pagesize;
      
      /* we have vertical data, make it available */
      data.verticaloffset = sat_calloc(numkeys, sizeof(Id));
      for (i = 1; i < numkeys; i++)
        if (keys[i].storage == KEY_STORAGE_VERTICAL_OFFSET)
	  {
	    data.verticaloffset[i] = fileoffset;
	    fileoffset += keys[i].size;
	  }
      data.lastverticaloffset = fileoffset;
      pagesize = read_u32(&data);
      repodata_read_or_setup_pages(&data, pagesize, fileoffset);
    }
  else
    {
      /* no longer needed */
      data.fp = 0;
    }

  if (parent && !data.error)
    {
      /* we're a store */
      sat_free(parent->schemata);
      sat_free(parent->schemadata);
      sat_free(parent->keys);
      sat_free(parent->location);
      *parent = data;
    }
  else if ((data.incoredatalen || data.fp) && !data.error)
    {
      /* we got some data, make it available */
      repo->repodata = sat_realloc2(repo->repodata, repo->nrepodata + 1, sizeof(data));
      repo->repodata[repo->nrepodata++] = data;
    }
  else
    {
      /* discard data */
      sat_free(data.dirpool.dirs);
      sat_free(data.incoreoffset);
      sat_free(schemata);
      sat_free(schemadata);
      sat_free(keys);
    }

  sat_free(idmap);
  mypool = 0;
  return data.error;
}

int
repo_add_solv(Repo *repo, FILE *fp)
{
  return repo_add_solv_parent(repo, fp, 0);
}

static void
repodata_load_solv(Repodata *data)
{
  FILE *fp;
  Pool *pool = data->repo->pool;
  if (!pool->loadcallback)
    {   
      data->state = REPODATA_ERROR;
      return;
    }   
  fp = pool->loadcallback(pool, data, pool->loadcallbackdata);
  if (!fp)
    {   
      data->state = REPODATA_ERROR;
      return;
    }   
  if (repo_add_solv_parent(data->repo, fp, data))
    data->state = REPODATA_ERROR;
  else
    data->state = REPODATA_AVAILABLE;
  fclose(fp);
}
