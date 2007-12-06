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

#define INTERESTED_START	SOLVABLE_NAME
#define INTERESTED_END		SOLVABLE_FRESHENS

static Pool *mypool;		/* for pool_debug... */

/*-----------------------------------------------------------------*/
/* .solv read functions */

/*
 * read u32
 */

static unsigned int
read_u32(FILE *fp)
{
  int c, i;
  unsigned int x = 0;

  for (i = 0; i < 4; i++)
    {
      c = getc(fp);
      if (c == EOF)
	{
	  pool_debug(mypool, SAT_FATAL, "unexpected EOF\n");
	  exit(1);
	}
      x = (x << 8) | c;
    }
  return x;
}


/*
 * read u8
 */

static unsigned int
read_u8(FILE *fp)
{
  int c;
  c = getc(fp);
  if (c == EOF)
    {
      pool_debug(mypool, SAT_FATAL, "unexpected EOF\n");
      exit(1);
    }
  return c;
}


/*
 * read Id
 */

static Id
read_id(FILE *fp, Id max)
{
  unsigned int x = 0;
  int c, i;

  for (i = 0; i < 5; i++)
    {
      c = getc(fp);
      if (c == EOF)
	{
          pool_debug(mypool, SAT_FATAL, "unexpected EOF\n");
	  exit(1);
	}
      if (!(c & 128))
	{
	  x = (x << 7) | c;
	  if (max && x >= max)
	    {
              pool_debug(mypool, SAT_FATAL, "read_id: id too large (%u/%u)\n", x, max);
	      exit(1);
	    }
	  return x;
	}
      x = (x << 7) ^ c ^ 128;
    }
  pool_debug(mypool, SAT_FATAL, "read_id: id too long\n");
  exit(1);
}


/*
 * read array of Ids
 */

static Id *
read_idarray(FILE *fp, Id max, Id *map, Id *store, Id *end, int relative)
{
  unsigned int x = 0;
  int c;
  Id old = 0;
  for (;;)
    {
      c = getc(fp);
      if (c == EOF)
	{
	  pool_debug(mypool, SAT_FATAL, "unexpected EOF\n");
	  exit(1);
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
		      pool_debug(mypool, SAT_FATAL, "read_idarray: array overflow\n");
		      exit(1);
		    }
		  *store++ = SOLVABLE_PREREQMARKER;
		  old = 0;
		  x = 0;
		  continue;
		}
	      x = (x - 1) + old;
	      old = x;
	    }
	  if (x >= max)
	    {
	      pool_debug(mypool, SAT_FATAL, "read_idarray: id too large (%u/%u)\n", x, max);
	      exit(1);
	    }
	  if (map)
	    x = map[x];
	  if (store == end)
	    {
	      pool_debug(mypool, SAT_FATAL, "read_idarray: array overflow\n");
	      exit(1);
	    }
	  *store++ = x;
	  if ((c & 64) == 0)
	    {
	      if (x == 0)	/* already have trailing zero? */
		return store;
	      if (store == end)
		{
		  pool_debug(mypool, SAT_FATAL, "read_idarray: array overflow\n");
		  exit(1);
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


/*-----------------------------------------------------------------*/

struct key {
  Id name;
  Id type;
  Id size;
};


// ----------------------------------------------

/*
 * read repo from .solv file
 *  and add it to pool
 */

void
repo_add_solv(Repo *repo, FILE *fp)
{
  Pool *pool = repo->pool;
  int i, l;
  unsigned int numid, numrel, numsolv;
  unsigned int numkeys, numschemata, numinfo;

  int type;
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
  struct key *keys;
  Id *schemadata, *schemadatap, *schemadataend;
  Id *schemata, key;

  mypool = pool;

  if (read_u32(fp) != ('S' << 24 | 'O' << 16 | 'L' << 8 | 'V'))
    {
      pool_debug(pool, SAT_FATAL, "not a SOLV file\n");
      exit(1);
    }
  solvversion = read_u32(fp);
  switch (solvversion)
    {
      case SOLV_VERSION_1:
      case SOLV_VERSION_2:
        break;
      default:
        pool_debug(pool, SAT_FATAL, "unsupported SOLV version\n");
        exit(1);
    }

  pool_freeidhashes(pool);

  numid = read_u32(fp);
  numrel = read_u32(fp);
  numsolv = read_u32(fp);
  numkeys = read_u32(fp);
  numschemata = read_u32(fp);
  numinfo = read_u32(fp);
  solvflags = read_u32(fp);

  /*******  Part 1: string IDs  *****************************************/

  sizeid = read_u32(fp);	       /* size of string+Id space */

  /*
   * read strings and Ids
   * 
   */

  
  /*
   * alloc buffers
   */

  /* alloc string buffer */
  strsp = (char *)xrealloc(pool->ss.stringspace, pool->ss.sstrings + sizeid + 1);
  /* alloc string offsets (Id -> Offset into string space) */
  str = (Offset *)xrealloc(pool->ss.strings, (pool->ss.nstrings + numid) * sizeof(Offset));

  pool->ss.stringspace = strsp;
  pool->ss.strings = str;		       /* array of offsets into strsp, indexed by Id */

  /* point to _BEHIND_ already allocated string/Id space */
  strsp += pool->ss.sstrings;

  /* alloc id map for name and rel Ids. this maps ids in the solv files
   * to the ids in our pool */
  idmap = (Id *)xcalloc(numid + numrel, sizeof(Id));

  /*
   * read new repo at end of pool
   */
  
  if ((solvflags & SOLV_FLAG_PREFIX_POOL) == 0)
    {
      if (fread(strsp, sizeid, 1, fp) != 1)
	{
	  pool_debug(pool, SAT_FATAL, "read error while reading strings\n");
	  exit(1);
	}
    }
  else
    {
      unsigned int pfsize = read_u32 (fp);
      char *prefix = xmalloc (pfsize);
      char *pp = prefix;
      char *old_str = "";
      char *dest = strsp;
      if (fread (prefix, pfsize, 1, fp) != 1)
        {
	  pool_debug(pool, SAT_FATAL, "read error while reading strings\n");
	  exit(1);
	}
      for (i = 1; i < numid; i++)
        {
	  int same = (unsigned char)*pp++;
	  size_t len = strlen (pp) + 1;
	  if (same)
	    memcpy (dest, old_str, same);
	  memcpy (dest + same, pp, len);
	  pp += len;
	  old_str = dest;
	  dest += same + len;
	}
      xfree (prefix);
    }
  strsp[sizeid] = 0;		       /* make string space \0 terminated */
  sp = strsp;

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
   * ensure sufficient hash size
   */
  
  hashtbl = (Id *)xcalloc(hashmask + 1, sizeof(Id));

  /*
   * fill hashtable with strings already in pool
   */
  
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
	  pool_debug(pool, SAT_FATAL, "not enough strings\n");
	  exit(1);
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
      ran = (Reldep *)xrealloc(pool->rels, (pool->nrels + numrel) * sizeof(Reldep));
      if (!ran)
	{
	  pool_debug(pool, SAT_FATAL, "no mem for rel space\n");
	  exit(1);
	}
      pool->rels = ran;	       /* extended rel space */

      hashmask = mkmask(pool->nrels + numrel);
#if 0
      POOL_DEBUG(SAT_DEBUG_STATS, "read %d rels\n", numrel);
      POOL_DEBUG(SAT_DEBUG_STATS, "rel hash buckets: %d\n", hashmask + 1);
#endif
      /*
       * prep hash table with already existing RelDeps
       */
      
      hashtbl = (Id *)xcalloc(hashmask + 1, sizeof(Id));
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
	  name = read_id(fp, i + numid);	/* read (repo relative) Ids */
	  evr = read_id(fp, i + numid);
	  flags = read_u8(fp);
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
      keys[i].name = idmap[read_id(fp, numid)];
      keys[i].type = read_id(fp, 0);
      keys[i].size = read_id(fp, 0);
    }

  /*******  Part 4: Schemata ********************************************/
  
  id = read_id(fp, 0);
  schemadata = xcalloc(id, sizeof(Id));
  schemadatap = schemadata;
  schemadataend = schemadata + id;
  schemata = xcalloc(numschemata, sizeof(Id));
  for (i = 0; i < numschemata; i++)
    {
      schemata[i] = schemadatap - schemadata;
      schemadatap = read_idarray(fp, numid, 0, schemadatap, schemadataend, 0);
    }

  /*******  Part 5: Info  ***********************************************/
  /* we skip the info for now... */
  for (i = 0; i < numinfo; i++)
    {
      Id *schema = schemadata + schemata[read_id(fp, numschemata)];
      while ((key = *schema++) != 0)
	{
	  type = keys[key].type;
	  switch (type)
	    {
	      case TYPE_ID:
	        read_id(fp, numid + numrel);   /* just check Id */
	        break;
	      case TYPE_U32:
	        read_u32(fp);
	        break;
	      case TYPE_STR:
	        while(read_u8(fp) != 0)
		  ;
	        break;
	      case TYPE_IDARRAY:
	      case TYPE_REL_IDARRAY:
		while ((read_u8(fp) & 0xc0) != 0)
		  ;
		break;
	      default:
	        pool_debug(pool, SAT_FATAL, "unknown type %d\n", type);
	        exit(0);
	    }
	}
    }

  /*******  Part 6: packed sizes (optional)  ****************************/
  if ((solvflags & SOLV_FLAG_PACKEDSIZES) != 0)
    {
      for (i = 0; i < numsolv; i++)
	read_id(fp, 0);
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
      idarraydataend = repo->idarraydata + repo->idarraysize;
      repo->lastoff = 0;
    }
  else
    {
      idarraydatap = 0;
      idarraydataend = 0;
    }

  /* read solvables */
  s = pool_id2solvable(pool, repo_add_solvable_block(repo, numsolv));

  if ((solvflags & SOLV_FLAG_VERTICAL) != 0)
    {
      Id *solvschema = xcalloc(numsolv, sizeof(Id));
      unsigned char *used = xmalloc(numschemata);
      Solvable *sstart = s;
      Id type;

      for (i = 0; i < numsolv; i++)
	solvschema[i] = read_id(fp, numschemata);
      for (key = 1; key < numkeys; key++)
	{
	  id = keys[key].name;
	  type = keys[key].type;
	  memset(used, 0, numschemata);
	  for (i = 0; i < numschemata; i++)
	    {
	      Id *keyp = schemadata + schemata[i];
	      while (*keyp)
		if (*keyp++ == key)
		  {
		    used[i] = 1;
		    break;
		  }
	    }
	  for (i = 0, s = sstart; i < numsolv; i++, s++)
	     {
	      if (!used[solvschema[i]])
		continue;
	      switch (type)
		{
		case TYPE_ID:
		  did = idmap[read_id(fp, numid + numrel)];
		  if (id == SOLVABLE_NAME)
		    s->name = did;
		  else if (id == SOLVABLE_ARCH)
		    s->arch = did;
		  else if (id == SOLVABLE_EVR)
		    s->evr = did;
		  else if (id == SOLVABLE_VENDOR)
		    s->vendor = did;
		  break;
		case TYPE_U32:
		  h = read_u32(fp);
		  if (id == RPM_RPMDBID)
		    {
		      if (!repo->rpmdbid)
			repo->rpmdbid = (Id *)xcalloc(numsolv, sizeof(Id));
		      repo->rpmdbid[i] = h;
		    }
		  break;
		case TYPE_STR:
		  while(read_u8(fp) != 0)
		    ;
		  break;
		case TYPE_IDARRAY:
		case TYPE_REL_IDARRAY:
		  if (id < INTERESTED_START || id > INTERESTED_END)
		    {
		      /* not interested in array */
		      while ((read_u8(fp) & 0xc0) != 0)
			;
		      break;
		    }
		  ido = idarraydatap - repo->idarraydata;
		  idarraydatap = read_idarray(fp, numid + numrel, idmap, idarraydatap, idarraydataend, type == TYPE_REL_IDARRAY);
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
		  break;
		}
	    }
	}
      xfree(used);
      xfree(solvschema);
      xfree(idmap);
      xfree(schemata);
      xfree(schemadata);
      xfree(keys);
      mypool = 0;
      return;
    }
  for (i = 0; i < numsolv; i++, s++)
    {
      Id *keyp = schemadata + schemata[read_id(fp, numschemata)];
      while ((key = *keyp++) != 0)
	{
	  id = keys[key].name;
	  switch (keys[key].type)
	    {
	    case TYPE_ID:
	      did = idmap[read_id(fp, numid + numrel)];
	      if (id == SOLVABLE_NAME)
		s->name = did;
	      else if (id == SOLVABLE_ARCH)
		s->arch = did;
	      else if (id == SOLVABLE_EVR)
		s->evr = did;
	      else if (id == SOLVABLE_VENDOR)
		s->vendor = did;
#if 0
	      POOL_DEBUG(SAT_DEBUG_STATS, "%s -> %s\n", id2str(pool, id), id2str(pool, did));
#endif
	      break;
	    case TYPE_U32:
	      h = read_u32(fp);
#if 0
	      POOL_DEBUG(SAT_DEBUG_STATS, "%s -> %u\n", id2str(pool, id), h);
#endif
	      if (id == RPM_RPMDBID)
		{
		  if (!repo->rpmdbid)
		    repo->rpmdbid = (Id *)xcalloc(numsolv, sizeof(Id));
		  repo->rpmdbid[i] = h;
		}
	      break;
	    case TYPE_STR:
	      while(read_u8(fp) != 0)
		;
	      break;
	    case TYPE_IDARRAY:
	    case TYPE_REL_IDARRAY:
	      if (id < INTERESTED_START || id > INTERESTED_END)
		{
		  /* not interested in array */
		  while ((read_u8(fp) & 0xc0) != 0)
		    ;
		  break;
		}
	      ido = idarraydatap - repo->idarraydata;
	      idarraydatap = read_idarray(fp, numid + numrel, idmap, idarraydatap, idarraydataend, keys[key].type == TYPE_REL_IDARRAY);
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
	    }
	}
    }
  xfree(idmap);
  xfree(schemata);
  xfree(schemadata);
  xfree(keys);
  mypool = 0;
}

// EOF
