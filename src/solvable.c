/*
 * Copyright (c) 2008, Novell Inc.
 *
 * This program is licensed under the BSD license, read LICENSE.BSD
 * for further information
 */

/*
 * solvable.c
 *
 * set/retrieve data from solvables
 */

#include <stdio.h>
#include <stdlib.h>
#include <stdarg.h>
#include <unistd.h>
#include <string.h>

#include "pool.h"
#include "repo.h"
#include "util.h"
#include "policy.h"
#include "poolvendor.h"
#include "chksum.h"
#include "linkedpkg.h"
#include "evr.h"

const char *
pool_solvable2str(Pool *pool, Solvable *s)
{
  const char *n, *e, *a;
  int nl, el, al;
  char *p;
  n = pool_id2str(pool, s->name);
  e = s->evr ? pool_id2str(pool, s->evr) : "";
  /* XXX: may want to skip the epoch here */
  a = s->arch ? pool_id2str(pool, s->arch) : "";
  nl = strlen(n);
  el = strlen(e);
  al = strlen(a);
  if (pool->havedistepoch)
    {
      /* strip the distepoch from the evr */
      const char *de = strrchr(e, '-');
      if (de && (de = strchr(de, ':')) != 0)
	el = de - e;
    }
  p = pool_alloctmpspace(pool, nl + el + al + 3);
  strcpy(p, n);
  if (el)
    {
      p[nl++] = '-';
      strncpy(p + nl, e, el);
      p[nl + el] = 0;
    }
  if (al)
    {
      p[nl + el] = pool->disttype == DISTTYPE_HAIKU ? '-' : '.';
      strcpy(p + nl + el + 1, a);
    }
  if (pool->disttype == DISTTYPE_CONDA && solvable_lookup_type(s, SOLVABLE_BUILDFLAVOR))
    {
      Queue flavorq;
      int i;
      queue_init(&flavorq);
      solvable_lookup_idarray(s, SOLVABLE_BUILDFLAVOR, &flavorq);
      for (i = 0; i < flavorq.count; i++)
	p = pool_tmpappend(pool, p, "-", pool_id2str(pool, flavorq.elements[i]));
      queue_free(&flavorq);
    }
  return p;
}

Id
solvable_lookup_type(Solvable *s, Id keyname)
{
  if (!s->repo)
    return 0;
  return repo_lookup_type(s->repo, s - s->repo->pool->solvables, keyname);
}

Id
solvable_lookup_id(Solvable *s, Id keyname)
{
  if (!s->repo)
    return 0;
  return repo_lookup_id(s->repo, s - s->repo->pool->solvables, keyname);
}

int
solvable_lookup_idarray(Solvable *s, Id keyname, Queue *q)
{
  if (!s->repo)
    {
      queue_empty(q);
      return 0;
    }
  return repo_lookup_idarray(s->repo, s - s->repo->pool->solvables, keyname, q);
}

int
solvable_lookup_deparray(Solvable *s, Id keyname, Queue *q, Id marker)
{
  if (!s->repo)
    {
      queue_empty(q);
      return 0;
    }
  return repo_lookup_deparray(s->repo, s - s->repo->pool->solvables, keyname, q, marker);
}

static const char *
solvable_lookup_str_joinarray(Solvable *s, Id keyname, const char *joinstr)
{
  Queue q;
  Id qbuf[10];
  char *str = 0;

  queue_init_buffer(&q, qbuf, sizeof(qbuf)/sizeof(*qbuf));
  if (solvable_lookup_idarray(s, keyname, &q) && q.count)
    {
      Pool *pool = s->repo->pool;
      if (q.count == 1)
	str = (char *)pool_id2str(pool, q.elements[0]);
      else
	{
	  int i;
	  str = pool_tmpjoin(pool, pool_id2str(pool, q.elements[0]), 0, 0);
	  for (i = 1; i < q.count; i++)
	    str = pool_tmpappend(pool, str, joinstr, pool_id2str(pool, q.elements[i]));
	}
    }
  queue_free(&q);
  return str;
}

const char *
solvable_lookup_str(Solvable *s, Id keyname)
{
  const char *str;
  if (!s->repo)
    return 0;
  str = repo_lookup_str(s->repo, s - s->repo->pool->solvables, keyname);
  if (!str && (keyname == SOLVABLE_LICENSE || keyname == SOLVABLE_GROUP || keyname == SOLVABLE_BUILDFLAVOR))
    str = solvable_lookup_str_joinarray(s, keyname, ", ");
  return str;
}

static const char *
solvable_lookup_str_base(Solvable *s, Id keyname, Id basekeyname, int usebase)
{
  Pool *pool;
  const char *str, *basestr;
  Id p, pp, name;
  Solvable *s2;
  int pass;

  if (!s->repo)
    return 0;
  pool = s->repo->pool;
  str = solvable_lookup_str(s, keyname);
  if (str || keyname == basekeyname)
    return str;
  basestr = solvable_lookup_str(s, basekeyname);
  if (!basestr)
    return 0;
  /* search for a solvable with same name and same base that has the
   * translation */
  if (!pool->whatprovides)
    return usebase ? basestr : 0;
  name = s->name;
  /* we do this in two passes, first same vendor, then all other vendors */
  for (pass = 0; pass < 2; pass++)
    {
      FOR_PROVIDES(p, pp, name)
	{
	  s2 = pool->solvables + p;
	  if (s2->name != name)
	    continue;
	  if ((s->vendor == s2->vendor) != (pass == 0))
	    continue;
	  str = solvable_lookup_str(s2, basekeyname);
	  if (!str || strcmp(str, basestr))
	    continue;
	  str = solvable_lookup_str(s2, keyname);
	  if (str)
	    return str;
	}
#ifdef ENABLE_LINKED_PKGS
      /* autopattern/product translation magic */
      if (pass == 1 && name == s->name)
	{
	  name = find_autopackage_name(pool, s);
	  if (name && name != s->name)
	    pass = -1;	/* start over with new name */
	}
#endif
    }
  return usebase ? basestr : 0;
}

const char *
solvable_lookup_str_poollang(Solvable *s, Id keyname)
{
  Pool *pool;
  int i, cols;
  const char *str;
  Id *row;

  if (!s->repo)
    return 0;
  pool = s->repo->pool;
  if (!pool->nlanguages)
    return solvable_lookup_str(s, keyname);
  cols = pool->nlanguages + 1;
  if (!pool->languagecache)
    {
      pool->languagecache = solv_calloc(cols * ID_NUM_INTERNAL, sizeof(Id));
      pool->languagecacheother = 0;
    }
  if (keyname >= ID_NUM_INTERNAL)
    {
      row = pool->languagecache + ID_NUM_INTERNAL * cols;
      for (i = 0; i < pool->languagecacheother; i++, row += cols)
	if (*row == keyname)
	  break;
      if (i >= pool->languagecacheother)
	{
	  pool->languagecache = solv_realloc2(pool->languagecache, pool->languagecacheother + 1, cols * sizeof(Id));
	  row = pool->languagecache + cols * (ID_NUM_INTERNAL + pool->languagecacheother++);
	  *row = keyname;
	}
    }
  else
    row = pool->languagecache + keyname * cols;
  row++;	/* skip keyname */
  for (i = 0; i < pool->nlanguages; i++, row++)
    {
      if (!*row)
        *row = pool_id2langid(pool, keyname, pool->languages[i], 1);
      str = solvable_lookup_str_base(s, *row, keyname, 0);
      if (str)
	return str;
    }
  return solvable_lookup_str(s, keyname);
}

const char *
solvable_lookup_str_lang(Solvable *s, Id keyname, const char *lang, int usebase)
{
  Id id;
  if (!s->repo)
    return 0;
  id = pool_id2langid(s->repo->pool, keyname, lang, 0);
  if (id)
    return solvable_lookup_str_base(s, id, keyname, usebase);
  if (!usebase)
    return 0;
  return solvable_lookup_str(s, keyname);
}

unsigned long long
solvable_lookup_num(Solvable *s, Id keyname, unsigned long long notfound)
{
  if (!s->repo)
    return notfound;
  return repo_lookup_num(s->repo, s - s->repo->pool->solvables, keyname, notfound);
}

unsigned long long
solvable_lookup_sizek(Solvable *s, Id keyname, unsigned long long notfound)
{
  unsigned long long size;
  if (!s->repo)
    return notfound;
  size = solvable_lookup_num(s, keyname, (unsigned long long)-1);
  return size == (unsigned long long)-1 ? notfound : ((size + 1023) >> 10);
}

int
solvable_lookup_void(Solvable *s, Id keyname)
{
  if (!s->repo)
    return 0;
  return repo_lookup_void(s->repo, s - s->repo->pool->solvables, keyname);
}

int
solvable_lookup_bool(Solvable *s, Id keyname)
{
  Id type;
  if (!s->repo)
    return 0;
  /* historic nonsense: there are two ways of storing a bool, as num == 1 or void. test both. */
  type = repo_lookup_type(s->repo, s - s->repo->pool->solvables, keyname);
  if (type == REPOKEY_TYPE_VOID)
    return 1;
  if (type == REPOKEY_TYPE_NUM || type == REPOKEY_TYPE_CONSTANT)
    return repo_lookup_num(s->repo, s - s->repo->pool->solvables, keyname, 0) == 1;
  return 0;
}

const unsigned char *
solvable_lookup_bin_checksum(Solvable *s, Id keyname, Id *typep)
{
  if (!s->repo)
    {
      *typep = 0;
      return 0;
    }
  return repo_lookup_bin_checksum(s->repo, s - s->repo->pool->solvables, keyname, typep);
}

const char *
solvable_lookup_checksum(Solvable *s, Id keyname, Id *typep)
{
  const unsigned char *chk = solvable_lookup_bin_checksum(s, keyname, typep);
  return chk ? pool_bin2hex(s->repo->pool, chk, solv_chksum_len(*typep)) : 0;
}

unsigned int
solvable_lookup_count(Solvable *s, Id keyname)
{
  return s->repo ? repo_lookup_count(s->repo, s - s->repo->pool->solvables, keyname) : 0;
}

static inline const char *
evrid2vrstr(Pool *pool, Id evrid)
{
  const char *p, *evr = pool_id2str(pool, evrid);
  if (!evr)
    return evr;
  for (p = evr; *p >= '0' && *p <= '9'; p++)
    ;
  return p != evr && *p == ':' && p[1] ? p + 1 : evr;
}

const char *
solvable_lookup_location(Solvable *s, unsigned int *medianrp)
{
  Pool *pool;
  int l = 0;
  char *loc;
  const char *mediadir, *mediafile;

  if (medianrp)
    *medianrp = 0;
  if (!s->repo)
    return 0;
  pool = s->repo->pool;
  if (medianrp)
    *medianrp = solvable_lookup_num(s, SOLVABLE_MEDIANR, 0);
  if (solvable_lookup_void(s, SOLVABLE_MEDIADIR))
    mediadir = pool_id2str(pool, s->arch);
  else
    mediadir = solvable_lookup_str(s, SOLVABLE_MEDIADIR);
  if (mediadir)
    l = strlen(mediadir) + 1;
  if (solvable_lookup_void(s, SOLVABLE_MEDIAFILE))
    {
      const char *name, *evr, *arch;
      name = pool_id2str(pool, s->name);
      evr = evrid2vrstr(pool, s->evr);
      arch = pool_id2str(pool, s->arch);
      /* name-vr.arch.rpm */
      loc = pool_alloctmpspace(pool, l + strlen(name) + strlen(evr) + strlen(arch) + 7);
      if (mediadir)
	sprintf(loc, "%s/%s-%s.%s.rpm", mediadir, name, evr, arch);
      else
	sprintf(loc, "%s-%s.%s.rpm", name, evr, arch);
    }
  else
    {
      mediafile = solvable_lookup_str(s, SOLVABLE_MEDIAFILE);
      if (!mediafile)
	return 0;
      loc = pool_alloctmpspace(pool, l + strlen(mediafile) + 1);
      if (mediadir)
	sprintf(loc, "%s/%s", mediadir, mediafile);
      else
	strcpy(loc, mediafile);
    }
  return loc;
}

const char *
solvable_get_location(Solvable *s, unsigned int *medianrp)
{
  const char *loc = solvable_lookup_location(s, medianrp);
  if (medianrp && *medianrp == 0)
    *medianrp = 1;	/* compat, to be removed */
  return loc;
}

const char *
solvable_lookup_sourcepkg(Solvable *s)
{
  Pool *pool;
  const char *evr, *name;
  Id archid;

  if (!s->repo)
    return 0;
  pool = s->repo->pool;
  if (solvable_lookup_void(s, SOLVABLE_SOURCENAME))
    name = pool_id2str(pool, s->name);
  else
    name = solvable_lookup_str(s, SOLVABLE_SOURCENAME);
  if (!name)
    return 0;
  archid = solvable_lookup_id(s, SOLVABLE_SOURCEARCH);
  if (solvable_lookup_void(s, SOLVABLE_SOURCEEVR))
    evr = evrid2vrstr(pool, s->evr);
  else
    evr = solvable_lookup_str(s, SOLVABLE_SOURCEEVR);
  if (archid == ARCH_SRC || archid == ARCH_NOSRC)
    {
      char *str;
      str = pool_tmpjoin(pool, name, evr ? "-" : 0, evr);
      str = pool_tmpappend(pool, str, ".", pool_id2str(pool, archid));
      return pool_tmpappend(pool, str, ".rpm", 0);
    }
  else
    return name;	/* FIXME */
}


/*****************************************************************************/

/*
 * Create maps containing the state of each solvable. Input is a "installed" queue,
 * it contains all solvable ids that are considered to be installed.
 *
 * The created maps can be used for * pool_calc_duchanges() and
 * pool_calc_installsizechange().
 *
 */
void
pool_create_state_maps(Pool *pool, Queue *installed, Map *installedmap, Map *conflictsmap)
{
  int i;
  Solvable *s;
  Id p, *dp;
  Id *conp, con;

  map_init(installedmap, pool->nsolvables);
  if (conflictsmap)
    map_init(conflictsmap, pool->nsolvables);
  for (i = 0; i < installed->count; i++)
    {
      p = installed->elements[i];
      if (p <= 0)	/* makes it work with decisionq */
	continue;
      MAPSET(installedmap, p);
      if (!conflictsmap)
	continue;
      s = pool->solvables + p;
      if (!s->conflicts)
	continue;
      conp = s->repo->idarraydata + s->conflicts;
      while ((con = *conp++) != 0)
	{
	  dp = pool_whatprovides_ptr(pool, con);
	  for (; *dp; dp++)
	    MAPSET(conflictsmap, *dp);
	}
    }
}

/* Tests if two solvables have identical content. Currently
 * both solvables need to come from the same pool
 */

int
solvable_identical(Solvable *s1, Solvable *s2)
{
  unsigned long long bt1, bt2;
  Id rq1, rq2;
  Id *reqp;
  if (s1->name != s2->name)
    return 0;
  if (s1->arch != s2->arch)
    return 0;
  if (s1->evr != s2->evr)
    return 0;

  /* check vendor, map missing vendor to empty string */
  if ((s1->vendor ? s1->vendor : 1) != (s2->vendor ? s2->vendor : 1))
    {
      /* workaround for bug 881493 */
      if (s1->repo && !strncmp(pool_id2str(s1->repo->pool, s1->name), "product:", 8))
	return 1;
      return 0;
    }

  /* looking good, try some fancier stuff */
  /* might also look up the package checksum here */
  bt1 = solvable_lookup_num(s1, SOLVABLE_BUILDTIME, 0);
  bt2 = solvable_lookup_num(s2, SOLVABLE_BUILDTIME, 0);
  if (bt1 && bt2)
    {
      if (bt1 != bt2)
        return 0;
    }
  else
    {
      if (s1->repo)
	{
          /* workaround for bugs 881493 and 885830*/
	  const char *n = pool_id2str(s1->repo->pool, s1->name);
	  if (!strncmp(n, "product:", 8) || !strncmp(n, "application:", 12))
	    return 1;
	}
      /* look at requires in a last attempt to find recompiled packages */
      rq1 = rq2 = 0;
      if (s1->requires)
	for (reqp = s1->repo->idarraydata + s1->requires; *reqp; reqp++)
	  rq1 ^= *reqp;
      if (s2->requires)
	for (reqp = s2->repo->idarraydata + s2->requires; *reqp; reqp++)
	  rq2 ^= *reqp;
      if (rq1 != rq2)
	 return 0;
    }
  if (s1->repo && s1->repo->pool->disttype == DISTTYPE_CONDA)
    {
      /* check buildflavor and buildversion */
      const char *str1, *str2;
      str1 = solvable_lookup_str(s1, SOLVABLE_BUILDFLAVOR);
      str2 = solvable_lookup_str(s2, SOLVABLE_BUILDFLAVOR);
      if (str1 != str2 && (!str1 || !str2 || strcmp(str1, str2) != 0))
	return 0;
      str1 = solvable_lookup_str(s1, SOLVABLE_BUILDVERSION);
      str2 = solvable_lookup_str(s2, SOLVABLE_BUILDVERSION);
      if (str1 != str2 && (!str1 || !str2 || strcmp(str1, str2) != 0))
	return 0;
    }
  return 1;
}

/* return the self provide dependency of a solvable */
Id
solvable_selfprovidedep(Solvable *s)
{
  Pool *pool;
  Reldep *rd;
  Id prov, *provp;

  if (!s->repo)
    return s->name;
  pool = s->repo->pool;
  if (s->provides)
    {
      provp = s->repo->idarraydata + s->provides;
      while ((prov = *provp++) != 0)
	{
	  if (!ISRELDEP(prov))
	    continue;
	  rd = GETRELDEP(pool, prov);
	  if (rd->name == s->name && rd->evr == s->evr && rd->flags == REL_EQ)
	    return prov;
	}
    }
  return pool_rel2id(pool, s->name, s->evr, REL_EQ, 1);
}

/* setter functions, simply call the repo variants */
void
solvable_set_id(Solvable *s, Id keyname, Id id)
{
  repo_set_id(s->repo, s - s->repo->pool->solvables, keyname, id);
}

void
solvable_set_num(Solvable *s, Id keyname, unsigned long long num)
{
  repo_set_num(s->repo, s - s->repo->pool->solvables, keyname, num);
}

void
solvable_set_str(Solvable *s, Id keyname, const char *str)
{
  repo_set_str(s->repo, s - s->repo->pool->solvables, keyname, str);
}

void
solvable_set_poolstr(Solvable *s, Id keyname, const char *str)
{
  repo_set_poolstr(s->repo, s - s->repo->pool->solvables, keyname, str);
}

void
solvable_add_poolstr_array(Solvable *s, Id keyname, const char *str)
{
  repo_add_poolstr_array(s->repo, s - s->repo->pool->solvables, keyname, str);
}

void
solvable_add_idarray(Solvable *s, Id keyname, Id id)
{
  repo_add_idarray(s->repo, s - s->repo->pool->solvables, keyname, id);
}

void
solvable_add_deparray(Solvable *s, Id keyname, Id dep, Id marker)
{
  repo_add_deparray(s->repo, s - s->repo->pool->solvables, keyname, dep, marker);
}

void
solvable_set_idarray(Solvable *s, Id keyname, Queue *q)
{
  repo_set_idarray(s->repo, s - s->repo->pool->solvables, keyname, q);
}

void
solvable_set_deparray(Solvable *s, Id keyname, Queue *q, Id marker)
{
  repo_set_deparray(s->repo, s - s->repo->pool->solvables, keyname, q, marker);
}

void
solvable_unset(Solvable *s, Id keyname)
{
  repo_unset(s->repo, s - s->repo->pool->solvables, keyname);
}

/* return true if a dependency intersects dep in the keyname array */
int
solvable_matchesdep(Solvable *s, Id keyname, Id dep, int marker)
{
  int i;
  Pool *pool = s->repo->pool;
  Queue q;

  if (keyname == SOLVABLE_NAME)
    return pool_match_nevr(pool, s, dep) ? 1 : 0;	/* nevr match hack */
  queue_init(&q);
  solvable_lookup_deparray(s, keyname, &q, marker);
  for (i = 0; i < q.count; i++)
    if (pool_match_dep(pool, q.elements[i], dep))
      break;
  i = i == q.count ? 0 : 1;
  queue_free(&q);
  return i;
}

int
solvable_matchessolvable_int(Solvable *s, Id keyname, int marker, Id solvid, Map *solvidmap, Queue *depq, Map *missc, int reloff, Queue *outdepq)
{
  Pool *pool = s->repo->pool;
  int i, boff;
  Id *wp;

  if (depq->count)
    queue_empty(depq);
  if (outdepq && outdepq->count)
    queue_empty(outdepq);
  solvable_lookup_deparray(s, keyname, depq, marker);
  for (i = 0; i < depq->count; i++)
    {
      Id dep = depq->elements[i];
      boff = ISRELDEP(dep) ? reloff + GETRELID(dep) : dep;
      if (MAPTST(missc, boff))
	continue;
      if (ISRELDEP(dep))
	{
	  Reldep *rd = GETRELDEP(pool, dep);
	  if (!ISRELDEP(rd->name) && rd->flags < 8)
	    {
	      /* do pre-filtering on the base */
	      if (MAPTST(missc, rd->name))
		continue;
	      wp = pool_whatprovides_ptr(pool, rd->name);
	      if (solvidmap)
		{
		  for (; *wp; wp++)
		    if (MAPTST(solvidmap, *wp))
		      break;
		}
	      else
		{
		  for (; *wp; wp++)
		    if (*wp == solvid)
		      break;
		}
	      if (!*wp)
		{
		  /* the base does not include solvid, no need to check the complete dep */
		  MAPSET(missc, rd->name);
		  MAPSET(missc, boff);
		  continue;
		}
	    }
	}
      wp = pool_whatprovides_ptr(pool, dep);
      if (solvidmap)
	{
	  for (; *wp; wp++)
	    if (MAPTST(solvidmap, *wp))
	      break;
	}
      else
	{
	  for (; *wp; wp++)
	    if (*wp == solvid)
	      break;
	}
      if (*wp)
	{
	  if (outdepq)
	    {
	      queue_pushunique(outdepq, dep);
	      continue;
	    }
	  return 1;
	}
      MAPSET(missc, boff);
    }
  return outdepq && outdepq->count ? 1 : 0;
}

int
solvable_matchessolvable(Solvable *s, Id keyname, Id solvid, Queue *depq, int marker)
{
  Pool *pool = s->repo->pool;
  Map missc;		/* cache for misses */
  int res, reloff;
  Queue qq;

  if (depq && depq->count)
    queue_empty(depq);
  if (s - pool->solvables == solvid)
    return 0;		/* no self-matches */

  queue_init(&qq);
  reloff = pool->ss.nstrings;
  map_init(&missc, reloff + pool->nrels);
  res = solvable_matchessolvable_int(s, keyname, marker, solvid, 0, &qq, &missc, reloff, depq);
  map_free(&missc);
  queue_free(&qq);
  return res;
}

static int
solvidset2str_evrcmp(Pool *pool, Id a, Id b)
{
  Solvable *as = pool->solvables + a, *bs = pool->solvables + b;
  return as->evr != bs->evr ? pool_evrcmp(pool, as->evr, bs->evr, EVRCMP_COMPARE) : 0;
}

static int
solvidset2str_sortcmp(const void *va, const void *vb, void *vd)
{
  Pool *pool = vd;
  Solvable *as = pool->solvables + *(Id *)va, *bs = pool->solvables + *(Id *)vb;
  if (as->name != bs->name)
    {
      int r = strcmp(pool_id2str(pool, as->name), pool_id2str(pool, bs->name));
      if (r)
	return r;
      return as->name - bs->name;
    }
  if (as->evr != bs->evr)
    {
      int r = pool_evrcmp(pool, as->evr, bs->evr, EVRCMP_COMPARE);
      if (r)
        return r;
    }
  return *(Id *)va - *(Id *)vb;
}

static const char *
solvidset2str_striprelease(Pool *pool, Id evr, Id otherevr)
{
  const char *evrstr = pool_id2str(pool, evr);
  const char *r = strchr(evrstr, '-');
  char *evrstr2;
  int cmp;
  if (!r)
    return evrstr;
  evrstr2 = pool_tmpjoin(pool, evrstr, 0, 0);
  evrstr2[r - evrstr] = 0;
  cmp = pool_evrcmp_str(pool, evrstr2, pool_id2str(pool, otherevr), pool->disttype != DISTTYPE_DEB ? EVRCMP_MATCH_RELEASE : EVRCMP_COMPARE);
  return cmp == 1 ? evrstr2 : evrstr;
}

const char *
pool_solvidset2str(Pool *pool, Queue *q)
{
  Queue pq;
  Queue pr;
  char *s = 0;
  int i, j, k, kstart;
  Id name = 0;

  if (!q->count)
    return "";
  if (q->count == 1)
    return pool_solvid2str(pool, q->elements[0]);
  queue_init_clone(&pq, q);
  queue_init(&pr);
  solv_sort(pq.elements, pq.count, sizeof(Id), solvidset2str_sortcmp, pool);

  for (i = 0; i < pq.count; i++)
    {
      Id p = pq.elements[i];
      if (s)
	s = pool_tmpappend(pool, s, ", ", 0);

      if (i == 0 || pool->solvables[p].name != name)
	{
	  Id p2, pp2;
	  name = pool->solvables[p].name;
	  queue_empty(&pr);
	  FOR_PROVIDES(p2, pp2, name)
	    if (pool->solvables[p].name == name)
	      queue_push(&pr, p2);
	  if (pr.count > 1)
	    solv_sort(pr.elements, pr.count, sizeof(Id), solvidset2str_sortcmp, pool);
	}

      for (k = 0; k < pr.count; k++)
	if (pr.elements[k] == p)
	  break;
      if (k == pr.count)
	{
	  /* not in provides, list as singularity */
	  s = pool_tmpappend(pool, s, pool_solvid2str(pool, pq.elements[i]), 0);
	  continue;
	}
      if (k && solvidset2str_evrcmp(pool, pr.elements[k], pr.elements[k - 1]) == 0)
	{
	  /* unclear start, list as single package */
	  s = pool_tmpappend(pool, s, pool_solvid2str(pool, pq.elements[i]), 0);
	  continue;
	}
      kstart = k;
      for (j = i + 1, k = k + 1; j < pq.count; j++, k++)
        if (k == pr.count || pq.elements[j] != pr.elements[k])
	  break;
      while (j > i + 1 && k && k < pr.count && solvidset2str_evrcmp(pool, pr.elements[k], pr.elements[k - 1]) == 0)
	{
	  j--;
	  k--;
	}
      if (k == 0 || j == i + 1)
	{
	  s = pool_tmpappend(pool, s, pool_solvid2str(pool, pq.elements[i]), 0);
	  continue;
	}
      /* create an interval */
      s = pool_tmpappend(pool, s, pool_id2str(pool, name), 0);
      if (kstart > 0)
        s = pool_tmpappend(pool, s, " >= ", solvidset2str_striprelease(pool, pool->solvables[pr.elements[kstart]].evr, pool->solvables[pr.elements[kstart - 1]].evr));
      if (k < pr.count)
        s = pool_tmpappend(pool, s, " < ", solvidset2str_striprelease(pool, pool->solvables[pr.elements[k]].evr, pool->solvables[pr.elements[k - 1]].evr));
      i = j - 1;
    }
  queue_free(&pq);
  queue_free(&pr);
  return s;
}

