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

#include "pool.h"
#include "util.h"
#include "repo_write.h"

/*------------------------------------------------------------------*/
/* Id map optimizations */

typedef struct needid {
  Id need;
  Id map;
} NeedId;


#define RELOFF(id) (pool->ss.nstrings + GETRELID(id))

/*
 * increment need Id
 * idarray: array of Ids, ID_NULL terminated
 * needid: array of Id->NeedId
 * 
 * return count
 * 
 */

static int
incneedid(Pool *pool, Id *idarray, NeedId *needid)
{
  if (!idarray)
    return 0;

  Id id;
  int n = 0;

  while ((id = *idarray++) != 0)
    {
      n++;
      while (ISRELDEP(id))
	{
	  Reldep *rd = GETRELDEP(pool, id);
	  needid[RELOFF(id)].need++;
	  if (ISRELDEP(rd->evr))
	    {
	      Id ida[2];
	      ida[0] = rd->evr;
	      ida[1] = 0;
	      incneedid(pool, ida, needid);
	    }
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


/*------------------------------------------------------------------*/
/* output routines */

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
      perror("write error");
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
      perror("write error");
      exit(1);
    }
}


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
      perror("write error");
      exit(1);
    }
}


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


/*
 * Repo
 */

void
repo_write(Repo *repo, FILE *fp)
{
  Pool *pool = repo->pool;
  int i, n;
  Solvable *s;
  NeedId *needid;
  int nstrings, nrels, nkeys, nschemata;
  unsigned int sizeid;
  Reldep *ran;
  Id *idarraydata;

  int idsizes[RPM_RPMDBID + 1];
  int id2key[RPM_RPMDBID + 1];
  int nsolvables;

  Id *schemadata, *schemadatap, *schema, *sp;
  Id schemaid;
  int schemadatalen;
  Id *solvschema;	/* schema of our solvables */
  Id lastschema[256];
  Id lastschemakey[256];

  nsolvables = 0;
  idarraydata = repo->idarraydata;

  needid = (NeedId *)xcalloc(pool->ss.nstrings + pool->nrels, sizeof(*needid));

  memset(idsizes, 0, sizeof(idsizes));

  for (i = repo->start, s = pool->solvables + i; i < repo->end; i++, s++)
    {
      if (s->repo != repo)
	continue;
      nsolvables++;
      needid[s->name].need++;
      needid[s->arch].need++;
      needid[s->evr].need++;
      if (s->vendor)
	{
          needid[s->vendor].need++;
          idsizes[SOLVABLE_VENDOR] = 1;
	}
      if (s->provides)
        idsizes[SOLVABLE_PROVIDES]    += incneedid(pool, idarraydata + s->provides, needid);
      if (s->requires)
        idsizes[SOLVABLE_REQUIRES]    += incneedid(pool, idarraydata + s->requires, needid);
      if (s->conflicts)
        idsizes[SOLVABLE_CONFLICTS]   += incneedid(pool, idarraydata + s->conflicts, needid);
      if (s->obsoletes)
        idsizes[SOLVABLE_OBSOLETES]   += incneedid(pool, idarraydata + s->obsoletes, needid);
      if (s->recommends)
        idsizes[SOLVABLE_RECOMMENDS]  += incneedid(pool, idarraydata + s->recommends, needid);
      if (s->suggests)
        idsizes[SOLVABLE_SUGGESTS]    += incneedid(pool, idarraydata + s->suggests, needid);
      if (s->supplements)
        idsizes[SOLVABLE_SUPPLEMENTS] += incneedid(pool, idarraydata + s->supplements, needid);
      if (s->enhances)
        idsizes[SOLVABLE_ENHANCES]    += incneedid(pool, idarraydata + s->enhances, needid);
      if (s->freshens)
        idsizes[SOLVABLE_FRESHENS]    += incneedid(pool, idarraydata + s->freshens, needid);
    }
  if (nsolvables != repo->nsolvables)
    abort();

  idsizes[SOLVABLE_NAME] = 1;
  idsizes[SOLVABLE_ARCH] = 1;
  idsizes[SOLVABLE_EVR] = 1;
  if (repo->rpmdbid)
    idsizes[RPM_RPMDBID] = 1;

  for (i = SOLVABLE_NAME; i <= RPM_RPMDBID; i++)
    {
      if (idsizes[i])
        needid[i].need++;
    }

  needid[0].need = 0;
  needid[pool->ss.nstrings].need = 0;
  for (i = 0; i < pool->ss.nstrings + pool->nrels; i++)
    needid[i].map = i;

  qsort(needid + 1, pool->ss.nstrings - 1, sizeof(*needid), needid_cmp_need);
  qsort(needid + pool->ss.nstrings, pool->nrels, sizeof(*needid), needid_cmp_need);

  sizeid = 0;
  for (i = 1; i < pool->ss.nstrings; i++)
    {
      if (!needid[i].need)
        break;
      needid[i].need = 0;
      sizeid += strlen(pool->ss.stringspace + pool->ss.strings[needid[i].map]) + 1;
    }

  nstrings = i;
  for (i = 0; i < nstrings; i++)
    needid[needid[i].map].need = i;

  for (i = 0; i < pool->nrels; i++)
    {
      if (!needid[pool->ss.nstrings + i].need)
        break;
      else
        needid[pool->ss.nstrings + i].need = 0;
    }

  nrels = i;
  for (i = 0; i < nrels; i++)
    {
      needid[needid[pool->ss.nstrings + i].map].need = nstrings + i;
    }

  /* find the keys we need */
  nkeys = 1;
  memset(id2key, 0, sizeof(id2key));
  for (i = SOLVABLE_NAME; i <= RPM_RPMDBID; i++)
    if (idsizes[i])
      id2key[i] = nkeys++;

  /* find the schemata we need */
  solvschema = xcalloc(repo->nsolvables, sizeof(Id));

  memset(lastschema, 0, sizeof(lastschema));
  memset(lastschemakey, 0, sizeof(lastschemakey));
  schemadata = xmalloc(256 * sizeof(Id));
  schemadatalen = 256;
  schemadatap = schemadata;
  *schemadatap++ = 0;
  schemadatalen--;
  nschemata = 0;
  for (i = repo->start, s = pool->solvables + i, n = 0; i < repo->end; i++, s++)
    {
      unsigned int h;
      Id *sp;

      if (s->repo != repo)
	continue;
      if (schemadatalen < 32)
	{
	  int l = schemadatap - schemadata;
          fprintf(stderr, "growing schemadata\n");
	  schemadata = xrealloc(schemadata, (schemadatap - schemadata + 256) * sizeof(Id));
	  schemadatalen = 256;
	  schemadatap = schemadata + l;
	}
      schema = schemadatap;
      *schema++ = SOLVABLE_NAME;
      *schema++ = SOLVABLE_ARCH;
      *schema++ = SOLVABLE_EVR;
      if (s->vendor)
        *schema++ = SOLVABLE_VENDOR;
      if (s->provides)
        *schema++ = SOLVABLE_PROVIDES;
      if (s->obsoletes)
        *schema++ = SOLVABLE_OBSOLETES;
      if (s->conflicts)
        *schema++ = SOLVABLE_CONFLICTS;
      if (s->requires)
        *schema++ = SOLVABLE_REQUIRES;
      if (s->recommends)
        *schema++ = SOLVABLE_RECOMMENDS;
      if (s->suggests)
        *schema++ = SOLVABLE_SUGGESTS;
      if (s->supplements)
        *schema++ = SOLVABLE_SUPPLEMENTS;
      if (s->enhances)
        *schema++ = SOLVABLE_ENHANCES;
      if (s->freshens)
        *schema++ = SOLVABLE_FRESHENS;
      if (repo->rpmdbid)
        *schema++ = RPM_RPMDBID;
      *schema++ = 0;
      for (sp = schemadatap, h = 0; *sp; )
	h = h * 7 + *sp++;
      h &= 255;
      if (lastschema[h] && !memcmp(schemadata + lastschema[h], schemadatap, (schema - schemadatap) * sizeof(Id)))
	{
	  solvschema[n++] = lastschemakey[h];
	  continue;
	}
      schemaid = 0;
      for (sp = schemadata + 1; sp < schemadatap; )
	{
	  if (!memcmp(sp, schemadatap, (schema - schemadatap) * sizeof(Id)))
	    break;
	  while (*sp++)
	    ;
	  schemaid++;
	}
      if (sp >= schemadatap)
	{
	  if (schemaid != nschemata)
	    abort();
	  lastschema[h] = schemadatap - schemadata;
	  lastschemakey[h] = schemaid;
	  schemadatalen -= schema - schemadatap;
	  schemadatap = schema;
	  nschemata++;
	}
      solvschema[n++] = schemaid;
    }
  /* convert all schemas to keys */
  for (sp = schemadata; sp < schemadatap; sp++)
    *sp = id2key[*sp];

  /* write file header */
  write_u32(fp, 'S' << 24 | 'O' << 16 | 'L' << 8 | 'V');
  write_u32(fp, SOLV_VERSION_1);

  /* write counts */
  write_u32(fp, nstrings);
  write_u32(fp, nrels);
  write_u32(fp, nsolvables);
  write_u32(fp, nkeys);
  write_u32(fp, nschemata);
  write_u32(fp, 0);	/* no info block */
#if 0
  write_u32(fp, SOLV_FLAG_VERTICAL);	/* no flags */
#else
  write_u32(fp, 0);	/* no flags */
#endif

  /*
   * write strings
   */
  write_u32(fp, sizeid);
  for (i = 1; i < nstrings; i++)
    {
      char *str = pool->ss.stringspace + pool->ss.strings[needid[i].map];
      if (fwrite(str, strlen(str) + 1, 1, fp) != 1)
	{
	  perror("write error");
	  exit(1);
	}
    }

  /*
   * write RelDeps
   */
  for (i = 0; i < nrels; i++)
    {
      ran = pool->rels + (needid[pool->ss.nstrings + i].map - pool->ss.nstrings);
      write_id(fp, needid[ISRELDEP(ran->name) ? RELOFF(ran->name) : ran->name].need);
      write_id(fp, needid[ISRELDEP(ran->evr) ? RELOFF(ran->evr) : ran->evr].need);
      write_u8( fp, ran->flags);
    }

  /*
   * write keys
   */
  for (i = SOLVABLE_NAME; i <= RPM_RPMDBID; i++)
    {
      if (!idsizes[i])
	continue;
      write_id(fp, needid[i].need);
      if (i >= SOLVABLE_PROVIDES && i <= SOLVABLE_FRESHENS)
	write_id(fp, TYPE_IDARRAY);
      else if (i == RPM_RPMDBID)
        write_id(fp, TYPE_U32);
      else
        write_id(fp, TYPE_ID);
      write_id(fp, idsizes[i]);
    }

  /*
   * write schemata
   */
  write_id(fp, schemadatap - schemadata - 1);
  for (sp = schemadata + 1; sp < schemadatap; )
    {
      write_idarray(fp, pool, 0, sp);
      while (*sp++)
	;
    }
  
#if 0
  if (1)
    {
      Id id, key;

      for (i = 0; i < nsolvables; i++)
	write_id(fp, solvschema[i]);
      unsigned char *used = xmalloc(nschemata);
      for (id = SOLVABLE_NAME; id <= RPM_RPMDBID; id++)
	{
	  key = id2key[id];
	  memset(used, 0, nschemata);
	  for (sp = schemadata + 1, i = 0; sp < schemadatap; sp++)
	    {
	      if (*sp == 0)
		i++;
	      else if (*sp == key)
		used[i] = 1;
	    }
	  for (i = repo->start, s = pool->solvables + i, n = 0; i < repo->end; i++, s++)
	    {
	      if (s->repo != repo)
		continue;
	      if (!used[solvschema[n++]])
		continue;
	      switch(id)
		{
		case SOLVABLE_NAME:
		  write_id(fp, needid[s->name].need);
		  break;
		case SOLVABLE_ARCH:
		  write_id(fp, needid[s->arch].need);
		  break;
		case SOLVABLE_EVR:
		  write_id(fp, needid[s->evr].need);
		  break;
		case SOLVABLE_VENDOR:
		  write_id(fp, needid[s->vendor].need);
		  break;
		case RPM_RPMDBID:
		  write_u32(fp, repo->rpmdbid[i - repo->start]);
		  break;
		case SOLVABLE_PROVIDES:
		  write_idarray(fp, pool, needid, idarraydata + s->provides);
		  break;
		case SOLVABLE_OBSOLETES:
		  write_idarray(fp, pool, needid, idarraydata + s->obsoletes);
		  break;
		case SOLVABLE_CONFLICTS:
		  write_idarray(fp, pool, needid, idarraydata + s->conflicts);
		  break;
		case SOLVABLE_REQUIRES:
		  write_idarray(fp, pool, needid, idarraydata + s->requires);
		  break;
		case SOLVABLE_RECOMMENDS:
		  write_idarray(fp, pool, needid, idarraydata + s->recommends);
		  break;
		case SOLVABLE_SUPPLEMENTS:
		  write_idarray(fp, pool, needid, idarraydata + s->supplements);
		  break;
		case SOLVABLE_SUGGESTS:
		  write_idarray(fp, pool, needid, idarraydata + s->suggests);
		  break;
		case SOLVABLE_ENHANCES:
		  write_idarray(fp, pool, needid, idarraydata + s->enhances);
		  break;
		case SOLVABLE_FRESHENS:
		  write_idarray(fp, pool, needid, idarraydata + s->freshens);
		  break;
		}
	    }
	}
      xfree(used);
      xfree(needid);
      xfree(solvschema);
      xfree(schemadata);
      return;
    }
  
#endif

  /*
   * write Solvables
   */
  for (i = repo->start, s = pool->solvables + i, n = 0; i < repo->end; i++, s++)
    {
      if (s->repo != repo)
	continue;
      /* keep in sync with schema generation! */
      write_id(fp, solvschema[n++]);
      write_id(fp, needid[s->name].need);
      write_id(fp, needid[s->arch].need);
      write_id(fp, needid[s->evr].need);
      if (s->vendor)
        write_id(fp, needid[s->vendor].need);
      if (s->provides)
        write_idarray(fp, pool, needid, idarraydata + s->provides);
      if (s->obsoletes)
        write_idarray(fp, pool, needid, idarraydata + s->obsoletes);
      if (s->conflicts)
        write_idarray(fp, pool, needid, idarraydata + s->conflicts);
      if (s->requires)
        write_idarray(fp, pool, needid, idarraydata + s->requires);
      if (s->recommends)
        write_idarray(fp, pool, needid, idarraydata + s->recommends);
      if (s->suggests)
        write_idarray(fp, pool, needid, idarraydata + s->suggests);
      if (s->supplements)
        write_idarray(fp, pool, needid, idarraydata + s->supplements);
      if (s->enhances)
        write_idarray(fp, pool, needid, idarraydata + s->enhances);
      if (s->freshens)
        write_idarray(fp, pool, needid, idarraydata + s->freshens);
      if (repo->rpmdbid)
        write_u32(fp, repo->rpmdbid[i - repo->start]);
    }

  xfree(needid);
  xfree(solvschema);
  xfree(schemadata);
}

// EOF
