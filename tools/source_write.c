/*
 * source_write.c
 * 
 * Write Source data out to binary file
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
#include "source_write.h"

/*------------------------------------------------------------------*/
/* Id map optimizations */

typedef struct needid {
  Id need;
  Id map;
} NeedId;

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

  while ((id = *idarray++) != ID_NULL)
    {
      n++;
      while (ISRELDEP(id))
	{
	  Reldep *rd = GETRELDEP(pool, id);
	  needid[GETRELID(pool, id)].need++;
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
      write_u8(fp, ID_NULL);
      return;
    }
  for (;;)
    {
      id = *ids++;
      id = needid[ISRELDEP(id) ? GETRELID(pool, id) : id].need;
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
 * Source
 */

void
pool_writesource(Pool *pool, Source *source, FILE *fp)
{
  int i, numsolvdata;
  Solvable *s, *sstart;
  NeedId *needid;
  int nstrings, nrels;
  unsigned int sizeid;
  Reldep *ran;

  int idsizes[RPM_RPMDBID + 1];
  int bits, bitmaps;
  int nsolvables;

  nsolvables = source->nsolvables;
  sstart = pool->solvables + source->start;

  needid = (NeedId *)calloc(pool->nstrings + pool->nrels, sizeof(*needid));

  memset(idsizes, 0, sizeof(idsizes));

  for (i = 0; i < nsolvables; i++)
    {
      s = sstart + i;
      needid[s->name].need++;
      needid[s->arch].need++;
      needid[s->evr].need++;
      idsizes[SOLVABLE_PROVIDES]    += incneedid(pool, s->provides, needid);
      idsizes[SOLVABLE_REQUIRES]    += incneedid(pool, s->requires, needid);
      idsizes[SOLVABLE_CONFLICTS]   += incneedid(pool, s->conflicts, needid);
      idsizes[SOLVABLE_OBSOLETES]   += incneedid(pool, s->obsoletes, needid);
      idsizes[SOLVABLE_RECOMMENDS]  += incneedid(pool, s->recommends, needid);
      idsizes[SOLVABLE_SUGGESTS]    += incneedid(pool, s->suggests, needid);
      idsizes[SOLVABLE_SUPPLEMENTS] += incneedid(pool, s->supplements, needid);
      idsizes[SOLVABLE_ENHANCES]    += incneedid(pool, s->enhances, needid);
      idsizes[SOLVABLE_FRESHENS]    += incneedid(pool, s->freshens, needid);
    }

  idsizes[SOLVABLE_NAME] = 1;
  idsizes[SOLVABLE_ARCH] = 1;
  idsizes[SOLVABLE_EVR] = 1;
  if (source->rpmdbid)
    idsizes[RPM_RPMDBID] = 1;

  for (i = SOLVABLE_NAME; i <= RPM_RPMDBID; i++)
    {
      if (idsizes[i])
        needid[i].need++;
    }

  needid[0].need = 0;
  needid[pool->nstrings].need = 0;
  for (i = 0; i < pool->nstrings + pool->nrels; i++)
    {
      needid[i].map = i;
    }

  qsort(needid + 1, pool->nstrings - 1, sizeof(*needid), needid_cmp_need);
  qsort(needid + pool->nstrings, pool->nrels, sizeof(*needid), needid_cmp_need);

  sizeid = 0;
  for (i = 1; i < pool->nstrings; i++)
    {
      if (!needid[i].need)
        break;
      needid[i].need = 0;
      sizeid += strlen(pool->stringspace + pool->strings[needid[i].map]) + 1;
    }

  nstrings = i;
  for (i = 0; i < nstrings; i++)
    {
      needid[needid[i].map].need = i;
    }

  for (i = 0; i < pool->nrels; i++)
    {
      if (!needid[pool->nstrings + i].need)
        break;
      else
        needid[pool->nstrings + i].need = 0;
    }

  nrels = i;
  for (i = 0; i < nrels; i++)
    {
      needid[needid[pool->nstrings + i].map].need = nstrings + i;
    }

  /* write file header */
  write_u32(fp, 'S' << 24 | 'O' << 16 | 'L' << 8 | 'V');
  write_u32(fp, SOLV_VERSION);

  /* write counts */
  write_u32(fp, nstrings);
  write_u32(fp, nrels);
  write_u32(fp, nsolvables);
  write_u32(fp, sizeid);

  /*
   * write strings
   */
  for (i = 1; i < nstrings; i++)
    {
      char *str = pool->stringspace + pool->strings[needid[i].map];
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
      ran = pool->rels + (needid[pool->nstrings + i].map - pool->nstrings);
      write_id(fp, needid[ISRELDEP(ran->name) ? GETRELID(pool, ran->name) : ran->name].need);
      write_id(fp, needid[ISRELDEP(ran->evr) ? GETRELID(pool, ran->evr) : ran->evr].need);
      write_u8( fp, ran->flags);
    }

  write_u32(fp, 0);	/* no source data */

  /*
   * write Solvables
   */
  
  numsolvdata = 0;
  for (i = SOLVABLE_NAME; i <= RPM_RPMDBID; i++)
    {
      if (idsizes[i])
        numsolvdata++;
    }
  write_u32(fp, (unsigned int)numsolvdata);

  bitmaps = 0;
  for (i = SOLVABLE_NAME; i <= SOLVABLE_FRESHENS; i++)
    {
      if (!idsizes[i])
	continue;
      if (i >= SOLVABLE_PROVIDES && i <= SOLVABLE_FRESHENS)
	{
	  write_u8(fp, TYPE_IDARRAY|TYPE_BITMAP);
	  bitmaps++;
	}
      else
	write_u8(fp, TYPE_ID);
      write_id(fp, needid[i].need);
      if (i >= SOLVABLE_PROVIDES && i <= SOLVABLE_FRESHENS)
	write_u32(fp, idsizes[i]);
      else
	write_u32(fp, 0);
    }

  if (source->rpmdbid)
    {
      write_u8(fp, TYPE_U32);
      write_id(fp, needid[RPM_RPMDBID].need);
      write_u32(fp, 0);
    }

  for (i = 0; i < nsolvables; ++i)
    {
      s = sstart + i;
      bits = 0;
      if (idsizes[SOLVABLE_FRESHENS])
	bits = (bits << 1) | (s->freshens ? 1 : 0);
      if (idsizes[SOLVABLE_ENHANCES])
	bits = (bits << 1) | (s->enhances ? 1 : 0);
      if (idsizes[SOLVABLE_SUPPLEMENTS])
	bits = (bits << 1) | (s->supplements ? 1 : 0);
      if (idsizes[SOLVABLE_SUGGESTS])
	bits = (bits << 1) | (s->suggests ? 1 : 0);
      if (idsizes[SOLVABLE_RECOMMENDS])
	bits = (bits << 1) | (s->recommends ? 1 : 0);
      if (idsizes[SOLVABLE_REQUIRES])
	bits = (bits << 1) | (s->requires ? 1 : 0);
      if (idsizes[SOLVABLE_CONFLICTS])
	bits = (bits << 1) | (s->conflicts ? 1 : 0);
      if (idsizes[SOLVABLE_OBSOLETES])
	bits = (bits << 1) | (s->obsoletes ? 1 : 0);
      if (idsizes[SOLVABLE_PROVIDES])
	bits = (bits << 1) | (s->provides ? 1 : 0);

      if (bitmaps > 24)
	write_u8(fp, bits >> 24);
      if (bitmaps > 16)
	write_u8(fp, bits >> 16);
      if (bitmaps > 8)
	write_u8(fp, bits >> 8);
      if (bitmaps)
	write_u8(fp, bits);

      write_id(fp, needid[s->name].need);
      write_id(fp, needid[s->arch].need);
      write_id(fp, needid[s->evr].need);

      if (s->provides)
        write_idarray(fp, pool, needid, s->provides);
      if (s->obsoletes)
        write_idarray(fp, pool, needid, s->obsoletes);
      if (s->conflicts)
        write_idarray(fp, pool, needid, s->conflicts);
      if (s->requires)
        write_idarray(fp, pool, needid, s->requires);
      if (s->recommends)
        write_idarray(fp, pool, needid, s->recommends);
      if (s->suggests)
        write_idarray(fp, pool, needid, s->suggests);
      if (s->supplements)
        write_idarray(fp, pool, needid, s->supplements);
      if (s->enhances)
        write_idarray(fp, pool, needid, s->enhances);
      if (s->freshens)
        write_idarray(fp, pool, needid, s->freshens);
      if (source->rpmdbid)
        write_u32(fp, source->rpmdbid[i]);
    }

  free(needid);
}

// EOF
