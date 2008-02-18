/*
 * Copyright (c) 2007, Novell Inc.
 *
 * This program is licensed under the BSD license, read LICENSE.BSD
 * for further information
 */

/*
 * repo_rpmdb
 * 
 * convert rpm db to repo
 * 
 */

#include <sys/types.h>
#include <sys/stat.h>
#include <limits.h>
#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include <db43/db.h>

#include "pool.h"
#include "repo.h"
#include "hash.h"
#include "util.h"
#include "repo_rpmdb.h"

#include "tools_util.h"


#define TAG_NAME		1000
#define TAG_VERSION		1001
#define TAG_RELEASE		1002
#define TAG_EPOCH		1003
#define TAG_SUMMARY		1004
#define TAG_DESCRIPTION		1005
#define TAG_BUILDTIME		1006
#define TAG_VENDOR		1011
#define TAG_GROUP		1016
#define TAG_ARCH		1022
#define TAG_FILESIZES		1028
#define TAG_FILEMODES		1030
#define TAG_PROVIDENAME		1047
#define TAG_REQUIREFLAGS	1048
#define TAG_REQUIRENAME		1049
#define TAG_REQUIREVERSION	1050
#define TAG_CONFLICTFLAGS	1053
#define TAG_CONFLICTNAME	1054
#define TAG_CONFLICTVERSION	1055
#define TAG_OBSOLETENAME	1090
#define TAG_PROVIDEFLAGS	1112
#define TAG_PROVIDEVERSION	1113
#define TAG_OBSOLETEFLAGS	1114
#define TAG_OBSOLETEVERSION	1115
#define TAG_DIRINDEXES		1116
#define TAG_BASENAMES		1117
#define TAG_DIRNAMES		1118
#define TAG_SUGGESTSNAME	1156
#define TAG_SUGGESTSVERSION	1157
#define TAG_SUGGESTSFLAGS	1158
#define TAG_ENHANCESNAME	1159
#define TAG_ENHANCESVERSION	1160
#define TAG_ENHANCESFLAGS	1161

#define DEP_LESS		(1 << 1)
#define DEP_GREATER		(1 << 2)
#define DEP_EQUAL		(1 << 3)
#define DEP_STRONG		(1 << 27)
#define DEP_PRE			((1 << 6) | (1 << 9) | (1 << 10) | (1 << 11) | (1 << 12))


static DBT key;
static DBT data;

struct rpmid {
  unsigned int dbid;
  char *name;
};

typedef struct rpmhead {
  int cnt;
  int dcnt;
  unsigned char *dp;
  unsigned char data[1];
} RpmHead;

static unsigned int *
headint32array(RpmHead *h, int tag, int *cnt)
{
  unsigned int i, o, *r;
  unsigned char *d, taga[4];

  d = h->dp - 16;
  taga[0] = tag >> 24;
  taga[1] = tag >> 16;
  taga[2] = tag >> 8;
  taga[3] = tag;
  for (i = 0; i < h->cnt; i++, d -= 16)
    if (d[3] == taga[3] && d[2] == taga[2] && d[1] == taga[1] && d[0] == taga[0])
      break;
  if (i >= h->cnt)
    return 0;
  if (d[4] != 0 || d[5] != 0 || d[6] != 0 || d[7] != 4)
    return 0;
  o = d[8] << 24 | d[9] << 16 | d[10] << 8 | d[11];
  i = d[12] << 24 | d[13] << 16 | d[14] << 8 | d[15];
  if (o + 4 * i > h->dcnt)
    return 0;
  d = h->dp + o;
  r = sat_calloc(i ? i : 1, sizeof(unsigned int));
  if (cnt)
    *cnt = i;
  for (o = 0; o < i; o++, d += 4)
    r[o] = d[0] << 24 | d[1] << 16 | d[2] << 8 | d[3];
  return r;
}

static unsigned int
headint32(RpmHead *h, int tag)
{
  unsigned int i, o;
  unsigned char *d, taga[4];

  d = h->dp - 16; 
  taga[0] = tag >> 24; 
  taga[1] = tag >> 16; 
  taga[2] = tag >> 8;
  taga[3] = tag;
  for (i = 0; i < h->cnt; i++, d -= 16) 
    if (d[3] == taga[3] && d[2] == taga[2] && d[1] == taga[1] && d[0] == taga[0])
      break;
  if (i >= h->cnt)
    return 0;
  if (d[4] != 0 || d[5] != 0 || d[6] != 0 || d[7] != 4)
    return 0;
  o = d[8] << 24 | d[9] << 16 | d[10] << 8 | d[11];
  i = d[12] << 24 | d[13] << 16 | d[14] << 8 | d[15];
  if (i == 0 || o + 4 * i > h->dcnt)
    return 0;
  d = h->dp + o;
  return d[0] << 24 | d[1] << 16 | d[2] << 8 | d[3];
}

static unsigned int *
headint16array(RpmHead *h, int tag, int *cnt)
{
  unsigned int i, o, *r;
  unsigned char *d, taga[4];

  d = h->dp - 16;
  taga[0] = tag >> 24;
  taga[1] = tag >> 16;
  taga[2] = tag >> 8;
  taga[3] = tag;
  for (i = 0; i < h->cnt; i++, d -= 16)
    if (d[3] == taga[3] && d[2] == taga[2] && d[1] == taga[1] && d[0] == taga[0])
      break;
  if (i >= h->cnt)
    return 0;
  if (d[4] != 0 || d[5] != 0 || d[6] != 0 || d[7] != 3)
    return 0;
  o = d[8] << 24 | d[9] << 16 | d[10] << 8 | d[11];
  i = d[12] << 24 | d[13] << 16 | d[14] << 8 | d[15];
  if (o + 4 * i > h->dcnt)
    return 0;
  d = h->dp + o;
  r = sat_calloc(i ? i : 1, sizeof(unsigned int));
  if (cnt)
    *cnt = i;
  for (o = 0; o < i; o++, d += 2)
    r[o] = d[0] << 8 | d[1];
  return r;
}

static char *
headstring(RpmHead *h, int tag)
{
  unsigned int i, o;
  unsigned char *d, taga[4];
  d = h->dp - 16;
  taga[0] = tag >> 24;
  taga[1] = tag >> 16;
  taga[2] = tag >> 8;
  taga[3] = tag;
  for (i = 0; i < h->cnt; i++, d -= 16)
    if (d[3] == taga[3] && d[2] == taga[2] && d[1] == taga[1] && d[0] == taga[0])
      break;
  if (i >= h->cnt)
    return 0;
  /* 6: STRING, 9: I18NSTRING */
  if (d[4] != 0 || d[5] != 0 || d[6] != 0 || (d[7] != 6 && d[7] != 9))
    return 0;
  o = d[8] << 24 | d[9] << 16 | d[10] << 8 | d[11];
  return (char *)h->dp + o;
}

static char **
headstringarray(RpmHead *h, int tag, int *cnt)
{
  unsigned int i, o;
  unsigned char *d, taga[4];
  char **r;

  d = h->dp - 16;
  taga[0] = tag >> 24;
  taga[1] = tag >> 16;
  taga[2] = tag >> 8;
  taga[3] = tag;
  for (i = 0; i < h->cnt; i++, d -= 16)
    if (d[3] == taga[3] && d[2] == taga[2] && d[1] == taga[1] && d[0] == taga[0])
      break;
  if (i >= h->cnt)
    return 0;
  if (d[4] != 0 || d[5] != 0 || d[6] != 0 || d[7] != 8)
    return 0;
  o = d[8] << 24 | d[9] << 16 | d[10] << 8 | d[11];
  i = d[12] << 24 | d[13] << 16 | d[14] << 8 | d[15];
  r = sat_calloc(i ? i : 1, sizeof(char *));
  if (cnt)
    *cnt = i;
  d = h->dp + o;
  for (o = 0; o < i; o++)
    {
      r[o] = (char *)d;
      if (o + 1 < i)
        d += strlen((char *)d) + 1;
      if (d >= h->dp + h->dcnt)
        {
          sat_free(r);
          return 0;
        }
    }
  return r;
}

static char *headtoevr(RpmHead *h)
{
  unsigned int epoch;
  char *version, *v;
  char *release;
  char *evr;

  version  = headstring(h, TAG_VERSION);
  release  = headstring(h, TAG_RELEASE);
  epoch = headint32(h, TAG_EPOCH);
  if (!version || !release)
    {
      fprintf(stderr, "headtoevr: bad rpm header\n");
      exit(1);
    }
  for (v = version; *v >= 0 && *v <= '9'; v++)
    ;
  if (epoch || (v != version && *v == ':'))
    {
      char epochbuf[11];        /* 32bit decimal will fit in */
      sprintf(epochbuf, "%u", epoch);
      evr = sat_malloc(strlen(epochbuf) + 1 + strlen(version) + 1 + strlen(release) + 1);
      sprintf(evr, "%s:%s-%s", epochbuf, version, release);
    }
  else
    {
      evr = sat_malloc(strlen(version) + 1 + strlen(release) + 1);
      sprintf(evr, "%s-%s", version, release);
    }
  return evr;
}

static unsigned int
makedeps(Pool *pool, Repo *repo, RpmHead *rpmhead, int tagn, int tagv, int tagf, int strong)
{
  char **n, **v;
  unsigned int *f;
  int i, cc, nc, vc, fc;
  int haspre = 0;
  unsigned int olddeps;
  Id *ida;

  n = headstringarray(rpmhead, tagn, &nc);
  if (!n)
    return 0;
  v = headstringarray(rpmhead, tagv, &vc);
  if (!v)
    {
      sat_free(n);
      return 0;
    }
  f = headint32array(rpmhead, tagf, &fc);
  if (!f)
    {
      sat_free(n);
      free(v);
      return 0;
    }
  if (nc != vc || nc != fc)
    {
      fprintf(stderr, "bad dependency entries\n");
      exit(1);
    }

  cc = nc;
  if (strong)
    {
      cc = 0;
      for (i = 0; i < nc; i++)
	if ((f[i] & DEP_STRONG) == (strong == 1 ? 0 : DEP_STRONG))
	  {
	    cc++;
	    if ((f[i] & DEP_PRE) != 0)
	      haspre = 1;
	  }
    }
  else
    {
      for (i = 0; i < nc; i++)
	if ((f[i] & DEP_PRE) != 0)
	  {
	    haspre = 1;
	    break;
	  }
    }
  if (tagn != TAG_REQUIRENAME)
     haspre = 0;
  if (cc == 0)
    {
      sat_free(n);
      sat_free(v);
      sat_free(f);
      return 0;
    }
  cc += haspre;
  olddeps = repo_reserve_ids(repo, 0, cc);
  ida = repo->idarraydata + olddeps;
  for (i = 0; ; i++)
    {
      if (i == nc)
	{
	  if (haspre != 1)
	    break;
	  haspre = 2;
	  i = 0;
	  *ida++ = SOLVABLE_PREREQMARKER;
	}
      if (strong && (f[i] & DEP_STRONG) != (strong == 1 ? 0 : DEP_STRONG))
	continue;
      if (haspre == 1 && (f[i] & DEP_PRE) != 0)
	continue;
      if (haspre == 2 && (f[i] & DEP_PRE) == 0)
	continue;
      if (f[i] & (DEP_LESS|DEP_GREATER|DEP_EQUAL))
	{
	  Id name, evr;
	  int flags = 0;
	  if ((f[i] & DEP_LESS) != 0)
	    flags |= 4;
	  if ((f[i] & DEP_EQUAL) != 0)
	    flags |= 2;
	  if ((f[i] & DEP_GREATER) != 0)
	    flags |= 1;
	  name = str2id(pool, n[i], 1);
	  if (v[i][0] == '0' && v[i][1] == ':' && v[i][2])
	    evr = str2id(pool, v[i] + 2, 1);
	  else
	    evr = str2id(pool, v[i], 1);
	  *ida++ = rel2id(pool, name, evr, flags, 1);
	}
      else
        *ida++ = str2id(pool, n[i], 1);
    }
  *ida++ = 0;
  repo->idarraysize += cc + 1;
  sat_free(n);
  sat_free(v);
  sat_free(f);
  return olddeps;
}

static Offset
copydeps(Pool *pool, Repo *repo, Offset fromoff, Repo *fromrepo)
{
  int cc;
  Id id, *ida, *from;
  Offset ido;
  Pool *frompool = fromrepo->pool;

  if (!fromoff)
    return 0;
  from = fromrepo->idarraydata + fromoff;
  for (ida = from, cc = 0; *ida; ida++, cc++)
    ;
  if (cc == 0)
    return 0;
  ido = repo_reserve_ids(repo, 0, cc);
  ida = repo->idarraydata + ido;
  if (frompool && pool != frompool)
    {
      while (*from)
	{
	  id = *from++;
	  if (ISRELDEP(id))
	    {
	      Reldep *rd = GETRELDEP(frompool, id);
	      Id name = str2id(pool, id2str(frompool, rd->name), 1);
	      Id evr = str2id(pool, id2str(frompool, rd->evr), 1);
	      id = rel2id(pool, name, evr, rd->flags, 1);
	    }
	  else
	    id = str2id(pool, id2str(frompool, id), 1);
	  *ida++ = id;
	}
      *ida = 0;
    }
  else
    memcpy(ida, from, (cc + 1) * sizeof(Id));
  repo->idarraysize += cc + 1;
  return ido;
}


#define FILEFILTER_EXACT    0
#define FILEFILTER_STARTS   1
#define FILEFILTER_CONTAINS 2

struct filefilter {
  int dirmatch;
  char *dir;
  char *base;
};

static struct filefilter filefilters[] = {
  { FILEFILTER_CONTAINS, "/bin/", 0},
  { FILEFILTER_CONTAINS, "/sbin/", 0},
  { FILEFILTER_CONTAINS, "/lib/", 0},
  { FILEFILTER_CONTAINS, "/lib64/", 0},
  { FILEFILTER_CONTAINS, "/etc/", 0},
  { FILEFILTER_STARTS, "/usr/games/", 0},
  { FILEFILTER_EXACT, "/usr/share/dict/", "words"},
  { FILEFILTER_STARTS, "/usr/share/", "magic.mime"},
  { FILEFILTER_STARTS, "/opt/gnome/games/", 0},
};

static void
adddudata(Pool *pool, Repo *repo, Repodata *repodata, Solvable *s, RpmHead *rpmhead, char **dn, unsigned int *di, int fc, int dic)
{
  Id entry, did;
  int i, fszc;
  unsigned int *fkb, *fn, *fsz, *fm;

  fsz = headint32array(rpmhead, TAG_FILESIZES, &fszc);
  if (!fsz || fc != fszc)
    {
      sat_free(fsz);
      return;
    }
  /* stupid rpm recodrs sizes of directories, so we have to check the mode */
  fm = headint16array(rpmhead, TAG_FILEMODES, &fszc);
  if (!fm || fc != fszc)
    {
      sat_free(fsz);
      sat_free(fm);
      return;
    }
  fn = sat_calloc(dic, sizeof(unsigned int));
  fkb = sat_calloc(dic, sizeof(unsigned int));
  for (i = 0; i < fc; i++)
    {
      if (fsz[i] == 0 || !S_ISREG(fm[i]))
	continue;
      if (di[i] >= dic)
	continue;
      fn[di[i]]++;
      /* does not consider hard links. tough luck. */
      fkb[di[i]] += fsz[i] / 1024 + 1;
    }
  sat_free(fsz);
  sat_free(fm);
  /* commit */
  repodata_extend(repodata, s - pool->solvables);
  entry = (s - pool->solvables) - repodata->start;
  for (i = 0; i < fc; i++)
    {
      if (!fn[i])
	continue;
      did = repodata_str2dir(repodata, dn[i], 1);
      repodata_add_dirnumnum(repodata, entry, id_diskusage, did, fkb[i], fn[i]);
    }
  sat_free(fn);
  sat_free(fkb);
}

/* assumes last processed array is provides! */
static unsigned int
addfileprovides(Pool *pool, Repo *repo, Repodata *repodata, Solvable *s, RpmHead *rpmhead, unsigned int olddeps)
{
  char **bn;
  char **dn;
  unsigned int *di;
  int bnc, dnc, dic;
  int i, j;
  struct filefilter *ff;
  char *fn = 0;
  int fna = 0;

  if (!repodata)
    return olddeps;
  bn = headstringarray(rpmhead, TAG_BASENAMES, &bnc);
  if (!bn)
    return olddeps;
  dn = headstringarray(rpmhead, TAG_DIRNAMES, &dnc);
  if (!dn)
    {
      sat_free(bn);
      return olddeps;
    }
  di = headint32array(rpmhead, TAG_DIRINDEXES, &dic);
  if (!di)
    {
      sat_free(bn);
      sat_free(dn);
      return olddeps;
    }
  if (bnc != dic)
    {
      fprintf(stderr, "bad filelist\n");
      exit(1);
    }

  if (repodata)
    adddudata(pool, repo, repodata, s, rpmhead, dn, di, bnc, dic);

  for (i = 0; i < bnc; i++)
    {
      ff = filefilters;
      for (j = 0; j < sizeof(filefilters)/sizeof(*filefilters); j++, ff++)
	{
	  if (ff->dir)
	    {
	      switch (ff->dirmatch)
		{
		case FILEFILTER_STARTS:
		  if (strncmp(dn[di[i]], ff->dir, strlen(ff->dir)))
		    continue;
		  break;
		case FILEFILTER_CONTAINS:
		  if (!strstr(dn[di[i]], ff->dir))
		    continue;
		  break;
		case FILEFILTER_EXACT:
		default:
		  if (strcmp(dn[di[i]], ff->dir))
		    continue;
		  break;
		}
	    }
	  if (ff->base)
	    {
	      if (strcmp(bn[i], ff->base))
		continue;
	    }
	  break;
	}
      if (j == sizeof(filefilters)/sizeof(*filefilters))
	continue;
      j = strlen(bn[i]) + strlen(dn[di[i]]) + 1;
      if (j > fna)
	{
	  fna = j + 256;
	  fn = sat_realloc(fn, fna);
	}
      strcpy(fn, dn[di[i]]);
      strcat(fn, bn[i]);
#if 0
      olddeps = repo_addid_dep(repo, olddeps, str2id(pool, fn, 1), SOLVABLE_FILEMARKER);
#endif
      if (repodata)
	{
	  Id entry, did;
	  repodata_extend(repodata, s - pool->solvables);
	  entry = (s - pool->solvables) - repodata->start;
	  did = repodata_str2dir(repodata, dn[di[i]], 1);
	  repodata_add_dirstr(repodata, entry, id_filelist, did, bn[i]);
	}
    }
  if (fn)
    sat_free(fn);
  sat_free(bn);
  sat_free(dn);
  sat_free(di);
  return olddeps;
}

static int
rpm2solv(Pool *pool, Repo *repo, Repodata *repodata, Solvable *s, RpmHead *rpmhead)
{
  char *name;
  char *evr;

  name = headstring(rpmhead, TAG_NAME);
  if (!strcmp(name, "gpg-pubkey"))
    return 0;
  s->name = str2id(pool, name, 1);
  if (!s->name)
    {
      fprintf(stderr, "package has no name\n");
      exit(1);
    }
  s->arch = str2id(pool, headstring(rpmhead, TAG_ARCH), 1);
  if (!s->arch)
    s->arch = ARCH_NOARCH;
  evr = headtoevr(rpmhead);
  s->evr = str2id(pool, evr, 1);
  sat_free(evr);
  s->vendor = str2id(pool, headstring(rpmhead, TAG_VENDOR), 1);

  s->provides = makedeps(pool, repo, rpmhead, TAG_PROVIDENAME, TAG_PROVIDEVERSION, TAG_PROVIDEFLAGS, 0);
  s->provides = addfileprovides(pool, repo, repodata, s, rpmhead, s->provides);
  s->provides = repo_addid_dep(repo, s->provides, rel2id(pool, s->name, s->evr, REL_EQ, 1), 0);
  s->requires = makedeps(pool, repo, rpmhead, TAG_REQUIRENAME, TAG_REQUIREVERSION, TAG_REQUIREFLAGS, 0);
  s->conflicts = makedeps(pool, repo, rpmhead, TAG_CONFLICTNAME, TAG_CONFLICTVERSION, TAG_CONFLICTFLAGS, 0);
  s->obsoletes = makedeps(pool, repo, rpmhead, TAG_OBSOLETENAME, TAG_OBSOLETEVERSION, TAG_OBSOLETEFLAGS, 0);

  s->recommends = makedeps(pool, repo, rpmhead, TAG_SUGGESTSNAME, TAG_SUGGESTSVERSION, TAG_SUGGESTSFLAGS, 2);
  s->suggests = makedeps(pool, repo, rpmhead, TAG_SUGGESTSNAME, TAG_SUGGESTSVERSION, TAG_SUGGESTSFLAGS, 1);
  s->supplements = makedeps(pool, repo, rpmhead, TAG_ENHANCESNAME, TAG_ENHANCESVERSION, TAG_ENHANCESFLAGS, 2);
  s->enhances  = makedeps(pool, repo, rpmhead, TAG_ENHANCESNAME, TAG_ENHANCESVERSION, TAG_ENHANCESFLAGS, 1);
  s->freshens = 0;
  s->supplements = repo_fix_legacy(repo, s->provides, s->supplements);

  if (repodata)
    {
      Id entry;
      char *str;
      unsigned int u32;

      repodata_extend(repodata, s - pool->solvables);
      entry = (s - pool->solvables) - repodata->start;
      str = headstring(rpmhead, TAG_SUMMARY);
      if (str)
        repodata_set_str(repodata, entry, id_summary, str);
      str = headstring(rpmhead, TAG_DESCRIPTION);
      if (str)
	{
	  char *aut, *p;
	  for (aut = str; (aut = strchr(aut, '\n')) != 0; aut++)
	    if (!strncmp(aut, "\nAuthors:\n--------\n", 19))
	      break;
	  if (aut)
	    {
	      /* oh my, found SUSE special author section */
	      int l = aut - str;
	      str = strdup(str);
	      aut = str + l;
	      str[l] = 0;
	      while (l > 0 && str[l - 1] == '\n')
	        str[--l] = 0;
	      if (l)
                repodata_set_str(repodata, entry, id_description, str);
	      p = aut + 19;
	      aut = str;	/* copy over */
	      while (*p == ' ' || *p == '\n')
		p++;
	      while (*p)
		{
		  if (*p == '\n')
		    {
		      *aut++ = *p++;
		      while (*p == ' ')
			p++;
		      continue;
		    }
		  *aut++ = *p++;
		}
	      while (aut != str && aut[-1] == '\n')
		aut--;
	      *aut = 0;
	      if (*str)
	        repodata_set_str(repodata, entry, id_authors, str);
	      free(str);
	    }
	  else if (*str)
	    repodata_set_str(repodata, entry, id_description, str);
	}
      str = headstring(rpmhead, TAG_GROUP);
      if (str)
        repodata_set_poolstr(repodata, entry, id_group, str);
      u32 = headint32(rpmhead, TAG_BUILDTIME);
      if (u32)
        repodata_set_num(repodata, entry, id_time, u32);
    }
  return 1;
}


/*
 * read rpm db as repo
 * 
 */

void
repo_add_rpmdb(Repo *repo, Repo *ref, const char *rootdir)
{
  Pool *pool = repo->pool;
  unsigned char buf[16];
  DB *db = 0;
  DBC *dbc = 0;
  int byteswapped;
  unsigned int dbid;
  unsigned char *dp, *dbidp;
  int dl, nrpmids;
  struct rpmid *rpmids, *rp;
  int i;
  int rpmheadsize;
  RpmHead *rpmhead;
  Solvable *s;
  Id id, *refhash;
  unsigned int refmask, h;
  int asolv;
  Repodata *repodata;

  if (repo->start != repo->end)
    abort();		/* FIXME: rpmdbid */

  repodata = repo_add_repodata(repo);
  init_attr_ids(repo->pool);

  if (ref && !(ref->nsolvables && ref->rpmdbid))
    ref = 0;

  if (db_create(&db, 0, 0))
    {
      perror("db_create");
      exit(1);
    }

  if (!ref)
    {
      if (db->open(db, 0, "/var/lib/rpm/Packages", 0, DB_HASH, DB_RDONLY, 0664))
	{
	  perror("db->open /var/lib/rpm/Packages");
	  exit(1);
	}
      if (db->get_byteswapped(db, &byteswapped))
	{
	  perror("db->get_byteswapped");
	  exit(1);
	}
      if (db->cursor(db, NULL, &dbc, 0))
	{
	  perror("db->cursor");
	  exit(1);
	}
      dbidp = (unsigned char *)&dbid;
      repo->rpmdbid = sat_calloc(256, sizeof(unsigned int));
      asolv = 256;
      rpmheadsize = 0;
      rpmhead = 0;
      i = 0;
      s = 0;
      while (dbc->c_get(dbc, &key, &data, DB_NEXT) == 0)
	{
	  if (!s)
	    s = pool_id2solvable(pool, repo_add_solvable(repo));
	  if (i >= asolv)
	    {
	      repo->rpmdbid = sat_realloc(repo->rpmdbid, (asolv + 256) * sizeof(unsigned int));
	      memset(repo->rpmdbid + asolv, 0, 256 * sizeof(unsigned int));
	      asolv += 256;
	    }
          if (key.size != 4)
	    {
	      fprintf(stderr, "corrupt Packages database (key size)\n");
	      exit(1);
	    }
	  dp = key.data;
	  if (byteswapped)
	    {
	      dbidp[0] = dp[3];
	      dbidp[1] = dp[2];
	      dbidp[2] = dp[1];
	      dbidp[3] = dp[0];
	    }
	  else
	    memcpy(dbidp, dp, 4);
	  if (dbid == 0)		/* the join key */
	    continue;
	  if (data.size < 8)
	    {
	      fprintf(stderr, "corrupt rpm database (size %u)\n", data.size);
	      exit(1);
	    }
	  if (data.size > rpmheadsize)
	    rpmhead = sat_realloc(rpmhead, sizeof(*rpmhead) + data.size);
	  memcpy(buf, data.data, 8);
	  rpmhead->cnt = buf[0] << 24  | buf[1] << 16  | buf[2] << 8 | buf[3];
	  rpmhead->dcnt = buf[4] << 24  | buf[5] << 16  | buf[6] << 8 | buf[7];
	  if (8 + rpmhead->cnt * 16 + rpmhead->dcnt > data.size)
	    {
	      fprintf(stderr, "corrupt rpm database (data size)\n");
	      exit(1);
	    }
	  memcpy(rpmhead->data, (unsigned char *)data.data + 8, rpmhead->cnt * 16 + rpmhead->dcnt);
	  rpmhead->dp = rpmhead->data + rpmhead->cnt * 16;
	  repo->rpmdbid[i] = dbid;
	  if (rpm2solv(pool, repo, repodata, s, rpmhead))
	    {
	      i++;
	      s = 0;
	    }
	  else
	    {
	      /* We can reuse this solvable, but make sure it's still
		 associated with this repo.  */
	      memset(s, 0, sizeof(*s));
	      s->repo = repo;
	    }
	}
      if (s)
	{
	  /* oops, could not reuse. free it instead */
          repo_free_solvable_block(repo, s - pool->solvables, 1, 1);
	  s = 0;
	}
      dbc->c_close(dbc);
      db->close(db, 0);
      db = 0;
    }
  else
    {
      if (db->open(db, 0, "/var/lib/rpm/Name", 0, DB_HASH, DB_RDONLY, 0664))
	{
	  perror("db->open /var/lib/rpm/Name");
	  exit(1);
	}
      if (db->get_byteswapped(db, &byteswapped))
	{
	  perror("db->get_byteswapped");
	  exit(1);
	}
      if (db->cursor(db, NULL, &dbc, 0))
	{
	  perror("db->cursor");
	  exit(1);
	}
      dbidp = (unsigned char *)&dbid;
      nrpmids = 0;
      rpmids = 0;
      while (dbc->c_get(dbc, &key, &data, DB_NEXT) == 0)
	{
	  if (key.size == 10 && !memcmp(key.data, "gpg-pubkey", 10))
	    continue;
	  dl = data.size;
	  dp = data.data;
	  while(dl >= 8)
	    {
	      if (byteswapped)
		{
		  dbidp[0] = dp[3];
		  dbidp[1] = dp[2];
		  dbidp[2] = dp[1];
		  dbidp[3] = dp[0];
		}
	      else
		memcpy(dbidp, dp, 4);
	      if ((nrpmids & 255) == 0)
		rpmids = sat_realloc(rpmids, sizeof(*rpmids) * (nrpmids + 256));
	      rpmids[nrpmids].dbid = dbid;
	      rpmids[nrpmids].name = sat_malloc((int)key.size + 1);
	      memcpy(rpmids[nrpmids].name, key.data, (int)key.size);
	      rpmids[nrpmids].name[(int)key.size] = 0;
	      nrpmids++;
	      dp += 8;
	      dl -= 8;
	    }
	}
      dbc->c_close(dbc);
      db->close(db, 0);
      db = 0;

      rp = rpmids;
      dbidp = (unsigned char *)&dbid;
      rpmheadsize = 0;
      rpmhead = 0;

      refhash = 0;
      refmask = 0;
      if (ref)
	{
	  refmask = mkmask(ref->nsolvables);
	  refhash = sat_calloc(refmask + 1, sizeof(Id));
	  for (i = 0; i < ref->nsolvables; i++)
	    {
	      h = ref->rpmdbid[i] & refmask;
	      while (refhash[h])
		h = (h + 317) & refmask;
	      refhash[h] = i + 1;	/* make it non-zero */
	    }
	}

      repo->rpmdbid = sat_calloc(nrpmids, sizeof(unsigned int));

      s = pool_id2solvable(pool, repo_add_solvable_block(repo, nrpmids));

      for (i = 0; i < nrpmids; i++, rp++, s++)
	{
	  dbid = rp->dbid;
	  repo->rpmdbid[i] = dbid;
	  if (refhash)
	    {
	      h = dbid & refmask;
	      while ((id = refhash[h]))
		{
		  if (ref->rpmdbid[id - 1] == dbid)
		    break;
		  h = (h + 317) & refmask;
		}
	      if (id)
		{
		  Solvable *r = ref->pool->solvables + ref->start + (id - 1);
		  if (pool == ref->pool)
		    {
		      s->name = r->name;
		      s->evr = r->evr;
		      s->arch = r->arch;
		      s->vendor = r->vendor;
		    }
		  else
		    {
		      if (r->name)
			s->name = str2id(pool, id2str(ref->pool, r->name), 1);
		      if (r->evr)
			s->evr = str2id(pool, id2str(ref->pool, r->evr), 1);
		      if (r->arch)
			s->arch = str2id(pool, id2str(ref->pool, r->arch), 1);
		      if (r->vendor)
			s->vendor = str2id(pool, id2str(ref->pool, r->vendor), 1);
		    }
		  s->provides = copydeps(pool, repo, r->provides, ref);
		  s->requires = copydeps(pool, repo, r->requires, ref);
		  s->conflicts = copydeps(pool, repo, r->conflicts, ref);
		  s->obsoletes = copydeps(pool, repo, r->obsoletes, ref);
		  s->recommends = copydeps(pool, repo, r->recommends, ref);
		  s->suggests = copydeps(pool, repo, r->suggests, ref);
		  s->supplements = copydeps(pool, repo, r->supplements, ref);
		  s->enhances  = copydeps(pool, repo, r->enhances, ref);
		  s->freshens = copydeps(pool, repo, r->freshens, ref);
		  continue;
		}
	    }
	  if (!db)
	    {
	      if (db_create(&db, 0, 0))
		{
		  perror("db_create");
		  exit(1);
		}
	      if (db->open(db, 0, "/var/lib/rpm/Packages", 0, DB_HASH, DB_RDONLY, 0664))
		{
		  perror("db->open /var/lib/rpm/Packages");
		  exit(1);
		}
	      if (db->get_byteswapped(db, &byteswapped))
		{
		  perror("db->get_byteswapped");
		  exit(1);
		}
	    }
	  if (byteswapped)
	    {
	      buf[0] = dbidp[3];
	      buf[1] = dbidp[2];
	      buf[2] = dbidp[1];
	      buf[3] = dbidp[0];
	    }
	  else
	    memcpy(buf, dbidp, 4);
	  key.data = buf;
	  key.size = 4;
	  data.data = 0;
	  data.size = 0;
	  if (db->get(db, NULL, &key, &data, 0))
	    {
	      perror("db->get");
	      fprintf(stderr, "corrupt rpm database\n");
	      exit(1);
	    }
	  if (data.size < 8)
	    {
	      fprintf(stderr, "corrupt rpm database (size)\n");
	      exit(1);
	    }
	  if (data.size > rpmheadsize)
	    rpmhead = sat_realloc(rpmhead, sizeof(*rpmhead) + data.size);
	  memcpy(buf, data.data, 8);
	  rpmhead->cnt = buf[0] << 24  | buf[1] << 16  | buf[2] << 8 | buf[3];
	  rpmhead->dcnt = buf[4] << 24  | buf[5] << 16  | buf[6] << 8 | buf[7];
	  if (8 + rpmhead->cnt * 16 + rpmhead->dcnt > data.size)
	    {
	      fprintf(stderr, "corrupt rpm database (data size)\n");
	      exit(1);
	    }
	  memcpy(rpmhead->data, (unsigned char *)data.data + 8, rpmhead->cnt * 16 + rpmhead->dcnt);
	  rpmhead->dp = rpmhead->data + rpmhead->cnt * 16;

	  rpm2solv(pool, repo, repodata, s, rpmhead);
	}

      if (refhash)
	sat_free(refhash);
      if (rpmids)
	{
	  for (i = 0; i < nrpmids; i++)
	    sat_free(rpmids[i].name);
	  sat_free(rpmids);
	}
    }
  if (rpmhead)
    sat_free(rpmhead);
  if (db)
    db->close(db, 0);
  if (repodata)
    repodata_internalize(repodata);
}

/* XXX: delete me! */
void rpmdb_dummy()
{
  makeevr(0, 0);
  split(0, 0, 0);
}
