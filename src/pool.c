/*
 * Copyright (c) 2007-2009, Novell Inc.
 *
 * This program is licensed under the BSD license, read LICENSE.BSD
 * for further information
 */

/*
 * pool.c
 *
 * The pool contains information about solvables
 * stored optimized for memory consumption and fast retrieval.
 */

#include <stdio.h>
#include <stdlib.h>
#include <stdarg.h>
#include <unistd.h>
#include <string.h>

#include "pool.h"
#include "poolvendor.h"
#include "repo.h"
#include "poolid.h"
#include "poolarch.h"
#include "util.h"
#include "bitmap.h"
#include "evr.h"

#define SOLVABLE_BLOCK	255

#undef LIBSOLV_KNOWNID_H
#define KNOWNID_INITIALIZE
#include "knownid.h"
#undef KNOWNID_INITIALIZE

/* create pool */
Pool *
pool_create(void)
{
  Pool *pool;
  Solvable *s;

  pool = (Pool *)solv_calloc(1, sizeof(*pool));

  stringpool_init(&pool->ss, initpool_data);
  pool_init_rels(pool);

  /* alloc space for Solvable 0 and system solvable */
  pool->solvables = solv_extend_resize(0, 2, sizeof(Solvable), SOLVABLE_BLOCK);
  pool->nsolvables = 2;
  memset(pool->solvables, 0, 2 * sizeof(Solvable));

  queue_init(&pool->vendormap);
  queue_init(&pool->pooljobs);
  queue_init(&pool->lazywhatprovidesq);

#if defined(DEBIAN)
  pool->disttype = DISTTYPE_DEB;
  pool->noarchid = ARCH_ALL;
#elif defined(ARCHLINUX)
  pool->disttype = DISTTYPE_ARCH;
  pool->noarchid = ARCH_ANY;
#elif defined(HAIKU)
  pool->disttype = DISTTYPE_HAIKU;
  pool->noarchid = ARCH_ANY;
  pool->obsoleteusesprovides = 1;
#else
  pool->disttype = DISTTYPE_RPM;
  pool->noarchid = ARCH_NOARCH;
#endif

  /* initialize the system solvable */
  s = pool->solvables + SYSTEMSOLVABLE;
  s->name = SYSTEM_SYSTEM;
  s->arch = pool->noarchid;
  s->evr = ID_EMPTY;

  pool->debugmask = SOLV_DEBUG_RESULT;	/* FIXME */
#if defined(FEDORA) || defined(MAGEIA)
  pool->implicitobsoleteusescolors = 1;
#endif
#ifdef RPM5
  pool->noobsoletesmultiversion = 1;
  pool->forbidselfconflicts = 1;
  pool->obsoleteusesprovides = 1;
  pool->implicitobsoleteusesprovides = 1;
  pool->havedistepoch = 1;
#endif
  return pool;
}


/* free all the resources of our pool */
void
pool_free(Pool *pool)
{
  int i;

  pool_freewhatprovides(pool);
  pool_freeidhashes(pool);
  pool_freeallrepos(pool, 1);
  solv_free(pool->id2arch);
  solv_free(pool->id2color);
  solv_free(pool->solvables);
  stringpool_free(&pool->ss);
  solv_free(pool->rels);
  pool_setvendorclasses(pool, 0);
  queue_free(&pool->vendormap);
  queue_free(&pool->pooljobs);
  queue_free(&pool->lazywhatprovidesq);
  for (i = 0; i < POOL_TMPSPACEBUF; i++)
    solv_free(pool->tmpspace.buf[i]);
  for (i = 0; i < pool->nlanguages; i++)
    free((char *)pool->languages[i]);
  solv_free((void *)pool->languages);
  solv_free(pool->languagecache);
  solv_free(pool->errstr);
  solv_free(pool->rootdir);
  solv_free(pool->nonstd_ids);
  solv_free(pool);
}

void
pool_freeallrepos(Pool *pool, int reuseids)
{
  int i;

  pool_freewhatprovides(pool);
  for (i = 1; i < pool->nrepos; i++)
    if (pool->repos[i])
      repo_freedata(pool->repos[i]);
  pool->repos = solv_free(pool->repos);
  pool->nrepos = 0;
  pool->urepos = 0;
  /* the first two solvables don't belong to a repo */
  pool_free_solvable_block(pool, 2, pool->nsolvables - 2, reuseids);
}

int
pool_setdisttype(Pool *pool, int disttype)
{
#ifdef MULTI_SEMANTICS
  int olddisttype = pool->disttype;
  switch(disttype)
    {
    case DISTTYPE_RPM:
    case DISTTYPE_APK:
      pool->noarchid = ARCH_NOARCH;
      break;
    case DISTTYPE_DEB:
      pool->noarchid = ARCH_ALL;
      break;
    case DISTTYPE_ARCH:
    case DISTTYPE_HAIKU:
      pool->noarchid = ARCH_ANY;
      break;
    case DISTTYPE_CONDA:
      pool->noarchid = ARCH_ANY;
      break;
    default:
      return -1;
    }
  pool->disttype = disttype;
  pool->solvables[SYSTEMSOLVABLE].arch = pool->noarchid;
  return olddisttype;
#else
  return pool->disttype == disttype ? disttype : -1;
#endif
}

int
pool_get_flag(Pool *pool, int flag)
{
  switch (flag)
    {
    case POOL_FLAG_PROMOTEEPOCH:
      return pool->promoteepoch;
    case POOL_FLAG_FORBIDSELFCONFLICTS:
      return pool->forbidselfconflicts;
    case POOL_FLAG_OBSOLETEUSESPROVIDES:
      return pool->obsoleteusesprovides;
    case POOL_FLAG_IMPLICITOBSOLETEUSESPROVIDES:
      return pool->implicitobsoleteusesprovides;
    case POOL_FLAG_OBSOLETEUSESCOLORS:
      return pool->obsoleteusescolors;
    case POOL_FLAG_IMPLICITOBSOLETEUSESCOLORS:
      return pool->implicitobsoleteusescolors;
    case POOL_FLAG_NOINSTALLEDOBSOLETES:
      return pool->noinstalledobsoletes;
    case POOL_FLAG_HAVEDISTEPOCH:
      return pool->havedistepoch;
    case POOL_FLAG_NOOBSOLETESMULTIVERSION:
      return pool->noobsoletesmultiversion;
    case POOL_FLAG_ADDFILEPROVIDESFILTERED:
      return pool->addfileprovidesfiltered;
    case POOL_FLAG_NOWHATPROVIDESAUX:
      return pool->nowhatprovidesaux;
    case POOL_FLAG_WHATPROVIDESWITHDISABLED:
      return pool->whatprovideswithdisabled;
    default:
      break;
    }
  return -1;
}

int
pool_set_flag(Pool *pool, int flag, int value)
{
  int old = pool_get_flag(pool, flag);
  switch (flag)
    {
    case POOL_FLAG_PROMOTEEPOCH:
      pool->promoteepoch = value;
      break;
    case POOL_FLAG_FORBIDSELFCONFLICTS:
      pool->forbidselfconflicts = value;
      break;
    case POOL_FLAG_OBSOLETEUSESPROVIDES:
      pool->obsoleteusesprovides = value;
      break;
    case POOL_FLAG_IMPLICITOBSOLETEUSESPROVIDES:
      pool->implicitobsoleteusesprovides = value;
      break;
    case POOL_FLAG_OBSOLETEUSESCOLORS:
      pool->obsoleteusescolors = value;
      break;
    case POOL_FLAG_IMPLICITOBSOLETEUSESCOLORS:
      pool->implicitobsoleteusescolors = value;
      break;
    case POOL_FLAG_NOINSTALLEDOBSOLETES:
      pool->noinstalledobsoletes = value;
      break;
    case POOL_FLAG_HAVEDISTEPOCH:
      pool->havedistepoch = value;
      break;
    case POOL_FLAG_NOOBSOLETESMULTIVERSION:
      pool->noobsoletesmultiversion = value;
      break;
    case POOL_FLAG_ADDFILEPROVIDESFILTERED:
      pool->addfileprovidesfiltered = value;
      break;
    case POOL_FLAG_NOWHATPROVIDESAUX:
      pool->nowhatprovidesaux = value;
      break;
    case POOL_FLAG_WHATPROVIDESWITHDISABLED:
      pool->whatprovideswithdisabled = value;
      break;
    default:
      break;
    }
  return old;
}


Id
pool_add_solvable(Pool *pool)
{
  pool->solvables = solv_extend(pool->solvables, pool->nsolvables, 1, sizeof(Solvable), SOLVABLE_BLOCK);
  memset(pool->solvables + pool->nsolvables, 0, sizeof(Solvable));
  return pool->nsolvables++;
}

Id
pool_add_solvable_block(Pool *pool, int count)
{
  Id nsolvables = pool->nsolvables;
  if (!count)
    return nsolvables;
  pool->solvables = solv_extend(pool->solvables, pool->nsolvables, count, sizeof(Solvable), SOLVABLE_BLOCK);
  memset(pool->solvables + nsolvables, 0, sizeof(Solvable) * count);
  pool->nsolvables += count;
  return nsolvables;
}

void
pool_free_solvable_block(Pool *pool, Id start, int count, int reuseids)
{
  if (!count)
    return;
  if (reuseids && start + count == pool->nsolvables)
    {
      /* might want to shrink solvable array */
      pool->nsolvables = start;
      return;
    }
  memset(pool->solvables + start, 0, sizeof(Solvable) * count);
}

void
pool_set_installed(Pool *pool, Repo *installed)
{
  if (pool->installed == installed)
    return;
  pool->installed = installed;
  pool_freewhatprovides(pool);
}

void
pool_debug(Pool *pool, int type, const char *format, ...)
{
  va_list args;
  char buf[1024];

  if ((type & (SOLV_FATAL|SOLV_ERROR)) == 0)
    {
      if ((pool->debugmask & type) == 0)
	return;
    }
  va_start(args, format);
  if (!pool->debugcallback)
    {
      if ((type & (SOLV_FATAL|SOLV_ERROR)) == 0 && !(pool->debugmask & SOLV_DEBUG_TO_STDERR))
        vprintf(format, args);
      else
        vfprintf(stderr, format, args);
      va_end(args);
      return;
    }
  vsnprintf(buf, sizeof(buf), format, args);
  va_end(args);
  pool->debugcallback(pool, pool->debugcallbackdata, type, buf);
}

int
pool_error(Pool *pool, int ret, const char *format, ...)
{
  va_list args;
  int l;

  if (!pool)
    return ret;
  va_start(args, format);
  if (!pool->errstr)
    {
      pool->errstra = 1024;
      pool->errstr = solv_malloc(pool->errstra);
    }
  if (!*format)
    {
      *pool->errstr = 0;
      l = 0;
    }
  else
    l = vsnprintf(pool->errstr, pool->errstra, format, args);
  va_end(args);
  if (l >= 0 && l + 1 > pool->errstra)
    {
      pool->errstra = l + 256;
      pool->errstr = solv_realloc(pool->errstr, pool->errstra);
      va_start(args, format);
      l = vsnprintf(pool->errstr, pool->errstra, format, args);
      va_end(args);
    }
  if (l < 0)
    strcpy(pool->errstr, "unknown error");
  if (pool->debugmask & SOLV_ERROR)
    pool_debug(pool, SOLV_ERROR, "%s\n", pool->errstr);
  return ret;
}

char *
pool_errstr(Pool *pool)
{
  return pool->errstr ? pool->errstr : "no error";
}

void
pool_setdebuglevel(Pool *pool, int level)
{
  int mask = SOLV_DEBUG_RESULT;
  if (level > 0)
    mask |= SOLV_DEBUG_STATS|SOLV_DEBUG_ANALYZE|SOLV_DEBUG_UNSOLVABLE|SOLV_DEBUG_SOLVER|SOLV_DEBUG_TRANSACTION|SOLV_ERROR;
  if (level > 1)
    mask |= SOLV_DEBUG_JOB|SOLV_DEBUG_SOLUTIONS|SOLV_DEBUG_POLICY;
  if (level > 2)
    mask |= SOLV_DEBUG_PROPAGATE;
  if (level > 3)
    mask |= SOLV_DEBUG_RULE_CREATION | SOLV_DEBUG_WATCHES;
  mask |= pool->debugmask & SOLV_DEBUG_TO_STDERR;	/* keep bit */
  pool->debugmask = mask;
}

void pool_setdebugcallback(Pool *pool, void (*debugcallback)(struct s_Pool *, void *data, int type, const char *str), void *debugcallbackdata)
{
  pool->debugcallback = debugcallback;
  pool->debugcallbackdata = debugcallbackdata;
}

void pool_setdebugmask(Pool *pool, int mask)
{
  pool->debugmask = mask;
}

void pool_setloadcallback(Pool *pool, int (*cb)(struct s_Pool *, struct s_Repodata *, void *), void *loadcbdata)
{
  pool->loadcallback = cb;
  pool->loadcallbackdata = loadcbdata;
}

void pool_setnamespacecallback(Pool *pool, Id (*cb)(struct s_Pool *, void *, Id, Id), void *nscbdata)
{
  pool->nscallback = cb;
  pool->nscallbackdata = nscbdata;
}

void
pool_search(Pool *pool, Id p, Id key, const char *match, int flags, int (*callback)(void *cbdata, Solvable *s, struct s_Repodata *data, struct s_Repokey *key, struct s_KeyValue *kv), void *cbdata)
{
  if (p)
    {
      if (pool->solvables[p].repo)
        repo_search(pool->solvables[p].repo, p, key, match, flags, callback, cbdata);
      return;
    }
  /* FIXME: obey callback return value! */
  for (p = 1; p < pool->nsolvables; p++)
    if (pool->solvables[p].repo)
      repo_search(pool->solvables[p].repo, p, key, match, flags, callback, cbdata);
}

void
pool_clear_pos(Pool *pool)
{
  memset(&pool->pos, 0, sizeof(pool->pos));
}

void
pool_set_languages(Pool *pool, const char **languages, int nlanguages)
{
  int i;

  pool->languagecache = solv_free(pool->languagecache);
  pool->languagecacheother = 0;
  for (i = 0; i < pool->nlanguages; i++)
    free((char *)pool->languages[i]);
  pool->languages = solv_free((void *)pool->languages);
  pool->nlanguages = nlanguages;
  if (!nlanguages)
    return;
  pool->languages = solv_calloc(nlanguages, sizeof(const char **));
  for (i = 0; i < pool->nlanguages; i++)
    pool->languages[i] = solv_strdup(languages[i]);
}

Id
pool_id2langid(Pool *pool, Id id, const char *lang, int create)
{
  const char *n;
  char buf[256], *p;
  int l;

  if (!lang || !*lang)
    return id;
  n = pool_id2str(pool, id);
  l = strlen(n) + strlen(lang) + 2;
  if (l > sizeof(buf))
    p = solv_malloc(strlen(n) + strlen(lang) + 2);
  else
    p = buf;
  sprintf(p, "%s:%s", n, lang);
  id = pool_str2id(pool, p, create);
  if (p != buf)
    free(p);
  return id;
}

char *
pool_alloctmpspace(Pool *pool, int len)
{
  int n = pool->tmpspace.n;
  if (!len)
    return 0;
  if (len > pool->tmpspace.len[n])
    {
      pool->tmpspace.buf[n] = solv_realloc(pool->tmpspace.buf[n], len + 32);
      pool->tmpspace.len[n] = len + 32;
    }
  pool->tmpspace.n = (n + 1) % POOL_TMPSPACEBUF;
  return pool->tmpspace.buf[n];
}

static char *
pool_alloctmpspace_free(Pool *pool, const char *space, int len)
{
  if (space)
    {
      int n, oldn;
      n = oldn = pool->tmpspace.n;
      for (;;)
	{
	  if (!n--)
	    n = POOL_TMPSPACEBUF - 1;
	  if (n == oldn)
	    break;
	  if (pool->tmpspace.buf[n] != space)
	    continue;
	  if (len > pool->tmpspace.len[n])
	    {
	      pool->tmpspace.buf[n] = solv_realloc(pool->tmpspace.buf[n], len + 32);
	      pool->tmpspace.len[n] = len + 32;
	    }
          return pool->tmpspace.buf[n];
	}
    }
  return 0;
}

void
pool_freetmpspace(Pool *pool, const char *space)
{
  int n = pool->tmpspace.n;
  if (!space)
    return;
  n = (n + (POOL_TMPSPACEBUF - 1)) % POOL_TMPSPACEBUF;
  if (pool->tmpspace.buf[n] == space)
    pool->tmpspace.n = n;
}

char *
pool_tmpjoin(Pool *pool, const char *str1, const char *str2, const char *str3)
{
  int l1, l2, l3;
  char *s, *str;
  l1 = str1 ? strlen(str1) : 0;
  l2 = str2 ? strlen(str2) : 0;
  l3 = str3 ? strlen(str3) : 0;
  s = str = pool_alloctmpspace(pool, l1 + l2 + l3 + 1);
  if (l1)
    {
      strcpy(s, str1);
      s += l1;
    }
  if (l2)
    {
      strcpy(s, str2);
      s += l2;
    }
  if (l3)
    {
      strcpy(s, str3);
      s += l3;
    }
  *s = 0;
  return str;
}

char *
pool_tmpappend(Pool *pool, const char *str1, const char *str2, const char *str3)
{
  int l1, l2, l3;
  char *s, *str;

  l1 = str1 ? strlen(str1) : 0;
  l2 = str2 ? strlen(str2) : 0;
  l3 = str3 ? strlen(str3) : 0;
  str = pool_alloctmpspace_free(pool, str1, l1 + l2 + l3 + 1);
  if (str)
    str1 = str;
  else
    str = pool_alloctmpspace(pool, l1 + l2 + l3 + 1);
  s = str;
  if (l1)
    {
      if (s != str1)
        strcpy(s, str1);
      s += l1;
    }
  if (l2)
    {
      strcpy(s, str2);
      s += l2;
    }
  if (l3)
    {
      strcpy(s, str3);
      s += l3;
    }
  *s = 0;
  return str;
}

const char *
pool_bin2hex(Pool *pool, const unsigned char *buf, int len)
{
  char *s;
  if (!len)
    return "";
  s = pool_alloctmpspace(pool, 2 * len + 1);
  solv_bin2hex(buf, len, s);
  return s;
}

const char *
pool_lookup_str(Pool *pool, Id entry, Id keyname)
{
  if (entry == SOLVID_POS && pool->pos.repo)
    return repo_lookup_str(pool->pos.repo, pool->pos.repodataid ? entry : pool->pos.solvid, keyname);
  if (entry <= 0)
    return 0;
  return solvable_lookup_str(pool->solvables + entry, keyname);
}

Id
pool_lookup_id(Pool *pool, Id entry, Id keyname)
{
  if (entry == SOLVID_POS && pool->pos.repo)
    return repo_lookup_id(pool->pos.repo, pool->pos.repodataid ? entry : pool->pos.solvid, keyname);
  if (entry <= 0)
    return 0;
  return solvable_lookup_id(pool->solvables + entry, keyname);
}

unsigned long long
pool_lookup_num(Pool *pool, Id entry, Id keyname, unsigned long long notfound)
{
  if (entry == SOLVID_POS && pool->pos.repo)
    return repo_lookup_num(pool->pos.repo, pool->pos.repodataid ? entry : pool->pos.solvid, keyname, notfound);
  if (entry <= 0)
    return notfound;
  return solvable_lookup_num(pool->solvables + entry, keyname, notfound);
}

int
pool_lookup_void(Pool *pool, Id entry, Id keyname)
{
  if (entry == SOLVID_POS && pool->pos.repo)
    return repo_lookup_void(pool->pos.repo, pool->pos.repodataid ? entry : pool->pos.solvid, keyname);
  if (entry <= 0)
    return 0;
  return solvable_lookup_void(pool->solvables + entry, keyname);
}

const unsigned char *
pool_lookup_bin_checksum(Pool *pool, Id entry, Id keyname, Id *typep)
{
  if (entry == SOLVID_POS && pool->pos.repo)
    return repo_lookup_bin_checksum(pool->pos.repo, pool->pos.repodataid ? entry : pool->pos.solvid, keyname, typep);
  if (entry <= 0)
    return 0;
  return solvable_lookup_bin_checksum(pool->solvables + entry, keyname, typep);
}

const char *
pool_lookup_checksum(Pool *pool, Id entry, Id keyname, Id *typep)
{
  if (entry == SOLVID_POS && pool->pos.repo)
    return repo_lookup_checksum(pool->pos.repo, pool->pos.repodataid ? entry : pool->pos.solvid, keyname, typep);
  if (entry <= 0)
    return 0;
  return solvable_lookup_checksum(pool->solvables + entry, keyname, typep);
}

int
pool_lookup_idarray(Pool *pool, Id entry, Id keyname, Queue *q)
{
  if (entry == SOLVID_POS && pool->pos.repo)
    return repo_lookup_idarray(pool->pos.repo, pool->pos.repodataid ? entry : pool->pos.solvid, keyname, q);
  if (entry <= 0)
    return 0;
  return solvable_lookup_idarray(pool->solvables + entry, keyname, q);
}

const char *
pool_lookup_deltalocation(Pool *pool, Id entry, unsigned int *medianrp)
{
  const char *loc;
  if (medianrp)
    *medianrp = 0;
  if (entry != SOLVID_POS)
    return 0;
  loc = pool_lookup_str(pool, entry, DELTA_LOCATION_DIR);
  loc = pool_tmpjoin(pool, loc, loc ? "/" : 0, pool_lookup_str(pool, entry, DELTA_LOCATION_NAME));
  loc = pool_tmpappend(pool, loc, "-", pool_lookup_str(pool, entry, DELTA_LOCATION_EVR));
  loc = pool_tmpappend(pool, loc, ".", pool_lookup_str(pool, entry, DELTA_LOCATION_SUFFIX));
  return loc;
}

void
pool_add_fileconflicts_deps(Pool *pool, Queue *conflicts)
{
  int hadhashes = pool->relhashtbl ? 1 : 0;
  Solvable *s;
  Id fn, p, q, md5;
  Id id;
  int i;

  if (!conflicts->count)
    return;
  for (i = 0; i < conflicts->count; i += 6)
    {
      fn = conflicts->elements[i];
      p = conflicts->elements[i + 1];
      md5 = conflicts->elements[i + 2];
      q = conflicts->elements[i + 4];
      id = pool_rel2id(pool, fn, md5, REL_FILECONFLICT, 1);
      s = pool->solvables + p;
      if (!s->repo)
	continue;
      s->provides = repo_addid_dep(s->repo, s->provides, id, SOLVABLE_FILEMARKER);
      if (pool->whatprovides)
	pool_add_new_provider(pool, id, p);
      s = pool->solvables + q;
      if (!s->repo)
	continue;
      s->conflicts = repo_addid_dep(s->repo, s->conflicts, id, 0);
    }
  if (!hadhashes)
    pool_freeidhashes(pool);
}

char *
pool_prepend_rootdir(Pool *pool, const char *path)
{
  if (!path)
    return 0;
  if (!pool->rootdir)
    return solv_strdup(path);
  return solv_dupjoin(pool->rootdir, "/", *path == '/' ? path + 1 : path);
}

const char *
pool_prepend_rootdir_tmp(Pool *pool, const char *path)
{
  if (!path)
    return 0;
  if (!pool->rootdir)
    return path;
  return pool_tmpjoin(pool, pool->rootdir, "/", *path == '/' ? path + 1 : path);
}

void
pool_set_rootdir(Pool *pool, const char *rootdir)
{
  solv_free(pool->rootdir);
  pool->rootdir = solv_strdup(rootdir);
}

const char *
pool_get_rootdir(Pool *pool)
{
  return pool->rootdir;
}

/* only used in libzypp */
void
pool_set_custom_vendorcheck(Pool *pool, int (*vendorcheck)(Pool *, Solvable *, Solvable *))
{
  pool->custom_vendorcheck = vendorcheck;
}

int (*pool_get_custom_vendorcheck(Pool *pool))(Pool *, Solvable *, Solvable *)
{
  return pool->custom_vendorcheck;
}

/* EOF */
