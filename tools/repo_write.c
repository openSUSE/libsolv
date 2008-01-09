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
#include <assert.h>

#include "pool.h"
#include "util.h"
#include "repo_write.h"

/* #define IGNORE_NEED_FREQUENCY */

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

static Pool *cmp_pool;
static int
needid_cmp_need_s(const void *ap, const void *bp)
{
  const NeedId *a = ap;
  const NeedId *b = bp;
  if (a == b)
    return 0;
#ifdef IGNORE_NEED_FREQUENCY
  if (a->need == 0)
    return 1;
  else if (b->need == 0)
    return -1;
#else
  int r;
  r = b->need - a->need;
  if (r)
    return r;
#endif
  char *as = cmp_pool->ss.stringspace + cmp_pool->ss.strings[a->map];
  char *bs = cmp_pool->ss.stringspace + cmp_pool->ss.strings[b->map];
  size_t alen = strlen (as);
  size_t blen = strlen (bs);
  return memcmp (as, bs, alen < blen ? alen + 1 : blen + 1);
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

static void
write_str(FILE *fp, const char *str)
{
  if (fputs (str, fp) == EOF
      || putc (0, fp) == EOF)
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

static int
cmp_ids (const void *pa, const void *pb)
{
  Id a = *(Id *)pa;
  Id b = *(Id *)pb;
  return a - b;
}

static void
write_idarray_sort(FILE *fp, Pool *pool, NeedId *needid, Id *ids)
{
  int len, i;
  if (!ids)
    return;
  if (!*ids)
    {
      write_u8 (fp, 0);
      return;
    }
  /* If we ever share idarrays we can't do this in-place.  */
  for (len = 0; ids[len]; len++)
    {
      Id id = ids[len];
      if (needid)
        ids[len] = needid[ISRELDEP(id) ? RELOFF(id) : id].need;
    }

  /* That bloody solvable:prereqmarker needs to stay in position :-(  */
  Id prereq = SOLVABLE_PREREQMARKER;
  if (needid)
    prereq = needid[prereq].need;
  for (i = 0; i < len; i++)
    if (ids[i] == prereq)
      break;
  if (i > 1)
    qsort (ids, i, sizeof (Id), cmp_ids);
  if ((len - i) > 2)
    qsort (ids + i + 1, len - i - 1, sizeof (Id), cmp_ids);

  Id old = 0;
  for (i = 0; i < len; i++)
    /* Ugly PREREQ handling.  A "difference" of 0 is the prereq marker,
       hence all real differences are offsetted by 1.  Otherwise we would
       have to handle negative differences, which would cost code space for
       the encoding of the sign.  We loose the exact mapping of prereq here,
       but we know the result, so we can recover from that in the reader.  */
    if (ids[i] == prereq)
      old = ids[i] = 0;
    else
      {
        ids[i] -= old;
	old = ids[i] + old;
	/* XXX If difference is zero we have multiple equal elements,
	   we might want to skip writing them out.  */
	ids[i]++;
      }

  /* The differencing above produces many runs of ones and twos.  I tried
     fairly elaborate schemes to RLE those, but they give only very mediocre
     improvements in compression, as coding the escapes costs quite some
     space.  Even if they are coded only as bits in IDs.  The best improvement
     was about 2.7% for the whole .solv file.  It's probably better to
     invest some complexity into sharing idarrays, than RLEing.  */
  for (i = 0; i < len - 1; i++)
    {
      Id id = ids[i];
      if (id >= 64)
	id = (id & 63) | ((id & ~63) << 1);
      write_id(fp, id | 64);
    }
  old = ids[i];
  if (old >= 64)
    old = (old & 63) | ((old & ~63) << 1);
  write_id(fp, old);
}

static inline void
write_id_value(FILE *fp, Id id, Id value, int eof)
{
  if (id >= 64)
    id = (id & 63) | ((id & ~63) << 1);
  write_id(fp, id | 64);
  if (value >= 64)
    value = (value & 63) | ((value & ~63) << 1);
  write_id(fp, value | (eof ? 0 : 64));
}

struct schemata {
  int nschemata;
  Id *schemadata, *schemadatap;
  int schemadatafree;

  Id lastschema[256];
  Id lastschemakey[256];
};

static Id
addschema(struct schemata *schemata, Id *schema)
{
  int h, len;
  Id *sp, schemaid;

  for (sp = schema, len = 0, h = 0; *sp; len++)
    h = h * 7 + *sp++;
  h &= 255;
  len++;
  if (schemata->lastschema[h] && !memcmp(schemata->schemadata + schemata->lastschema[h], schema, len * sizeof(Id)))
    return schemata->lastschemakey[h];

  schemaid = 0;
  for (sp = schemata->schemadata + 1; sp < schemata->schemadatap; )
    {
      if (!memcmp(sp, schemata->schemadatap, len * sizeof(Id)))
	return schemaid;
      while (*sp++)
	;
      schemaid++;
    }

  /* a new one */
  if (len > schemata->schemadatafree)
    {
      int l = schemata->schemadatap - schemata->schemadata;
      schemata->schemadata = sat_realloc(schemata->schemadata, (schemata->schemadatap - schemata->schemadata + len + 256) * sizeof(Id));
      schemata->schemadatafree = len + 256;
      schemata->schemadatap = schemata->schemadata + l;
      if (l == 0)
	{
	  /* leave first one free so that our lastschema test works */
	  *schemata->schemadatap++ = 0;
	  schemata->schemadatafree--;
	}
    }
  if (schemaid != schemata->nschemata)
    abort();
  schemata->lastschema[h] = schemata->schemadatap - schemata->schemadata;
  schemata->lastschemakey[h] = schemaid;
  memcpy(schemata->schemadatap, schema, len * sizeof(Id));
  schemata->schemadatafree -= len;
  schemata->schemadatap += len;
  schemata->nschemata++;
  return schemaid;
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
  int nstrings, nrels, nkeys;
  unsigned int sizeid;
  unsigned int solv_flags;
  Reldep *ran;
  Id *idarraydata;

  int idsizes[ID_NUM_INTERNAL];
  int id2key[ID_NUM_INTERNAL];
  int nsolvables;

  Id schema[ID_NUM_INTERNAL], *sp;
  struct schemata schemata;
  Id *solvschema;	/* schema of our solvables */
  Id repodataschema, repodataschema_internal;

  nsolvables = 0;
  idarraydata = repo->idarraydata;

  needid = sat_calloc(pool->ss.nstrings + pool->nrels, sizeof(*needid));
  memset(idsizes, 0, sizeof(idsizes));

  repodataschema = repodataschema_internal = 0;
  for (i = 0; i < repo->nrepodata; i++)
    {
      int j;
      idsizes[REPODATA_EXTERNAL] = 1;
      idsizes[REPODATA_KEYS]++;
      if (repo->repodata[i].location)
	{
	  repodataschema = 1;		/* mark that we need it */
          idsizes[REPODATA_LOCATION] = 1;
	}
      else
	repodataschema_internal = 1;	/* mark that we need it */
      for (j = 0; j < repo->repodata[i].nkeys; j++)
        needid[repo->repodata[i].keys[j].name].need++;
      idsizes[REPODATA_KEYS] += 2 * repo->repodata[i].nkeys;
    }

  idsizes[SOLVABLE_NAME] = 1;
  idsizes[SOLVABLE_ARCH] = 1;
  idsizes[SOLVABLE_EVR] = 1;
  if (repo->rpmdbid)
    idsizes[RPM_RPMDBID] = 1;
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

  for (i = SOLVABLE_NAME; i < ID_NUM_INTERNAL; i++)
    {
      if (idsizes[i])
        needid[i].need++;
    }

  needid[0].need = 0;
  needid[pool->ss.nstrings].need = 0;
  for (i = 0; i < pool->ss.nstrings + pool->nrels; i++)
    needid[i].map = i;

  cmp_pool = pool;
  qsort(needid + 1, pool->ss.nstrings - 1, sizeof(*needid), needid_cmp_need_s);
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
  for (i = SOLVABLE_NAME; i < ID_NUM_INTERNAL; i++)
    if (idsizes[i])
      id2key[i] = nkeys++;

  /* find the schemata we need */
  memset(&schemata, 0, sizeof(schemata));
  solvschema = sat_calloc(repo->nsolvables, sizeof(Id));

  for (i = repo->start, s = pool->solvables + i, n = 0; i < repo->end; i++, s++)
    {
      if (s->repo != repo)
	continue;
      sp = schema;
      *sp++ = SOLVABLE_NAME;
      *sp++ = SOLVABLE_ARCH;
      *sp++ = SOLVABLE_EVR;
      if (s->vendor)
        *sp++ = SOLVABLE_VENDOR;
      if (s->provides)
        *sp++ = SOLVABLE_PROVIDES;
      if (s->obsoletes)
        *sp++ = SOLVABLE_OBSOLETES;
      if (s->conflicts)
        *sp++ = SOLVABLE_CONFLICTS;
      if (s->requires)
        *sp++ = SOLVABLE_REQUIRES;
      if (s->recommends)
        *sp++ = SOLVABLE_RECOMMENDS;
      if (s->suggests)
        *sp++ = SOLVABLE_SUGGESTS;
      if (s->supplements)
        *sp++ = SOLVABLE_SUPPLEMENTS;
      if (s->enhances)
        *sp++ = SOLVABLE_ENHANCES;
      if (s->freshens)
        *sp++ = SOLVABLE_FRESHENS;
      if (repo->rpmdbid)
        *sp++ = RPM_RPMDBID;
      *sp = 0;
      solvschema[n++] = addschema(&schemata, schema);
    }

  if (repodataschema)
    {
      /* add us a schema for our repodata */
      sp = schema;
      *sp++ = REPODATA_EXTERNAL;
      *sp++ = REPODATA_KEYS;
      *sp++ = REPODATA_LOCATION;
      *sp = 0;
      repodataschema = addschema(&schemata, schema);
    }
  if (repodataschema_internal)
    {
      sp = schema;
      *sp++ = REPODATA_EXTERNAL;
      *sp++ = REPODATA_KEYS;
      *sp = 0;
      repodataschema_internal = addschema(&schemata, schema);
    }

  /* convert all schemas to local keys */
  if (schemata.nschemata)
    for (sp = schemata.schemadata; sp < schemata.schemadatap; sp++)
      *sp = id2key[*sp];

  /* write file header */
  write_u32(fp, 'S' << 24 | 'O' << 16 | 'L' << 8 | 'V');
  write_u32(fp, SOLV_VERSION_3);

  /* write counts */
  write_u32(fp, nstrings);
  write_u32(fp, nrels);
  write_u32(fp, nsolvables);
  write_u32(fp, nkeys);
  write_u32(fp, schemata.nschemata);
  write_u32(fp, repo->nrepodata);  /* info blocks.  */
  solv_flags = 0;
  solv_flags |= SOLV_FLAG_PREFIX_POOL;
#if 0
  solv_flags |= SOLV_FLAG_VERTICAL;
#endif
  write_u32(fp, solv_flags);

  /* Build the prefix-encoding of the string pool.  We need to know
     the size of that before writing it to the file, so we have to
     build a separate buffer for that.  As it's temporarily possible
     that this actually is an expansion we can't easily reuse the 
     stringspace for this.  The max expansion per string is 1 byte,
     so it will fit into sizeid+nstrings bytes.  */
  char *prefix = sat_malloc (sizeid + nstrings);
  char *pp = prefix;
  char *old_str = "";
  for (i = 1; i < nstrings; i++)
    {
      char *str = pool->ss.stringspace + pool->ss.strings[needid[i].map];
      int same;
      size_t len;
      for (same = 0; same < 255; same++)
	if (old_str[same] != str[same])
	  break;
      *pp++ = same;
      len = strlen (str + same) + 1;
      memcpy (pp, str + same, len);
      pp += len;
      old_str = str;
    }

  /*
   * write strings
   */
  write_u32(fp, sizeid);
  write_u32(fp, pp - prefix);
  if (fwrite(prefix, pp - prefix, 1, fp) != 1)
    {
      perror("write error");
      exit(1);
    }
  sat_free (prefix);

  /*
   * write RelDeps
   */
  for (i = 0; i < nrels; i++)
    {
      ran = pool->rels + (needid[pool->ss.nstrings + i].map - pool->ss.nstrings);
      write_id(fp, needid[ISRELDEP(ran->name) ? RELOFF(ran->name) : ran->name].need);
      write_id(fp, needid[ISRELDEP(ran->evr) ? RELOFF(ran->evr) : ran->evr].need);
      write_u8(fp, ran->flags);
    }

  /*
   * write keys
   */
  for (i = SOLVABLE_NAME; i < ID_NUM_INTERNAL; i++)
    {
      if (!idsizes[i])
	continue;
      write_id(fp, needid[i].need);
      if (i >= SOLVABLE_PROVIDES && i <= SOLVABLE_FRESHENS)
	write_id(fp, TYPE_REL_IDARRAY);
      else if (i == RPM_RPMDBID)
        write_id(fp, TYPE_U32);
      else if (i == REPODATA_EXTERNAL)
        write_id(fp, TYPE_VOID);
      else if (i == REPODATA_KEYS)
        write_id(fp, TYPE_IDVALUEARRAY);
      else if (i == REPODATA_LOCATION)
        write_id(fp, TYPE_STR);
      else
        write_id(fp, TYPE_ID);
      write_id(fp, idsizes[i]);
    }

  /*
   * write schemata
   */
  if (schemata.nschemata)
    {
      write_id(fp, schemata.schemadatap - schemata.schemadata - 1);
      for (sp = schemata.schemadata + 1; sp < schemata.schemadatap; )
	{
	  write_idarray(fp, pool, 0, sp);
	  while (*sp++)
	    ;
	}
    }
  else
    write_id(fp, 0);

  /*
   * write info block
   */
  for (i = 0; i < repo->nrepodata; i++)
    {
      int j;

      if (repo->repodata[i].location)
        write_id(fp, repodataschema);
      else
        write_id(fp, repodataschema_internal);
      /* keys + location, write idarray */
      for (j = 0; j < repo->repodata[i].nkeys; j++)
        {
	  Id id = needid[repo->repodata[i].keys[j].name].need;
	  write_id_value(fp, id, repo->repodata[i].keys[j].type, j == repo->repodata[i].nkeys - 1);
        }
      if (repo->repodata[i].location)
        write_str(fp, repo->repodata[i].location);
    }

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
        write_idarray_sort(fp, pool, needid, idarraydata + s->provides);
      if (s->obsoletes)
        write_idarray_sort(fp, pool, needid, idarraydata + s->obsoletes);
      if (s->conflicts)
        write_idarray_sort(fp, pool, needid, idarraydata + s->conflicts);
      if (s->requires)
        write_idarray_sort(fp, pool, needid, idarraydata + s->requires);
      if (s->recommends)
        write_idarray_sort(fp, pool, needid, idarraydata + s->recommends);
      if (s->suggests)
        write_idarray_sort(fp, pool, needid, idarraydata + s->suggests);
      if (s->supplements)
        write_idarray_sort(fp, pool, needid, idarraydata + s->supplements);
      if (s->enhances)
        write_idarray_sort(fp, pool, needid, idarraydata + s->enhances);
      if (s->freshens)
        write_idarray_sort(fp, pool, needid, idarraydata + s->freshens);
      if (repo->rpmdbid)
        write_u32(fp, repo->rpmdbid[i - repo->start]);
    }

  sat_free(needid);
  sat_free(solvschema);
  sat_free(schemata.schemadata);
}

// EOF
