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
#include "sat_debug.h"

#define INTERESTED_START	SOLVABLE_NAME
#define INTERESTED_END		SOLVABLE_FRESHENS

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
	  sat_debug (ERROR, "unexpected EOF\n");
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
      sat_debug (ERROR, "unexpected EOF\n");
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
	  sat_debug (ERROR, "unexpected EOF\n");
	  exit(1);
	}
      if (!(c & 128))
	{
	  x = (x << 7) | c;
	  if (x >= max)
	    {
	      sat_debug (ERROR, "read_id: id too large (%u/%u)\n", x, max);
	      exit(1);
	    }
	  return x;
	}
      x = (x << 7) ^ c ^ 128;
    }
  sat_debug (ERROR, "read_id: id too long\n");
  exit(1);
}


/*
 * read array of Ids
 */

static Id *
read_idarray(FILE *fp, Id max, Id *map, Id *store, Id *end)
{
  unsigned int x = 0;
  int c;
  for (;;)
    {
      c = getc(fp);
      if (c == EOF)
	{
	  sat_debug (ERROR, "unexpected EOF\n");
	  exit(1);
	}
      if ((c & 128) == 0)
	{
	  x = (x << 6) | (c & 63);
          if (x >= max)
	    {
	      sat_debug (ERROR, "read_idarray: id too large (%u/%u)\n", x, max);
	      exit(1);
	    }
	  if (store == end)
	    {
	      sat_debug (ERROR, "read_idarray: array overflow\n");
	      exit(1);
	    }
	  *store++ = map[x];
	  if ((c & 64) == 0)
	    {
	      if (store == end)
		{
		  sat_debug (ERROR, "read_idarray: array overflow\n");
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

typedef struct solvdata {
  int type;
  Id id;
  unsigned int size;
} SolvData;


// ----------------------------------------------

/*
 * read repo from .solv file
 *  and add it to pool
 */

void
repo_add_solv(Repo *repo, FILE *fp)
{
  Pool *pool = repo->pool;
  int i, j, l;
  unsigned int numid, numrel, numsolv, numsrcdata, numsolvdata;
  int numsolvdatabits, type;
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
  SolvData *solvdata;
  unsigned int size, size_str, size_idarray;
  Id *idarraydatap, *idarraydataend;
  Offset ido;
  unsigned int databits;
  Solvable *s;

  if (read_u32(fp) != ('S' << 24 | 'O' << 16 | 'L' << 8 | 'V'))
    {
      sat_debug (ERROR, "not a SOLV file\n");
      exit(1);
    }
  if (read_u32(fp) != SOLV_VERSION)
    {
      sat_debug (ERROR, "unsupported SOLV version\n");
      exit(1);
    }

  pool_freeidhashes(pool);

  numid = read_u32(fp);
  numrel = read_u32(fp);
  numsolv= read_u32(fp);

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
  
  if (fread(strsp, sizeid, 1, fp) != 1)
    {
      sat_debug (ERROR, "read error while reading strings\n");
      exit(1);
    }
  strsp[sizeid] = 0;		       /* make string space \0 terminated */
  sp = strsp;

  /*
   * build hashes for all read strings
   * 
   */
  
  hashmask = mkmask(pool->ss.nstrings + numid);

#if 0
  sat_debug (ALWAYS, "read %d strings\n", numid);
  sat_debug (ALWAYS, "string hash buckets: %d\n", hashmask + 1);
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
	  sat_debug (ERROR, "not enough strings\n");
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
	  sat_debug (ERROR, "no mem for rel space\n");
	  exit(1);
	}
      pool->rels = ran;	       /* extended rel space */

      hashmask = mkmask(pool->nrels + numrel);
#if 0
      sat_debug (ALWAYS, "read %d rels\n", numrel);
      sat_debug (ALWAYS, "rel hash buckets: %d\n", hashmask + 1);
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

  /*
   * read (but dont store yet) repo data
   */

#if 0
  sat_debug (ALWAYS, "read repo data\n");
#endif
  numsrcdata = read_u32(fp);
  for (i = 0; i < numsrcdata; i++)
    {
      type = read_u8(fp);
      id = idmap[read_id(fp, numid)];
      switch(type)
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
	default:
          sat_debug (ERROR, "unknown type\n");
	  exit(0);
	}
    }


  /*
   * read solvables
   */
  
#if 0
  sat_debug (ALWAYS, "read solvable data info\n");
#endif
  numsolvdata = read_u32(fp);
  numsolvdatabits = 0;
  solvdata = (SolvData *)xmalloc(numsolvdata * sizeof(SolvData));
  size_idarray = 0;
  size_str = 0;

  for (i = 0; i < numsolvdata; i++)
    {
      type = read_u8(fp);
      solvdata[i].type = type;
      if ((type & TYPE_BITMAP) != 0)
	{
	  type ^= TYPE_BITMAP;
	  numsolvdatabits++;
	}
      id = idmap[read_id(fp, numid)];
#if 0
      sat_debug (ALWAYS, "#%d: %s\n", i, id2str(pool, id));
#endif
      solvdata[i].id = id;
      size = read_u32(fp);
      solvdata[i].size = size;
      if (id >= INTERESTED_START && id <= INTERESTED_END)
	{
	  if (type == TYPE_STR)
	    size_str += size;
	  if (type == TYPE_IDARRAY)
	    size_idarray += size;
	}
    }

  if (numsolvdatabits >= 32)
    {
      sat_debug (ERROR, "too many data map bits\n");
      exit(1);
    }

  /* make room for our idarrays */
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

  /*
   * read solvables
   */
  
#if 0
  sat_debug (ALWAYS, "read solvables\n");
#endif
  s = pool_id2solvable(pool, repo_add_solvable_block(repo, numsolv));
  for (i = 0; i < numsolv; i++, s++)
    {
      databits = 0;
      if (numsolvdatabits)
	{
	  for (j = 0; j < (numsolvdatabits + 7) >> 3; j++)
	    databits = (databits << 8) | read_u8(fp);
	}
      for (j = 0; j < numsolvdata; j++)
	{
	  type = solvdata[j].type;
	  if ((type & TYPE_BITMAP) != 0)
	    {
	      if (!(databits & 1))
		{
		  databits >>= 1;
		  continue;
		}
	      databits >>= 1;
	      type ^= TYPE_BITMAP;
	    }
	  id = solvdata[j].id;
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
#if 0
	      sat_debug (ALWAYS, "%s -> %s\n", id2str(pool, id), id2str(pool, did));
#endif
	      break;
	    case TYPE_U32:
	      h = read_u32(fp);
#if 0
	      sat_debug (ALWAYS, "%s -> %u\n", id2str(pool, id), h);
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
	      if (id < INTERESTED_START || id > INTERESTED_END)
		{
		  /* not interested in array */
		  while ((read_u8(fp) & 0xc0) != 0)
		    ;
		  break;
		}
	      ido = idarraydatap - repo->idarraydata;
	      idarraydatap = read_idarray(fp, numid + numrel, idmap, idarraydatap, idarraydataend);
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
	      sat_debug (ALWAYS, "%s ->\n", id2str(pool, id));
	      for (; repo->idarraydata[ido]; ido++)
	        sat_debug (ALWAYS,"  %s\n", dep2str(pool, repo->idarraydata[ido]));
#endif
	      break;
	    }
	}
    }
  xfree(idmap);
  xfree(solvdata);
}

// EOF
