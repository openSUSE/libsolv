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
#include "attr_store_p.h"

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

static Id *
read_idarray(Repodata *data, Id max, Id *map, Id *store, Id *end, int relative)
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
      if ((c & 128) == 0)
	{
	  x = (x << 6) | (c & 63);
	  if (relative)
	    {
	      if (x == 0 && c == 0x40)
		{
		  /* prereq hack */
		  if (store == end)
		    {
		      pool_debug(mypool, SAT_ERROR, "read_idarray: array overflow\n");
		      data->error = SOLV_ERROR_OVERFLOW;
		      return 0;
		    }
		  *store++ = SOLVABLE_PREREQMARKER;
		  old = 0;
		  x = 0;
		  continue;
		}
	      x = (x - 1) + old;
	      old = x;
	    }
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
	  continue;
	}
      x = (x << 7) ^ c ^ 128;
    }
}

static void
read_str(Repodata *data, char **inbuf, unsigned *len)
{
  unsigned char *buf = (unsigned char*)*inbuf;
  if (!buf)
    {
      buf = xmalloc (1024);
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
	      buf = xmalloc(*len);
	      memcpy(buf, *inbuf, *len - 256);
	    }
	  else
	    buf = xrealloc(buf, *len);
        }
      buf[ofs++] = c;
    }
  buf[ofs++] = 0;
  *inbuf = (char*)buf;
}

static void
skip_item (Repodata *data, unsigned type, unsigned numid, unsigned numrel)
{
  switch (type)
    {
      case TYPE_VOID:
	break;
      case TYPE_ID:
	read_id(data, numid + numrel);   /* just check Id */
	break;
      case TYPE_U32:
	read_u32(data);
	break;
      case TYPE_ATTR_STRING:
      case TYPE_STR:
	while(read_u8(data) != 0)
	  ;
	break;
      case TYPE_IDARRAY:
      case TYPE_IDVALUEARRAY:
      case TYPE_IDVALUEVALUEARRAY:
      case TYPE_REL_IDARRAY:
      case TYPE_ATTR_INTLIST:
	while ((read_u8(data) & 0xc0) != 0)
	  ;
	break;
      case TYPE_COUNT_NAMED:
	{
	  unsigned count = read_id(data, 0);
	  while (count--)
	    {
	      read_id(data, numid);    /* Name */
	      unsigned t = read_id(data, TYPE_ATTR_TYPE_MAX + 1);
	      skip_item(data, t, numid, numrel);
	    }
	}
	break;
      case TYPE_COUNTED:
        {
	  unsigned count = read_id(data, 0);
	  unsigned t = read_id(data, TYPE_ATTR_TYPE_MAX + 1);
	  while (count--)
	    skip_item(data, t, numid, numrel);
	}
        break;
      case TYPE_ATTR_CHUNK:
	read_id(data, 0);
	/* Fallthrough.  */
      case TYPE_ATTR_INT:
	read_id(data, 0);
	break;
      case TYPE_ATTR_LOCALIDS:
	while (read_id(data, 0) != 0)
	  ;
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

static void
parse_repodata(Repodata *maindata, Id *keyp, Repokey *keys, Id *idmap, unsigned numid, unsigned numrel, Repo *repo)
{
  Id key, id;
  Id *ida, *ide;
  Repodata *data;
  int i, n;

  repo->repodata = xrealloc(repo->repodata, (repo->nrepodata + 1) * sizeof (*data));
  data = repo->repodata + repo->nrepodata++;
  data->repo = repo;
  memset(data, 0, sizeof(*data));

  while ((key = *keyp++) != 0)
    {
      id = keys[key].name;
      switch (keys[key].type)
	{
	case TYPE_IDVALUEARRAY:
	  if (id != REPODATA_KEYS)
	    {
	      skip_item(maindata, TYPE_IDVALUEARRAY, numid, numrel);
	      break;
	    }
	  ida = xcalloc(keys[key].size, sizeof(Id));
	  ide = read_idarray(maindata, 0, 0, ida, ida + keys[key].size, 0);
	  n = ide - ida - 1;
	  if (n & 1)
	    {
	      data->error = SOLV_ERROR_CORRUPT;
	      return;
	    }
	  data->nkeys = 1 + (n >> 1);
	  data->keys = xmalloc2(data->nkeys, sizeof(data->keys[0]));
	  memset(data->keys, 0, sizeof(Repokey));
	  for (i = 1, ide = ida; i < data->nkeys; i++)
	    {
	      if (*ide >= numid)
		{
		  data->error = SOLV_ERROR_CORRUPT;
		  pool_debug (mypool, SAT_ERROR, "invalid attribute data\n");
		  return;
		}
	      data->keys[i].name = idmap ? idmap[*ide++] : *ide++;
	      data->keys[i].type = *ide++;
	      data->keys[i].size = 0;
	      data->keys[i].storage = 0;
	    }
	  xfree(ida);
	  if (data->nkeys > 2)
	    qsort(data->keys + 1, data->nkeys - 1, sizeof(data->keys[0]), key_cmp);
	  break;
	case TYPE_STR:
	  if (id != REPODATA_LOCATION)
	    skip_item(maindata, TYPE_STR, numid, numrel);
	  else
	    {
	      char buf[1024];
	      unsigned len = sizeof (buf);
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
      data->incoredata = xrealloc(data->incoredata, data->incoredatalen + 1024);
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
incore_add_u32(Repodata *data, unsigned int x)
{
  unsigned char *dp;
  /* make sure we have at least 4 bytes free */
  if (data->incoredatafree < 4)
    {
      data->incoredata = xrealloc(data->incoredata, data->incoredatalen + 1024);
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

static void
incore_add_u8(Repodata *data, unsigned int x)
{
  unsigned char *dp;
  /* make sure we have at least 1 byte free */
  if (data->incoredatafree < 1)
    {
      data->incoredata = xrealloc(data->incoredata, data->incoredatalen + 1024);
      data->incoredatafree = 1024;
    }
  dp = data->incoredata + data->incoredatalen;
  *dp++ = x;
  data->incoredatafree--;
  data->incoredatalen++;
}

// ----------------------------------------------

/*
 * read repo from .solv file
 *  and add it to pool
 */

int
repo_add_solv(Repo *repo, FILE *fp)
{
  Pool *pool = repo->pool;
  int i, l;
  unsigned int numid, numrel, numsolv;
  unsigned int numkeys, numschemata, numinfo;
#if 0
  Attrstore *embedded_store = 0;
#endif

  Offset sizeid;
  Offset *str;			       /* map Id -> Offset into string space */
  char *strsp;			       /* repo string space */
  char *sp;			       /* pointer into string space */
  Id *idmap;			       /* map of repo Ids to pool Ids */
  Id id;
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

  Repodata data;

  memset(&data, 0, sizeof(data));
  data.repo = repo;
  data.fp = fp;

  mypool = pool;

  if (read_u32(&data) != ('S' << 24 | 'O' << 16 | 'L' << 8 | 'V'))
    {
      pool_debug(pool, SAT_ERROR, "not a SOLV file\n");
      return SOLV_ERROR_NOT_SOLV;
    }
  solvversion = read_u32(&data);
  switch (solvversion)
    {
      case SOLV_VERSION_1:
      case SOLV_VERSION_2:
      case SOLV_VERSION_3:
        break;
      default:
        pool_debug(pool, SAT_ERROR, "unsupported SOLV version\n");
        return SOLV_ERROR_UNSUPPORTED;
    }

  pool_freeidhashes(pool);

  numid = read_u32(&data);
  numrel = read_u32(&data);
  numsolv = read_u32(&data);
  numkeys = read_u32(&data);
  numschemata = read_u32(&data);
  numinfo = read_u32(&data);
  solvflags = read_u32(&data);

  if (solvversion < SOLV_VERSION_3
      && numinfo)
    {
      pool_debug(pool, SAT_ERROR, "unsupported SOLV format (has info)\n");
      return SOLV_ERROR_UNSUPPORTED;
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

  /* alloc string buffer */
  pool->ss.stringspace = xrealloc(pool->ss.stringspace, pool->ss.sstrings + sizeid + 1);
  /* alloc string offsets (Id -> Offset into string space) */
  pool->ss.strings = xrealloc2(pool->ss.strings, pool->ss.nstrings + numid, sizeof(Offset));

  strsp = pool->ss.stringspace;
  str = pool->ss.strings;		       /* array of offsets into strsp, indexed by Id */

  /* point to _BEHIND_ already allocated string/Id space */
  strsp += pool->ss.sstrings;


  /*
   * read new repo at end of pool
   */
  
  if ((solvflags & SOLV_FLAG_PREFIX_POOL) == 0)
    {
      if (fread(strsp, sizeid, 1, fp) != 1)
	{
	  pool_debug(pool, SAT_ERROR, "read error while reading strings\n");
	  return SOLV_ERROR_EOF;
	}
    }
  else
    {
      unsigned int pfsize = read_u32(&data);
      char *prefix = xmalloc(pfsize);
      char *pp = prefix;
      char *old_str = "";
      char *dest = strsp;
      if (fread(prefix, pfsize, 1, fp) != 1)
        {
	  pool_debug(pool, SAT_ERROR, "read error while reading strings\n");
	  xfree(prefix);
	  return SOLV_ERROR_EOF;
	}
      for (i = 1; i < numid; i++)
        {
	  int same = (unsigned char)*pp++;
	  size_t len = strlen (pp) + 1;
	  if (same)
	    memcpy(dest, old_str, same);
	  memcpy(dest + same, pp, len);
	  pp += len;
	  old_str = dest;
	  dest += same + len;
	}
      xfree(prefix);
    }
  strsp[sizeid] = 0;		       /* make string space \0 terminated */
  sp = strsp;


  /* alloc id map for name and rel Ids. this maps ids in the solv files
   * to the ids in our pool */
  idmap = (Id *)xcalloc(numid + numrel, sizeof(Id));

  /*
   * build hashes for all read strings
   * 
   */
  
  hashmask = mkmask(pool->ss.nstrings + numid);

#if 0
  POOL_DEBUG(SAT_DEBUG_STATS, "read %d strings\n", numid);
  POOL_DEBUG(SAT_DEBUG_STATS, "string hash buckets: %d\n", hashmask + 1);
#endif

  /*
   * create hashtable with strings already in pool
   */

  hashtbl = (Id *)xcalloc(hashmask + 1, sizeof(Id));
  for (i = 1; i < pool->ss.nstrings; i++)  /* leave out our dummy zero id */
    {
      h = strhash(pool->ss.stringspace + pool->ss.strings[i]) & hashmask;
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
	  xfree(hashtbl);
	  xfree(idmap);
	  pool_debug(pool, SAT_ERROR, "not enough strings\n");
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
	  if (!strcmp(pool->ss.stringspace + pool->ss.strings[id], sp))
	    break;		       /* existing string */
	  h = HASHCHAIN_NEXT(h, hh, hashmask);
	}

      /* length == offset to next string */
      l = strlen(sp) + 1;
      if (id == ID_NULL)	       /* end of hash chain -> new string */
	{
	  id = pool->ss.nstrings++;
	  hashtbl[h] = id;
	  str[id] = pool->ss.sstrings;    /* save Offset */
	  if (sp != pool->ss.stringspace + pool->ss.sstrings)   /* not at end-of-buffer */
	    memmove(pool->ss.stringspace + pool->ss.sstrings, sp, l);   /* append to pool buffer */
          pool->ss.sstrings += l;
	}
      idmap[i] = id;		       /* repo relative -> pool relative */
      sp += l;			       /* next string */
    }
  xfree(hashtbl);
  pool_shrink_strings(pool);	       /* vacuum */

  
  /*******  Part 2: Relation IDs  ***************************************/

  /*
   * read RelDeps
   * 
   */
  
  if (numrel)
    {
      /* extend rels */
      pool->rels = xrealloc2(pool->rels, pool->nrels + numrel, sizeof(Reldep));
      ran = pool->rels;

      hashmask = mkmask(pool->nrels + numrel);
#if 0
      POOL_DEBUG(SAT_DEBUG_STATS, "read %d rels\n", numrel);
      POOL_DEBUG(SAT_DEBUG_STATS, "rel hash buckets: %d\n", hashmask + 1);
#endif
      /*
       * prep hash table with already existing RelDeps
       */
      
      hashtbl = xcalloc(hashmask + 1, sizeof(Id));
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
      xfree(hashtbl);
      pool_shrink_rels(pool);		/* vacuum */
    }


  /*******  Part 3: Keys  ***********************************************/

  keys = xcalloc(numkeys, sizeof(*keys));
  /* keys start at 1 */
  for (i = 1; i < numkeys; i++)
    {
      id = read_id(&data, numid);
      if (idmap)
	id = idmap[id];
      keys[i].name = id;
      keys[i].type = read_id(&data, 0);
      keys[i].size = read_id(&data, 0);
      keys[i].storage = KEY_STORAGE_DROPPED;
      switch (keys[i].type)
	{
	case TYPE_ID:
	  switch(id)
	    {
	    case SOLVABLE_NAME:
	    case SOLVABLE_ARCH:
	    case SOLVABLE_EVR:
	    case SOLVABLE_VENDOR:
	      keys[i].storage = KEY_STORAGE_SOLVABLE;
	      break;
	    default:
	      keys[i].storage = KEY_STORAGE_INCORE;
	      break;
	    }
	  break;
	case TYPE_IDARRAY:
	case TYPE_REL_IDARRAY:
          if (id >= INTERESTED_START && id <= INTERESTED_END)
	    keys[i].storage = KEY_STORAGE_SOLVABLE;
	  else
	    keys[i].storage = KEY_STORAGE_INCORE;
	  break;
	case TYPE_STR:
	  keys[i].storage = KEY_STORAGE_INCORE;
	  break;
	case TYPE_U32:
	  if (id == RPM_RPMDBID)
	    keys[i].storage = KEY_STORAGE_SOLVABLE;
	  else
	    keys[i].storage = KEY_STORAGE_INCORE;
	  break;
	default:
	  break;
	}
    }

  data.keys = keys;
  data.nkeys = numkeys;

  /*******  Part 4: Schemata ********************************************/
  
  id = read_id(&data, 0);
  schemadata = xcalloc(id, sizeof(Id));
  schemadatap = schemadata;
  schemadataend = schemadata + id;
  schemata = xcalloc(numschemata, sizeof(Id));
  for (i = 0; i < numschemata; i++)
    {
      schemata[i] = schemadatap - schemadata;
      schemadatap = read_idarray(&data, numid, 0, schemadatap, schemadataend, 0);
    }
  data.schemata = schemata;
  data.nschemata = numschemata;
  data.schemadata = schemadata;

  /*******  Part 5: Info  ***********************************************/
  for (i = 0; i < numinfo; i++)
    {
      /* for now we're just interested in data that starts with
       * the repodata_external id
       */
      Id *keyp = schemadata + schemata[read_id(&data, numschemata)];
      key = *keyp;
      if (keys[key].name == REPODATA_EXTERNAL && keys[key].type == TYPE_VOID)
	{
	  /* external data for some ids */
	  parse_repodata(&data, keyp, keys, idmap, numid, numrel, repo);
	}
      else
	skip_schema(&data, keyp, keys, numid, numrel);
    }

  /*******  Part 6: packed sizes (optional)  ****************************/
  char *exists = 0;
  if ((solvflags & SOLV_FLAG_PACKEDSIZES) != 0)
    {
      exists = xmalloc (numsolv);
      for (i = 0; i < numsolv; i++)
	exists[i] = read_id(&data, 0) != 0;
    }

  /*******  Part 7: item data *******************************************/

  /* calculate idarray size */
  size_idarray = 0;
  for (i = 1; i < numkeys; i++)
    {
      id = keys[i].name;
      if ((keys[i].type == TYPE_IDARRAY || keys[i].type == TYPE_REL_IDARRAY)
          && id >= INTERESTED_START && id <= INTERESTED_END)
	size_idarray += keys[i].size;
    }

  /* allocate needed space in repo */
  if (size_idarray)
    {
      repo_reserve_ids(repo, 0, size_idarray);
      idarraydatap = repo->idarraydata + repo->idarraysize;
      repo->idarraysize += size_idarray;
      idarraydataend = idarraydatap + size_idarray;
      repo->lastoff = 0;
    }
  else
    {
      idarraydatap = 0;
      idarraydataend = 0;
    }

  /* read solvables */
  s = pool_id2solvable(pool, repo_add_solvable_block(repo, numsolv));

  /* store start and end of our id block */
  data.start = s - pool->solvables;
  data.end = data.start + numsolv;

  for (i = 0; i < numsolv; i++, s++)
    {
      if (data.error)
	break;
      if (exists && !exists[i])
        continue;
      Id *keyp = schemadata + schemata[read_id(&data, numschemata)];
      while ((key = *keyp++) != 0)
	{
	  id = keys[key].name;
	  switch (keys[key].type)
	    {
	    case TYPE_ID:
	      did = read_id(&data, numid + numrel);
	      if (idmap)
		did = idmap[did];
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
	    case TYPE_U32:
	      h = read_u32(&data);
#if 0
	      POOL_DEBUG(SAT_DEBUG_STATS, "%s -> %u\n", id2str(pool, id), h);
#endif
	      if (id == RPM_RPMDBID)
		{
		  if (!repo->rpmdbid)
		    repo->rpmdbid = (Id *)xcalloc(numsolv, sizeof(Id));
		  repo->rpmdbid[i] = h;
		}
	      else if (keys[key].storage == KEY_STORAGE_INCORE)
		incore_add_u32(&data, h);
	      break;
	    case TYPE_STR:
              if (keys[key].storage == KEY_STORAGE_INCORE)
		{
		  while ((h = read_u8(&data)) != 0)
		    incore_add_u8(&data, h);
		  incore_add_u8(&data, 0);
		}
	      else
		{
		  while (read_u8(&data) != 0)
		    ;
		}
	      break;
	    case TYPE_IDARRAY:
	    case TYPE_REL_IDARRAY:
	      if (id < INTERESTED_START || id > INTERESTED_END)
		{
		  if (keys[key].storage == KEY_STORAGE_INCORE)
		    {
		      if (idmap)
			{
			  Id old = 0, rel = keys[key].type == TYPE_REL_IDARRAY ? SOLVABLE_PREREQMARKER : 0;
			  do
			    {
			      did = read_id(&data, 0);
			      h = did & 0x40;
			      did = (did & 0x3f) | ((did >> 1) & ~0x3f);
			      if (rel)
				{
				  if (did == 0)
				    {
				      did = rel;
				      old = 0;
				    }
				  else
				    {
				      did += old;
				      old = did;
				    }
				}
			      if (did >= numid + numrel)
				abort();
			      did = idmap[did];
			      did = ((did & ~0x3f) << 1) | h;
			      incore_add_id(&data, did);
			    }
			  while (h);
			}
		      else
			{
			  while (((h = read_u8(&data)) & 0xc0) != 0)
			    incore_add_u8(&data, h);
			  break;
			}
		    }
		  else
		    {
		      while ((read_u8(&data) & 0xc0) != 0)
			;
		      break;
		    }
		  break;
		}
	      ido = idarraydatap - repo->idarraydata;
	      idarraydatap = read_idarray(&data, numid + numrel, idmap, idarraydatap, idarraydataend, keys[key].type == TYPE_REL_IDARRAY);
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
#if 0
	    case TYPE_VOID:

	    case TYPE_ATTR_INT:
	    case TYPE_ATTR_CHUNK:
	    case TYPE_ATTR_STRING:
	    case TYPE_ATTR_INTLIST:
	    case TYPE_ATTR_LOCALIDS:
	      if (!embedded_store)
		embedded_store = new_store (pool);
	      add_attr_from_file (embedded_store, i, id, keys[key].type, idmap, numid, &data);
	      break;
#endif
	    default:
	      skip_item(&data, keys[key].type, numid, numrel);
	    }
	}
    }

  if (data.error)
    {
      /* free solvables */
      repo_free_solvable_block(repo, data.start, data.end - data.start, 1);
      /* free id array */
      repo->idarraysize -= size_idarray;
      /* free incore data */
      data.incoredata = xfree(data.incoredata);
      data.incoredatalen = data.incoredatafree = 0;
    }

  if (data.incoredatafree)
    {
      /* shrink excess size */
      data.incoredata = xrealloc(data.incoredata, data.incoredatalen);
      data.incoredatafree = 0;
    }

  for (i = 1; i < numkeys; i++)
    if (keys[i].storage == KEY_STORAGE_VERTICAL_OFFSET)
      break;
  if (i < numkeys && !data.error)
    {
      /* we have vertical data, make it available */
      data.verticaloffset = ftell(fp);
    }
  else
    {
      /* no longer needed */
      data.fp = 0;
    }

  if (data.incoredatalen || data.fp)
    {
      /* we got some data, make it available */
      repo->repodata = xrealloc(repo->repodata, (repo->nrepodata + 1) * sizeof(data));
      repo->repodata[repo->nrepodata++] = data;
    }
  else
    {
      xfree(schemata);
      xfree(schemadata);
      xfree(keys);
    }

  xfree(exists);
#if 0
  if (embedded_store)
    {
      attr_store_pack (embedded_store);
      /* If we have any attributes we also have pages.  */
      read_or_setup_pages (fp, embedded_store);
      /* The NULL name here means embedded attributes.  */
      repo_add_attrstore (repo, embedded_store, NULL);
    }
#endif
  xfree(idmap);
  mypool = 0;
  return data.error;
}

// EOF
