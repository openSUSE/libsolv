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
  unsigned int solv_flags;
  Reldep *ran;
  Id *idarraydata;

  int idsizes[ID_NUM_INTERNAL];
  int id2key[ID_NUM_INTERNAL];
  int nsolvables;

  Id *schemadata, *schemadatap, *schema, *sp;
  Id schemaid;
  int schemadatalen;
  Id *solvschema;	/* schema of our solvables */
  Id repodataschema, repodataschema_internal;
  Id lastschema[256];
  Id lastschemakey[256];

  nsolvables = 0;
  idarraydata = repo->idarraydata;

  needid = (NeedId *)xcalloc(pool->ss.nstrings + pool->nrels, sizeof(*needid));
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

  if (repodataschema)
    {
      /* add us a schema for our repodata */
      repodataschema = nschemata++;
      *schemadatap++ = REPODATA_EXTERNAL;
      *schemadatap++ = REPODATA_KEYS;
      *schemadatap++ = REPODATA_LOCATION;
      *schemadatap++ = 0;
    }
  if (repodataschema_internal)
    {
      /* add us a schema for our repodata */
      repodataschema = nschemata++;
      *schemadatap++ = REPODATA_EXTERNAL;
      *schemadatap++ = REPODATA_KEYS;
      *schemadatap++ = 0;
    }

  /* convert all schemas to local keys */
  for (sp = schemadata; sp < schemadatap; sp++)
    *sp = id2key[*sp];

  /* write file header */
  write_u32(fp, 'S' << 24 | 'O' << 16 | 'L' << 8 | 'V');
  write_u32(fp, SOLV_VERSION_3);

  /* write counts */
  write_u32(fp, nstrings);
  write_u32(fp, nrels);
  write_u32(fp, nsolvables);
  write_u32(fp, nkeys);
  write_u32(fp, nschemata);
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
  char *prefix = xmalloc (sizeid + nstrings);
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
  xfree (prefix);

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
  write_id(fp, schemadatap - schemadata - 1);
  for (sp = schemadata + 1; sp < schemadatap; )
    {
      write_idarray(fp, pool, 0, sp);
      while (*sp++)
	;
    }

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
	  /* this looks horrible, we need some function */
	  Id id = needid[repo->repodata[i].keys[j].name].need;
	  if (id >= 64)
	    id = (id & 63) | ((id & ~63) << 1);
	  write_id(fp, id | 0x40);
	  id = repo->repodata[i].keys[j].type;
	  if (id >= 64)
	    id = (id & 63) | ((id & ~63) << 1);
	  write_id(fp, id | (j < repo->repodata[i].nkeys - 1 ? 0x40: 0));
        }
      if (repo->repodata[i].location)
        write_str(fp, repo->repodata[i].location);
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

  xfree(needid);
  xfree(solvschema);
  xfree(schemadata);
}

// EOF
