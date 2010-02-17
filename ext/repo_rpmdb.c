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
#include <unistd.h>
#include <assert.h>

#include <rpm/rpmio.h>
#include <rpm/rpmpgp.h>
#include <rpm/header.h>
#include <rpm/rpmdb.h>

#ifndef DB_CREATE
# ifdef FEDORA
#  include <db4/db.h>
# else
#  include <rpm/db.h>
# endif
#endif

#include "pool.h"
#include "repo.h"
#include "hash.h"
#include "util.h"
#include "queue.h"
#include "chksum.h"
#include "repo_rpmdb.h"

/* 3: added triggers */
#define RPMDB_COOKIE_VERSION 3

#define TAG_NAME		1000
#define TAG_VERSION		1001
#define TAG_RELEASE		1002
#define TAG_EPOCH		1003
#define TAG_SUMMARY		1004
#define TAG_DESCRIPTION		1005
#define TAG_BUILDTIME		1006
#define TAG_BUILDHOST		1007
#define TAG_INSTALLTIME		1008
#define TAG_SIZE                1009
#define TAG_DISTRIBUTION	1010
#define TAG_VENDOR		1011
#define TAG_LICENSE		1014
#define TAG_PACKAGER		1015
#define TAG_GROUP		1016
#define TAG_URL			1020
#define TAG_ARCH		1022
#define TAG_FILESIZES		1028
#define TAG_FILEMODES		1030
#define TAG_FILEMD5S		1035
#define TAG_FILELINKTOS		1036
#define TAG_SOURCERPM		1044
#define TAG_PROVIDENAME		1047
#define TAG_REQUIREFLAGS	1048
#define TAG_REQUIRENAME		1049
#define TAG_REQUIREVERSION	1050
#define TAG_NOSOURCE		1051
#define TAG_NOPATCH		1052
#define TAG_CONFLICTFLAGS	1053
#define TAG_CONFLICTNAME	1054
#define TAG_CONFLICTVERSION	1055
#define TAG_TRIGGERNAME		1066
#define TAG_TRIGGERVERSION	1067
#define TAG_TRIGGERFLAGS	1068
#define TAG_OBSOLETENAME	1090
#define TAG_FILEDEVICES		1095
#define TAG_FILEINODES		1096
#define TAG_PROVIDEFLAGS	1112
#define TAG_PROVIDEVERSION	1113
#define TAG_OBSOLETEFLAGS	1114
#define TAG_OBSOLETEVERSION	1115
#define TAG_DIRINDEXES		1116
#define TAG_BASENAMES		1117
#define TAG_DIRNAMES		1118
#define TAG_PAYLOADFORMAT	1124
#define TAG_PATCHESNAME         1133
#define TAG_FILECOLORS		1140
#define TAG_SUGGESTSNAME	1156
#define TAG_SUGGESTSVERSION	1157
#define TAG_SUGGESTSFLAGS	1158
#define TAG_ENHANCESNAME	1159
#define TAG_ENHANCESVERSION	1160
#define TAG_ENHANCESFLAGS	1161

#define SIGTAG_SIZE		1000
#define SIGTAG_PGP		1002	/* RSA signature */
#define SIGTAG_MD5		1004	/* header+payload md5 checksum */
#define SIGTAG_GPG		1005	/* DSA signature */

#define DEP_LESS		(1 << 1)
#define DEP_GREATER		(1 << 2)
#define DEP_EQUAL		(1 << 3)
#define DEP_STRONG		(1 << 27)
#define DEP_PRE			((1 << 6) | (1 << 9) | (1 << 10) | (1 << 11) | (1 << 12))


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


static inline unsigned char *
headfindtag(RpmHead *h, int tag)
{
  unsigned int i;
  unsigned char *d, taga[4];
  d = h->dp - 16;
  taga[0] = tag >> 24;
  taga[1] = tag >> 16;
  taga[2] = tag >> 8;
  taga[3] = tag;
  for (i = 0; i < h->cnt; i++, d -= 16)
    if (d[3] == taga[3] && d[2] == taga[2] && d[1] == taga[1] && d[0] == taga[0])
      return d;
  return 0;
}

static int
headexists(RpmHead *h, int tag)
{
  return headfindtag(h, tag) ? 1 : 0;
}

static unsigned int *
headint32array(RpmHead *h, int tag, int *cnt)
{
  unsigned int i, o, *r;
  unsigned char *d = headfindtag(h, tag);

  if (!d || d[4] != 0 || d[5] != 0 || d[6] != 0 || d[7] != 4)
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

/* returns the first entry of an integer array */
static unsigned int
headint32(RpmHead *h, int tag)
{
  unsigned int i, o;
  unsigned char *d = headfindtag(h, tag);

  if (!d || d[4] != 0 || d[5] != 0 || d[6] != 0 || d[7] != 4)
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
  unsigned char *d = headfindtag(h, tag);

  if (!d || d[4] != 0 || d[5] != 0 || d[6] != 0 || d[7] != 3)
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
  unsigned int o;
  unsigned char *d = headfindtag(h, tag);
  /* 6: STRING, 9: I18NSTRING */
  if (!d || d[4] != 0 || d[5] != 0 || d[6] != 0 || (d[7] != 6 && d[7] != 9))
    return 0;
  o = d[8] << 24 | d[9] << 16 | d[10] << 8 | d[11];
  if (o >= h->dcnt)
    return 0;
  return (char *)h->dp + o;
}

static char **
headstringarray(RpmHead *h, int tag, int *cnt)
{
  unsigned int i, o;
  unsigned char *d = headfindtag(h, tag);
  char **r;

  if (!d || d[4] != 0 || d[5] != 0 || d[6] != 0 || d[7] != 8)
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

static unsigned char *
headbinary(RpmHead *h, int tag, unsigned int *sizep)
{
  unsigned int i, o;
  unsigned char *d = headfindtag(h, tag);
  if (!d || d[4] != 0 || d[5] != 0 || d[6] != 0 || d[7] != 7)
    return 0;
  o = d[8] << 24 | d[9] << 16 | d[10] << 8 | d[11];
  i = d[12] << 24 | d[13] << 16 | d[14] << 8 | d[15];
  if (o > h->dcnt || o + i < o || o + i > h->dcnt)
    return 0;
  if (sizep)
    *sizep = i;
  return h->dp + o;
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


static void
setutf8string(Repodata *repodata, Id handle, Id tag, const char *str)
{
  const unsigned char *cp;
  int state = 0;
  int c;
  unsigned char *buf = 0, *bp;

  /* check if it's already utf8, code taken from screen ;-) */
  cp = (const unsigned char *)str;
  while ((c = *cp++) != 0)
    {
      if (state)
	{
          if ((c & 0xc0) != 0x80)
            break; /* encoding error */
          c = (c & 0x3f) | (state << 6);
          if (!(state & 0x40000000))
	    {
              /* check for overlong sequences */
              if ((c & 0x820823e0) == 0x80000000)
                c = 0xfdffffff;
              else if ((c & 0x020821f0) == 0x02000000)
                c = 0xfff7ffff;
              else if ((c & 0x000820f8) == 0x00080000)
                c = 0xffffd000;
              else if ((c & 0x0000207c) == 0x00002000)
                c = 0xffffff70;
            }
        }
      else
	{
          /* new sequence */
          if (c >= 0xfe)
            break;
          else if (c >= 0xfc)
            c = (c & 0x01) | 0xbffffffc;    /* 5 bytes to follow */
          else if (c >= 0xf8)
            c = (c & 0x03) | 0xbfffff00;    /* 4 */
          else if (c >= 0xf0)
            c = (c & 0x07) | 0xbfffc000;    /* 3 */
          else if (c >= 0xe0)
            c = (c & 0x0f) | 0xbff00000;    /* 2 */
          else if (c >= 0xc2)
            c = (c & 0x1f) | 0xfc000000;    /* 1 */
          else if (c >= 0x80)
            break;
        }
      state = (c & 0x80000000) ? c : 0;
    }
  if (c)
    {
      /* not utf8, assume latin1 */
      buf = sat_malloc(2 * strlen(str) + 1);
      cp = (const unsigned char *)str;
      str = (char *)buf;
      bp = buf;
      while ((c = *cp++) != 0)
	{
	  if (c >= 0xc0)
	    {
	      *bp++ = 0xc3;
	      c ^= 0x80;
	    }
	  else if (c >= 0x80)
	    *bp++ = 0xc2;
	  *bp++ = c;
	}
      *bp++ = 0;
    }
  repodata_set_str(repodata, handle, tag, str);
  if (buf)
    sat_free(buf);
}


#define MAKEDEPS_FILTER_WEAK	(1 << 0)
#define MAKEDEPS_FILTER_STRONG	(1 << 1)
#define MAKEDEPS_NO_RPMLIB	(1 << 2)

/*
 * strong: 0: ignore strongness
 *         1: filter to strong
 *         2: filter to weak
 */
static unsigned int
makedeps(Pool *pool, Repo *repo, RpmHead *rpmhead, int tagn, int tagv, int tagf, int flags)
{
  char **n, **v;
  unsigned int *f;
  int i, cc, nc, vc, fc;
  int haspre;
  unsigned int olddeps;
  Id *ida;
  int strong;

  strong = flags & (MAKEDEPS_FILTER_STRONG|MAKEDEPS_FILTER_WEAK);
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
  haspre = 0;	/* add no prereq marker */
  if (flags)
    {
      /* we do filtering */
      cc = 0;
      for (i = 0; i < nc; i++)
	{
	  if (strong && (f[i] & DEP_STRONG) != (strong == MAKEDEPS_FILTER_WEAK ? 0 : DEP_STRONG))
	    continue;
	  if ((flags & MAKEDEPS_NO_RPMLIB) != 0)
	    if (!strncmp(n[i], "rpmlib(", 7))
	      continue;
	  if ((f[i] & DEP_PRE) != 0)
	    haspre = 1;
	  cc++;
	}
    }
  else if (tagn == TAG_REQUIRENAME)
    {
      /* no filtering, just look for the first prereq */
      for (i = 0; i < nc; i++)
	if ((f[i] & DEP_PRE) != 0)
	  {
	    haspre = 1;
	    break;
	  }
    }
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
	  haspre = 2;	/* pass two: prereqs */
	  i = 0;
	  *ida++ = SOLVABLE_PREREQMARKER;
	}
      if (strong && (f[i] & DEP_STRONG) != (strong == MAKEDEPS_FILTER_WEAK ? 0 : DEP_STRONG))
	continue;
      if (haspre == 1 && (f[i] & DEP_PRE) != 0)
	continue;
      if (haspre == 2 && (f[i] & DEP_PRE) == 0)
	continue;
      if ((flags & MAKEDEPS_NO_RPMLIB) != 0)
	if (!strncmp(n[i], "rpmlib(", 7))
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


#ifdef USE_FILEFILTER

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

#endif

static void
adddudata(Pool *pool, Repo *repo, Repodata *data, Solvable *s, RpmHead *rpmhead, char **dn, unsigned int *di, int fc, int dc)
{
  Id handle, did;
  int i, fszc;
  unsigned int *fkb, *fn, *fsz, *fm, *fino;
  unsigned int inotest[256], inotestok;

  if (!fc)
    return;
  fsz = headint32array(rpmhead, TAG_FILESIZES, &fszc);
  if (!fsz || fc != fszc)
    {
      sat_free(fsz);
      return;
    }
  /* stupid rpm records sizes of directories, so we have to check the mode */
  fm = headint16array(rpmhead, TAG_FILEMODES, &fszc);
  if (!fm || fc != fszc)
    {
      sat_free(fsz);
      sat_free(fm);
      return;
    }
  fino = headint32array(rpmhead, TAG_FILEINODES, &fszc);
  if (!fino || fc != fszc)
    {
      sat_free(fsz);
      sat_free(fm);
      sat_free(fino);
      return;
    }
  inotestok = 0;
  if (fc < sizeof(inotest))
    {
      memset(inotest, 0, sizeof(inotest));
      for (i = 0; i < fc; i++)
	{
	  int off, bit;
	  if (fsz[i] == 0 || !S_ISREG(fm[i]))
	    continue;
	  off = (fino[i] >> 5) & (sizeof(inotest)/sizeof(*inotest) - 1);
	  bit = 1 << (fino[i] & 31);
	  if ((inotest[off] & bit) != 0)
	    break;
	  inotest[off] |= bit;
	}
      if (i == fc)
	inotestok = 1;
    }
  if (!inotestok)
    {
      unsigned int *fdev = headint32array(rpmhead, TAG_FILEDEVICES, &fszc);
      unsigned int *fx, j;
      unsigned int mask, hash, hh;
      if (!fdev || fc != fszc)
	{
	  sat_free(fsz);
	  sat_free(fm);
	  sat_free(fdev);
	  sat_free(fino);
	  return;
	}
      mask = fc;
      while ((mask & (mask - 1)) != 0)
	mask = mask & (mask - 1);
      mask <<= 2;
      if (mask > sizeof(inotest)/sizeof(*inotest))
        fx = sat_calloc(mask, sizeof(unsigned int));
      else
	{
	  fx = inotest;
	  memset(fx, 0, mask * sizeof(unsigned int));
	}
      mask--;
      for (i = 0; i < fc; i++)
	{
	  if (fsz[i] == 0 || !S_ISREG(fm[i]))
	    continue;
	  hash = (fino[i] + fdev[i] * 31) & mask;
          hh = 7;
	  while ((j = fx[hash]) != 0)
	    {
	      if (fino[j - 1] == fino[i] && fdev[j - 1] == fdev[i])
		{
		  fsz[i] = 0;	/* kill entry */
		  break;
		}
	      hash = (hash + hh++) & mask;
	    }
	  if (!j)
	    fx[hash] = i + 1;
	}
      if (fx != inotest)
        sat_free(fx);
      sat_free(fdev);
    }
  sat_free(fino);
  fn = sat_calloc(dc, sizeof(unsigned int));
  fkb = sat_calloc(dc, sizeof(unsigned int));
  for (i = 0; i < fc; i++)
    {
      if (di[i] >= dc)
	continue;
      fn[di[i]]++;
      if (fsz[i] == 0 || !S_ISREG(fm[i]))
	continue;
      fkb[di[i]] += fsz[i] / 1024 + 1;
    }
  sat_free(fsz);
  sat_free(fm);
  /* commit */
  handle = s - pool->solvables;
  for (i = 0; i < dc; i++)
    {
      if (!fn[i])
	continue;
      if (!*dn[i])
	{
          if (s->arch == ARCH_SRC || s->arch == ARCH_NOSRC)
	    did = repodata_str2dir(data, "/usr/src", 1);
	  else
	    continue;	/* work around rpm bug */
	}
      else
        did = repodata_str2dir(data, dn[i], 1);
      repodata_add_dirnumnum(data, handle, SOLVABLE_DISKUSAGE, did, fkb[i], fn[i]);
    }
  sat_free(fn);
  sat_free(fkb);
}

/* assumes last processed array is provides! */
static unsigned int
addfileprovides(Pool *pool, Repo *repo, Repodata *data, Solvable *s, RpmHead *rpmhead, unsigned int olddeps)
{
  char **bn;
  char **dn;
  unsigned int *di;
  int bnc, dnc, dic;
  int i;
#ifdef USE_FILEFILTER
  int j;
  struct filefilter *ff;
#endif
#if 0
  char *fn = 0;
  int fna = 0;
#endif

  if (!data)
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

  if (data)
    adddudata(pool, repo, data, s, rpmhead, dn, di, bnc, dnc);

  for (i = 0; i < bnc; i++)
    {
#ifdef USE_FILEFILTER
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
#endif
#if 0
      j = strlen(bn[i]) + strlen(dn[di[i]]) + 1;
      if (j > fna)
	{
	  fna = j + 256;
	  fn = sat_realloc(fn, fna);
	}
      strcpy(fn, dn[di[i]]);
      strcat(fn, bn[i]);
      olddeps = repo_addid_dep(repo, olddeps, str2id(pool, fn, 1), SOLVABLE_FILEMARKER);
#endif
      if (data)
	{
	  Id handle, did;
	  char *b = bn[i];

	  handle = s - pool->solvables;
	  did = repodata_str2dir(data, dn[di[i]], 1);
	  if (!did)
	    {
	      did = repodata_str2dir(data, "/", 1);
	      if (b && b[0] == '/')
		b++;	/* work around rpm bug */
	    }
	  repodata_add_dirstr(data, handle, SOLVABLE_FILELIST, did, b);
	}
    }
#if 0
  if (fn)
    sat_free(fn);
#endif
  sat_free(bn);
  sat_free(dn);
  sat_free(di);
  return olddeps;
}

static void
addsourcerpm(Pool *pool, Repodata *data, Id handle, char *sourcerpm, char *name, char *evr)
{
  const char *p, *sevr, *sarch;

  p = strrchr(sourcerpm, '.');
  if (!p || strcmp(p, ".rpm") != 0)
    return;
  p--;
  while (p > sourcerpm && *p != '.')
    p--;
  if (*p != '.' || p == sourcerpm)
    return;
  sarch = p-- + 1;
  while (p > sourcerpm && *p != '-')
    p--;
  if (*p != '-' || p == sourcerpm)
    return;
  p--;
  while (p > sourcerpm && *p != '-')
    p--;
  if (*p != '-' || p == sourcerpm)
    return;
  sevr = p + 1;
  if (!strcmp(sarch, "src.rpm"))
    repodata_set_constantid(data, handle, SOLVABLE_SOURCEARCH, ARCH_SRC);
  else if (!strcmp(sarch, "nosrc.rpm"))
    repodata_set_constantid(data, handle, SOLVABLE_SOURCEARCH, ARCH_NOSRC);
  else
    repodata_set_constantid(data, handle, SOLVABLE_SOURCEARCH, strn2id(pool, sarch, strlen(sarch) - 4, 1));
  if (evr && !strncmp(sevr, evr, sarch - sevr - 1) && evr[sarch - sevr - 1] == 0)
    repodata_set_void(data, handle, SOLVABLE_SOURCEEVR);
  else
    repodata_set_id(data, handle, SOLVABLE_SOURCEEVR, strn2id(pool, sevr, sarch - sevr - 1, 1));
  if (name && !strncmp(sourcerpm, name, sevr - sourcerpm - 1) && name[sevr - sourcerpm - 1] == 0)
    repodata_set_void(data, handle, SOLVABLE_SOURCENAME);
  else
    repodata_set_id(data, handle, SOLVABLE_SOURCENAME, strn2id(pool, sourcerpm, sevr - sourcerpm - 1, 1));
}

static int
rpm2solv(Pool *pool, Repo *repo, Repodata *data, Solvable *s, RpmHead *rpmhead, int flags)
{
  char *name;
  char *evr;
  char *sourcerpm;

  name = headstring(rpmhead, TAG_NAME);
  if (!strcmp(name, "gpg-pubkey"))
    return 0;
  s->name = str2id(pool, name, 1);
  if (!s->name)
    {
      fprintf(stderr, "package has no name\n");
      exit(1);
    }
  sourcerpm = headstring(rpmhead, TAG_SOURCERPM);
  if (sourcerpm)
    s->arch = str2id(pool, headstring(rpmhead, TAG_ARCH), 1);
  else
    {
      if (headexists(rpmhead, TAG_NOSOURCE) || headexists(rpmhead, TAG_NOPATCH))
        s->arch = ARCH_NOSRC;
      else
        s->arch = ARCH_SRC;
    }
  if (!s->arch)
    s->arch = ARCH_NOARCH;
  evr = headtoevr(rpmhead);
  s->evr = str2id(pool, evr, 1);
  s->vendor = str2id(pool, headstring(rpmhead, TAG_VENDOR), 1);

  s->provides = makedeps(pool, repo, rpmhead, TAG_PROVIDENAME, TAG_PROVIDEVERSION, TAG_PROVIDEFLAGS, 0);
  if ((flags & RPM_ADD_NO_FILELIST) == 0)
    s->provides = addfileprovides(pool, repo, data, s, rpmhead, s->provides);
  if (s->arch != ARCH_SRC && s->arch != ARCH_NOSRC)
    s->provides = repo_addid_dep(repo, s->provides, rel2id(pool, s->name, s->evr, REL_EQ, 1), 0);
  s->requires = makedeps(pool, repo, rpmhead, TAG_REQUIRENAME, TAG_REQUIREVERSION, TAG_REQUIREFLAGS, (flags & RPM_ADD_NO_RPMLIBREQS) ? MAKEDEPS_NO_RPMLIB : 0);
  s->conflicts = makedeps(pool, repo, rpmhead, TAG_CONFLICTNAME, TAG_CONFLICTVERSION, TAG_CONFLICTFLAGS, 0);
  s->obsoletes = makedeps(pool, repo, rpmhead, TAG_OBSOLETENAME, TAG_OBSOLETEVERSION, TAG_OBSOLETEFLAGS, 0);

  s->recommends = makedeps(pool, repo, rpmhead, TAG_SUGGESTSNAME, TAG_SUGGESTSVERSION, TAG_SUGGESTSFLAGS, MAKEDEPS_FILTER_STRONG);
  s->suggests = makedeps(pool, repo, rpmhead, TAG_SUGGESTSNAME, TAG_SUGGESTSVERSION, TAG_SUGGESTSFLAGS, MAKEDEPS_FILTER_WEAK);
  s->supplements = makedeps(pool, repo, rpmhead, TAG_ENHANCESNAME, TAG_ENHANCESVERSION, TAG_ENHANCESFLAGS, MAKEDEPS_FILTER_STRONG);
  s->enhances  = makedeps(pool, repo, rpmhead, TAG_ENHANCESNAME, TAG_ENHANCESVERSION, TAG_ENHANCESFLAGS, MAKEDEPS_FILTER_WEAK);
  s->supplements = repo_fix_supplements(repo, s->provides, s->supplements, 0);
  s->conflicts = repo_fix_conflicts(repo, s->conflicts);

  if (data)
    {
      Id handle;
      char *str;
      unsigned int u32;

      handle = s - pool->solvables;
      str = headstring(rpmhead, TAG_SUMMARY);
      if (str)
        setutf8string(data, handle, SOLVABLE_SUMMARY, str);
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
                setutf8string(data, handle, SOLVABLE_DESCRIPTION, str);
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
	        setutf8string(data, handle, SOLVABLE_AUTHORS, str);
	      free(str);
	    }
	  else if (*str)
	    setutf8string(data, handle, SOLVABLE_DESCRIPTION, str);
	}
      str = headstring(rpmhead, TAG_GROUP);
      if (str)
        repodata_set_poolstr(data, handle, SOLVABLE_GROUP, str);
      str = headstring(rpmhead, TAG_LICENSE);
      if (str)
        repodata_set_poolstr(data, handle, SOLVABLE_LICENSE, str);
      str = headstring(rpmhead, TAG_URL);
      if (str)
	repodata_set_str(data, handle, SOLVABLE_URL, str);
      str = headstring(rpmhead, TAG_DISTRIBUTION);
      if (str)
	repodata_set_poolstr(data, handle, SOLVABLE_DISTRIBUTION, str);
      str = headstring(rpmhead, TAG_PACKAGER);
      if (str)
	repodata_set_poolstr(data, handle, SOLVABLE_PACKAGER, str);
      u32 = headint32(rpmhead, TAG_BUILDTIME);
      if (u32)
        repodata_set_num(data, handle, SOLVABLE_BUILDTIME, u32);
      u32 = headint32(rpmhead, TAG_INSTALLTIME);
      if (u32)
        repodata_set_num(data, handle, SOLVABLE_INSTALLTIME, u32);
      u32 = headint32(rpmhead, TAG_SIZE);
      if (u32)
        repodata_set_num(data, handle, SOLVABLE_INSTALLSIZE, (u32 + 1023) / 1024);
      if (sourcerpm)
	addsourcerpm(pool, data, handle, sourcerpm, name, evr);
      if ((flags & RPM_ADD_TRIGGERS) != 0)
	{
	  Id id, lastid;
	  unsigned int ida = makedeps(pool, repo, rpmhead, TAG_TRIGGERNAME, TAG_TRIGGERVERSION, TAG_TRIGGERFLAGS, 0);

	  lastid = 0;
	  for (; (id = repo->idarraydata[ida]) != 0; ida++)
	    {
	      /* we currently do not support rel ids in incore data, so
	       * strip off versioning information */
	      while (ISRELDEP(id))
		{
		  Reldep *rd = GETRELDEP(pool, id);
		  id = rd->name;
		}
	      if (id == lastid)
		continue;
	      repodata_add_idarray(data, handle, SOLVABLE_TRIGGERS, id);
	      lastid = id;
	    }
	}
    }
  sat_free(evr);
  return 1;
}

static Id
copyreldep(Pool *pool, Pool *frompool, Id id)
{
  Reldep *rd = GETRELDEP(frompool, id);
  Id name = rd->name, evr = rd->evr;
  if (ISRELDEP(name))
    name = copyreldep(pool, frompool, name);
  else
    name = str2id(pool, id2str(frompool, name), 1);
  if (ISRELDEP(evr))
    evr = copyreldep(pool, frompool, evr);
  else
    evr = str2id(pool, id2str(frompool, evr), 1);
  return rel2id(pool, name, evr, rd->flags, 1);
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
	    id = copyreldep(pool, frompool, id);
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

#define COPYDIR_DIRCACHE_SIZE 512

static Id copydir_complex(Pool *pool, Repodata *data, Stringpool *fromspool, Repodata *fromdata, Id did, Id *cache);

static inline Id
copydir(Pool *pool, Repodata *data, Stringpool *fromspool, Repodata *fromdata, Id did, Id *cache)
{
  if (cache && cache[did & 255] == did)
    return cache[(did & 255) + 256];
  return copydir_complex(pool, data, fromspool, fromdata, did, cache);
}

static Id
copydir_complex(Pool *pool, Repodata *data, Stringpool *fromspool, Repodata *fromdata, Id did, Id *cache)
{
  Id parent = dirpool_parent(&fromdata->dirpool, did);
  Id compid = dirpool_compid(&fromdata->dirpool, did);
  if (parent)
    parent = copydir(pool, data, fromspool, fromdata, parent, cache);
  if (fromspool != &pool->ss)
    compid = str2id(pool, stringpool_id2str(fromspool, compid), 1);
  compid = dirpool_add_dir(&data->dirpool, parent, compid, 1);
  if (cache)
    {
      cache[did & 255] = did;
      cache[(did & 255) + 256] = compid;
    }
  return compid;
}

struct solvable_copy_cbdata {
  Repodata *data;
  Id handle;
  Id *dircache;
};

static int
solvable_copy_cb(void *vcbdata, Solvable *r, Repodata *fromdata, Repokey *key, KeyValue *kv)
{
  struct solvable_copy_cbdata *cbdata = vcbdata;
  Id id, keyname;
  Repodata *data = cbdata->data;
  Id handle = cbdata->handle;
  Pool *pool = data->repo->pool, *frompool = fromdata->repo->pool;
  Stringpool *fromspool = fromdata->localpool ? &fromdata->spool : &frompool->ss;

  keyname = key->name;
  if (keyname >= ID_NUM_INTERNAL)
    keyname = str2id(pool, id2str(frompool, keyname), 1);
  switch(key->type)
    {
    case REPOKEY_TYPE_ID:
    case REPOKEY_TYPE_CONSTANTID:
      id = kv->id;
      assert(!data->localpool);	/* implement me! */
      if (pool != frompool || fromdata->localpool)
	{
	  if (ISRELDEP(id))
	    id = copyreldep(pool, frompool, id);
	  else
	    id = str2id(pool, stringpool_id2str(fromspool, id), 1);
	}
      if (key->type == REPOKEY_TYPE_ID)
        repodata_set_id(data, handle, keyname, id);
      else
        repodata_set_constantid(data, handle, keyname, id);
      break;
    case REPOKEY_TYPE_STR:
      repodata_set_str(data, handle, keyname, kv->str);
      break;
    case REPOKEY_TYPE_VOID:
      repodata_set_void(data, handle, keyname);
      break;
    case REPOKEY_TYPE_NUM:
      repodata_set_num(data, handle, keyname, kv->num);
      break;
    case REPOKEY_TYPE_CONSTANT:
      repodata_set_constant(data, handle, keyname, kv->num);
      break;
    case REPOKEY_TYPE_DIRNUMNUMARRAY:
      id = kv->id;
      assert(!data->localpool);	/* implement me! */
      id = copydir(pool, data, fromspool, fromdata, id, cbdata->dircache);
      repodata_add_dirnumnum(data, handle, keyname, id, kv->num, kv->num2);
      break;
    case REPOKEY_TYPE_DIRSTRARRAY:
      id = kv->id;
      assert(!data->localpool);	/* implement me! */
      id = copydir(pool, data, fromspool, fromdata, id, cbdata->dircache);
      repodata_add_dirstr(data, handle, keyname, id, kv->str);
      break;
    default:
      break;
    }
  return 0;
}

static void
solvable_copy(Solvable *s, Solvable *r, Repodata *data, Id *dircache)
{
  Repo *repo = s->repo;
  Repo *fromrepo = r->repo;
  Pool *pool = repo->pool;
  struct solvable_copy_cbdata cbdata;

  /* copy solvable data */
  if (pool == fromrepo->pool)
    {
      s->name = r->name;
      s->evr = r->evr;
      s->arch = r->arch;
      s->vendor = r->vendor;
    }
  else
    {
      if (r->name)
	s->name = str2id(pool, id2str(fromrepo->pool, r->name), 1);
      if (r->evr)
	s->evr = str2id(pool, id2str(fromrepo->pool, r->evr), 1);
      if (r->arch)
	s->arch = str2id(pool, id2str(fromrepo->pool, r->arch), 1);
      if (r->vendor)
	s->vendor = str2id(pool, id2str(fromrepo->pool, r->vendor), 1);
    }
  s->provides = copydeps(pool, repo, r->provides, fromrepo);
  s->requires = copydeps(pool, repo, r->requires, fromrepo);
  s->conflicts = copydeps(pool, repo, r->conflicts, fromrepo);
  s->obsoletes = copydeps(pool, repo, r->obsoletes, fromrepo);
  s->recommends = copydeps(pool, repo, r->recommends, fromrepo);
  s->suggests = copydeps(pool, repo, r->suggests, fromrepo);
  s->supplements = copydeps(pool, repo, r->supplements, fromrepo);
  s->enhances  = copydeps(pool, repo, r->enhances, fromrepo);

  /* copy all attributes */
  if (!data)
    return;
  cbdata.data = data;
  cbdata.handle = s - pool->solvables;
  cbdata.dircache = dircache;
  repo_search(fromrepo, (r - fromrepo->pool->solvables), 0, 0, SEARCH_NO_STORAGE_SOLVABLE, solvable_copy_cb, &cbdata);
}

/* used to sort entries returned in some database order */
static int
rpmids_sort_cmp(const void *va, const void *vb, void *dp)
{
  struct rpmid const *a = va, *b = vb;
  int r;
  r = strcmp(a->name, b->name);
  if (r)
    return r;
  return a->dbid - b->dbid;
}

static int
pkgids_sort_cmp(const void *va, const void *vb, void *dp)
{
  Repo *repo = dp;
  Pool *pool = repo->pool;
  Solvable *a = pool->solvables + *(Id *)va;
  Solvable *b = pool->solvables + *(Id *)vb;
  Id *rpmdbid;

  if (a->name != b->name)
    return strcmp(id2str(pool, a->name), id2str(pool, b->name));
  rpmdbid = repo->rpmdbid;
  return rpmdbid[(a - pool->solvables) - repo->start] - rpmdbid[(b - pool->solvables) - repo->start];
}

static void
swap_solvables(Repo *repo, Repodata *data, Id pa, Id pb)
{
  Pool *pool = repo->pool;
  Solvable tmp;

  tmp = pool->solvables[pa];
  pool->solvables[pa] = pool->solvables[pb];
  pool->solvables[pb] = tmp;
  if (repo->rpmdbid)
    {
      Id tmpid = repo->rpmdbid[pa - repo->start];
      repo->rpmdbid[pa - repo->start] = repo->rpmdbid[pb - repo->start];
      repo->rpmdbid[pb - repo->start] = tmpid;
    }
  /* only works if nothing is already internalized! */
  if (data && data->attrs)
    {
      Id *tmpattrs = data->attrs[pa - data->start];
      data->attrs[pa - data->start] = data->attrs[pb - data->start];
      data->attrs[pb - data->start] = tmpattrs;
    }
}

static void
mkrpmdbcookie(struct stat *st, unsigned char *cookie)
{
  memset(cookie, 0, 32);
  cookie[3] = RPMDB_COOKIE_VERSION;
  memcpy(cookie + 16, &st->st_ino, sizeof(st->st_ino));
  memcpy(cookie + 24, &st->st_dev, sizeof(st->st_dev));
}

/* should look in /usr/lib/rpm/macros instead, but we want speed... */
static DB_ENV *
opendbenv(const char *rootdir)
{
  char dbpath[PATH_MAX];
  DB_ENV *dbenv = 0;
  int r;

  if (db_env_create(&dbenv, 0))
    {
      perror("db_env_create");
      return 0;
    }
#if defined(FEDORA) && (DB_VERSION_MAJOR >= 5 || (DB_VERSION_MAJOR == 4 && DB_VERSION_MINOR >= 5))
  dbenv->set_thread_count(dbenv, 8);
#endif
  snprintf(dbpath, PATH_MAX, "%s/var/lib/rpm", rootdir ? rootdir : "");
  if (access(dbpath, W_OK) == -1)
    {
      r = dbenv->open(dbenv, dbpath, DB_CREATE|DB_PRIVATE|DB_INIT_MPOOL, 0);
    }
  else
    {
#ifdef FEDORA
      r = dbenv->open(dbenv, dbpath, DB_CREATE|DB_INIT_CDB|DB_INIT_MPOOL, 0644);
#else
      r = dbenv->open(dbenv, dbpath, DB_CREATE|DB_PRIVATE|DB_INIT_MPOOL, 0);
#endif
    }
  if (r)
    {
      perror("dbenv open");
      dbenv->close(dbenv, 0);
      return 0;
    }
  return dbenv;
}
 

static int
count_headers(const char *rootdir, DB_ENV *dbenv)
{
  char dbpath[PATH_MAX];
  struct stat statbuf;
  int byteswapped;
  DB *db = 0;
  DBC *dbc = 0;
  int count = 0;
  DBT dbkey;
  DBT dbdata;

  snprintf(dbpath, PATH_MAX, "%s/var/lib/rpm/Name", rootdir);
  if (stat(dbpath, &statbuf))
    return 0;
  memset(&dbkey, 0, sizeof(dbkey));
  memset(&dbdata, 0, sizeof(dbdata));
  if (db_create(&db, dbenv, 0))
    {
      perror("db_create");
      exit(1);
    }
  if (db->open(db, 0, "Name", 0, DB_UNKNOWN, DB_RDONLY, 0664))
    {
      perror("db->open Name index");
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
  while (dbc->c_get(dbc, &dbkey, &dbdata, DB_NEXT) == 0)
    count += dbdata.size >> 3;
  dbc->c_close(dbc);
  db->close(db, 0);
  return count;
}

/*
 * read rpm db as repo
 *
 */

void
repo_add_rpmdb(Repo *repo, Repo *ref, const char *rootdir, int flags)
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
  char dbpath[PATH_MAX];
  DB_ENV *dbenv = 0;
  DBT dbkey;
  DBT dbdata;
  struct stat packagesstat;
  unsigned char newcookie[32];
  const unsigned char *oldcookie = 0;
  Id oldcookietype = 0;
  Repodata *data;
  int count = 0, done = 0;
  unsigned int now;

  now = sat_timems(0);
  memset(&dbkey, 0, sizeof(dbkey));
  memset(&dbdata, 0, sizeof(dbdata));

  if (!rootdir)
    rootdir = "";

  data = repo_add_repodata(repo, flags);

  if (ref && !(ref->nsolvables && ref->rpmdbid))
    ref = 0;

  if (!(dbenv = opendbenv(rootdir)))
    exit(1);

  /* XXX: should get ro lock of Packages database! */
  snprintf(dbpath, PATH_MAX, "%s/var/lib/rpm/Packages", rootdir);
  if (stat(dbpath, &packagesstat))
    {
      perror(dbpath);
      exit(1);
    }
  mkrpmdbcookie(&packagesstat, newcookie);
  repodata_set_bin_checksum(data, SOLVID_META, REPOSITORY_RPMDBCOOKIE, REPOKEY_TYPE_SHA256, newcookie);

  if (ref)
    oldcookie = repo_lookup_bin_checksum(ref, SOLVID_META, REPOSITORY_RPMDBCOOKIE, &oldcookietype);
  if (!ref || !oldcookie || oldcookietype != REPOKEY_TYPE_SHA256 || memcmp(oldcookie, newcookie, 32) != 0)
    {
      Id *pkgids;
      int solvstart = 0, solvend = 0;

      if ((flags & RPMDB_REPORT_PROGRESS) != 0)
	count = count_headers(rootdir, dbenv);
      if (db_create(&db, dbenv, 0))
	{
	  perror("db_create");
	  exit(1);
	}
      if (db->open(db, 0, "Packages", 0, DB_UNKNOWN, DB_RDONLY, 0664))
	{
	  perror("db->open Packages index");
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
      rpmheadsize = 0;
      rpmhead = 0;
      i = 0;
      s = 0;
      while (dbc->c_get(dbc, &dbkey, &dbdata, DB_NEXT) == 0)
	{
	  if (!s)
	    {
	      s = pool_id2solvable(pool, repo_add_solvable(repo));
	      if (!solvstart)
		solvstart = s - pool->solvables;
	      solvend = s - pool->solvables + 1;
	    }
	  if (!repo->rpmdbid)
	    repo->rpmdbid = repo_sidedata_create(repo, sizeof(Id));
          if (dbkey.size != 4)
	    {
	      fprintf(stderr, "corrupt Packages database (key size)\n");
	      exit(1);
	    }
	  dp = dbkey.data;
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
	  if (dbdata.size < 8)
	    {
	      fprintf(stderr, "corrupt rpm database (size %u)\n", dbdata.size);
	      exit(1);
	    }
	  if (dbdata.size > rpmheadsize)
	    {
	      rpmheadsize = dbdata.size + 128;
	      rpmhead = sat_realloc(rpmhead, sizeof(*rpmhead) + rpmheadsize);
	    }
	  memcpy(buf, dbdata.data, 8);
	  rpmhead->cnt = buf[0] << 24  | buf[1] << 16  | buf[2] << 8 | buf[3];
	  rpmhead->dcnt = buf[4] << 24  | buf[5] << 16  | buf[6] << 8 | buf[7];
	  if (8 + rpmhead->cnt * 16 + rpmhead->dcnt > dbdata.size)
	    {
	      fprintf(stderr, "corrupt rpm database (data size)\n");
	      exit(1);
	    }
	  memcpy(rpmhead->data, (unsigned char *)dbdata.data + 8, rpmhead->cnt * 16 + rpmhead->dcnt);
	  rpmhead->dp = rpmhead->data + rpmhead->cnt * 16;
	  repo->rpmdbid[(s - pool->solvables) - repo->start] = dbid;
	  if (rpm2solv(pool, repo, data, s, rpmhead, flags | RPM_ADD_TRIGGERS))
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
	  if ((flags & RPMDB_REPORT_PROGRESS) != 0)
	    {
	      if (done < count)
	        done++;
	      if (done < count && (done - 1) * 100 / count != done * 100 / count)
	        pool_debug(pool, SAT_ERROR, "%%%% %d\n", done * 100 / count);
	    }
	}
      if (s)
	{
	  /* oops, could not reuse. free it instead */
          repo_free_solvable_block(repo, s - pool->solvables, 1, 1);
	  solvend--;
	  s = 0;
	}
      dbc->c_close(dbc);
      db->close(db, 0);
      db = 0;
      /* now sort all solvables in the new solvstart..solvend block */
      if (solvend - solvstart > 1)
	{
	  pkgids = sat_malloc2(solvend - solvstart, sizeof(Id));
	  for (i = solvstart; i < solvend; i++)
	    pkgids[i - solvstart] = i;
	  sat_sort(pkgids, solvend - solvstart, sizeof(Id), pkgids_sort_cmp, repo);
	  /* adapt order */
	  for (i = solvstart; i < solvend; i++)
	    {
	      int j = pkgids[i - solvstart];
	      while (j < i)
		j = pkgids[i - solvstart] = pkgids[j - solvstart];
	      if (j != i)
	        swap_solvables(repo, data, i, j);
	    }
	  sat_free(pkgids);
	}
    }
  else
    {
      Id dircache[COPYDIR_DIRCACHE_SIZE];		/* see copydir */

      memset(dircache, 0, sizeof(dircache));
      if (db_create(&db, dbenv, 0))
	{
	  perror("db_create");
	  exit(1);
	}
      if (db->open(db, 0, "Name", 0, DB_UNKNOWN, DB_RDONLY, 0664))
	{
	  perror("db->open Name index");
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
      while (dbc->c_get(dbc, &dbkey, &dbdata, DB_NEXT) == 0)
	{
	  if (dbkey.size == 10 && !memcmp(dbkey.data, "gpg-pubkey", 10))
	    continue;
	  dl = dbdata.size;
	  dp = dbdata.data;
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
	      rpmids = sat_extend(rpmids, nrpmids, 1, sizeof(*rpmids), 255);
	      rpmids[nrpmids].dbid = dbid;
	      rpmids[nrpmids].name = sat_malloc((int)dbkey.size + 1);
	      memcpy(rpmids[nrpmids].name, dbkey.data, (int)dbkey.size);
	      rpmids[nrpmids].name[(int)dbkey.size] = 0;
	      nrpmids++;
	      dp += 8;
	      dl -= 8;
	    }
	}
      dbc->c_close(dbc);
      db->close(db, 0);
      db = 0;

      /* sort rpmids */
      sat_sort(rpmids, nrpmids, sizeof(*rpmids), rpmids_sort_cmp, 0);

      dbidp = (unsigned char *)&dbid;
      rpmheadsize = 0;
      rpmhead = 0;

      /* create hash from dbid to ref */
      refmask = mkmask(ref->nsolvables);
      refhash = sat_calloc(refmask + 1, sizeof(Id));
      for (i = 0; i < ref->end - ref->start; i++)
	{
	  if (!ref->rpmdbid[i])
	    continue;
	  h = ref->rpmdbid[i] & refmask;
	  while (refhash[h])
	    h = (h + 317) & refmask;
	  refhash[h] = i + 1;	/* make it non-zero */
	}

      /* count the misses, they will cost us time */
      if ((flags & RPMDB_REPORT_PROGRESS) != 0)
        {
	  for (i = 0, rp = rpmids; i < nrpmids; i++, rp++)
	    {
	      dbid = rp->dbid;
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
		    continue;
		}
	      count++;
	    }
        }

      s = pool_id2solvable(pool, repo_add_solvable_block(repo, nrpmids));
      if (!repo->rpmdbid)
        repo->rpmdbid = repo_sidedata_create(repo, sizeof(Id));

      for (i = 0, rp = rpmids; i < nrpmids; i++, rp++, s++)
	{
	  dbid = rp->dbid;
	  repo->rpmdbid[(s - pool->solvables) - repo->start] = dbid;
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
		  if (r->repo == ref)
		    {
		      solvable_copy(s, r, data, dircache);
		      continue;
		    }
		}
	    }
	  if (!db)
	    {
	      if (db_create(&db, dbenv, 0))
		{
		  perror("db_create");
		  exit(1);
		}
	      if (db->open(db, 0, "Packages", 0, DB_UNKNOWN, DB_RDONLY, 0664))
		{
		  perror("db->open var/lib/rpm/Packages");
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
	  dbkey.data = buf;
	  dbkey.size = 4;
	  dbdata.data = 0;
	  dbdata.size = 0;
	  if (db->get(db, NULL, &dbkey, &dbdata, 0))
	    {
	      perror("db->get");
	      fprintf(stderr, "corrupt rpm database, key %d not found\n", dbid);
	      fprintf(stderr, "please run 'rpm --rebuilddb' to recreate the database index files\n");
	      exit(1);
	    }
	  if (dbdata.size < 8)
	    {
	      fprintf(stderr, "corrupt rpm database (size)\n");
	      exit(1);
	    }
	  if (dbdata.size > rpmheadsize)
	    {
	      rpmheadsize = dbdata.size + 128;
	      rpmhead = sat_realloc(rpmhead, sizeof(*rpmhead) + rpmheadsize);
	    }
	  memcpy(buf, dbdata.data, 8);
	  rpmhead->cnt = buf[0] << 24  | buf[1] << 16  | buf[2] << 8 | buf[3];
	  rpmhead->dcnt = buf[4] << 24  | buf[5] << 16  | buf[6] << 8 | buf[7];
	  if (8 + rpmhead->cnt * 16 + rpmhead->dcnt > dbdata.size)
	    {
	      fprintf(stderr, "corrupt rpm database (data size)\n");
	      exit(1);
	    }
	  memcpy(rpmhead->data, (unsigned char *)dbdata.data + 8, rpmhead->cnt * 16 + rpmhead->dcnt);
	  rpmhead->dp = rpmhead->data + rpmhead->cnt * 16;

	  rpm2solv(pool, repo, data, s, rpmhead, flags | RPM_ADD_TRIGGERS);
	  if ((flags & RPMDB_REPORT_PROGRESS) != 0)
	    {
	      if (done < count)
		done++;
	      if (done < count && (done - 1) * 100 / count != done * 100 / count)
		pool_debug(pool, SAT_ERROR, "%%%% %d\n", done * 100 / count);
	    }
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
  if (db)
    db->close(db, 0);
  dbenv->close(dbenv, 0);
  if (rpmhead)
    sat_free(rpmhead);
  if (!(flags & REPO_NO_INTERNALIZE))
    repodata_internalize(data);
  if ((flags & RPMDB_REPORT_PROGRESS) != 0)
    pool_debug(pool, SAT_ERROR, "%%%% 100\n");
  POOL_DEBUG(SAT_DEBUG_STATS, "repo_add_rpmdb took %d ms\n", sat_timems(now));
  POOL_DEBUG(SAT_DEBUG_STATS, "repo size: %d solvables\n", repo->nsolvables);
  POOL_DEBUG(SAT_DEBUG_STATS, "repo memory used: %d K incore, %d K idarray\n", data->incoredatalen/1024, repo->idarraysize / (int)(1024/sizeof(Id)));
}


static inline unsigned int
getu32(const unsigned char *dp)
{
  return dp[0] << 24 | dp[1] << 16 | dp[2] << 8 | dp[3];
}


void
repo_add_rpms(Repo *repo, const char **rpms, int nrpms, int flags)
{
  int i, sigdsize, sigcnt, l;
  Pool *pool = repo->pool;
  Solvable *s;
  RpmHead *rpmhead = 0;
  int rpmheadsize = 0;
  char *payloadformat;
  FILE *fp;
  unsigned char lead[4096];
  int headerstart, headerend;
  struct stat stb;
  Repodata *data;
  unsigned char pkgid[16];
  int gotpkgid;
  Id chksumtype = 0;
  void *chksumh = 0;

  data = repo_add_repodata(repo, flags);

  if ((flags & RPM_ADD_WITH_SHA256SUM) != 0)
    chksumtype = REPOKEY_TYPE_SHA256;
  else if ((flags & RPM_ADD_WITH_SHA1SUM) != 0)
    chksumtype = REPOKEY_TYPE_SHA1;
  for (i = 0; i < nrpms; i++)
    {
      if ((fp = fopen(rpms[i], "r")) == 0)
	{
	  perror(rpms[i]);
	  continue;
	}
      if (fstat(fileno(fp), &stb))
	{
	  perror("stat");
	  continue;
	}
      if (chksumh)
	chksumh = sat_chksum_free(chksumh, 0);
      if (chksumtype)
	chksumh = sat_chksum_create(chksumtype);
      if (fread(lead, 96 + 16, 1, fp) != 1 || getu32(lead) != 0xedabeedb)
	{
	  fprintf(stderr, "%s: not a rpm\n", rpms[i]);
	  fclose(fp);
	  continue;
	}
      if (chksumh)
	sat_chksum_add(chksumh, lead, 96 + 16);
      if (lead[78] != 0 || lead[79] != 5)
	{
	  fprintf(stderr, "%s: not a V5 header\n", rpms[i]);
	  fclose(fp);
	  continue;
	}
      if (getu32(lead + 96) != 0x8eade801)
	{
	  fprintf(stderr, "%s: bad signature header\n", rpms[i]);
	  fclose(fp);
	  continue;
	}
      sigcnt = getu32(lead + 96 + 8);
      sigdsize = getu32(lead + 96 + 12);
      if (sigcnt >= 0x4000000 || sigdsize >= 0x40000000)
	{
	  fprintf(stderr, "%s: bad signature header\n", rpms[i]);
	  fclose(fp);
	  continue;
	}
      sigdsize += sigcnt * 16;
      sigdsize = (sigdsize + 7) & ~7;
      headerstart = 96 + 16 + sigdsize;
      gotpkgid = 0;
      if ((flags & RPM_ADD_WITH_PKGID) != 0)
	{
	  unsigned char *chksum;
	  unsigned int chksumsize;
	  /* extract pkgid from the signature header */
	  if (sigdsize > rpmheadsize)
	    {
	      rpmheadsize = sigdsize + 128;
	      rpmhead = sat_realloc(rpmhead, sizeof(*rpmhead) + rpmheadsize);
	    }
	  if (fread(rpmhead->data, sigdsize, 1, fp) != 1)
	    {
	      fprintf(stderr, "%s: unexpected EOF\n", rpms[i]);
	      fclose(fp);
	      continue;
	    }
	  if (chksumh)
	    sat_chksum_add(chksumh, rpmhead->data, sigdsize);
	  rpmhead->cnt = sigcnt;
	  rpmhead->dcnt = sigdsize - sigcnt * 16;
	  rpmhead->dp = rpmhead->data + rpmhead->cnt * 16;
	  chksum = headbinary(rpmhead, SIGTAG_MD5, &chksumsize);
	  if (chksum && chksumsize == 16)
	    {
	      gotpkgid = 1;
	      memcpy(pkgid, chksum, 16);
	    }
	}
      else
	{
	  /* just skip the signature header */
	  while (sigdsize)
	    {
	      l = sigdsize > 4096 ? 4096 : sigdsize;
	      if (fread(lead, l, 1, fp) != 1)
		{
		  fprintf(stderr, "%s: unexpected EOF\n", rpms[i]);
		  fclose(fp);
		  continue;
		}
	      if (chksumh)
		sat_chksum_add(chksumh, lead, l);
	      sigdsize -= l;
	    }
	}
      if (fread(lead, 16, 1, fp) != 1)
	{
	  fprintf(stderr, "%s: unexpected EOF\n", rpms[i]);
	  fclose(fp);
	  continue;
	}
      if (chksumh)
	sat_chksum_add(chksumh, lead, 16);
      if (getu32(lead) != 0x8eade801)
	{
	  fprintf(stderr, "%s: bad header\n", rpms[i]);
	  fclose(fp);
	  continue;
	}
      sigcnt = getu32(lead + 8);
      sigdsize = getu32(lead + 12);
      if (sigcnt >= 0x4000000 || sigdsize >= 0x40000000)
	{
	  fprintf(stderr, "%s: bad header\n", rpms[i]);
	  fclose(fp);
	  continue;
	}
      l = sigdsize + sigcnt * 16;
      headerend = headerstart + 16 + l;
      if (l > rpmheadsize)
	{
	  rpmheadsize = l + 128;
	  rpmhead = sat_realloc(rpmhead, sizeof(*rpmhead) + rpmheadsize);
	}
      if (fread(rpmhead->data, l, 1, fp) != 1)
	{
	  fprintf(stderr, "%s: unexpected EOF\n", rpms[i]);
	  fclose(fp);
	  continue;
	}
      if (chksumh)
	sat_chksum_add(chksumh, rpmhead->data, l);
      rpmhead->cnt = sigcnt;
      rpmhead->dcnt = sigdsize;
      rpmhead->dp = rpmhead->data + rpmhead->cnt * 16;
      if (headexists(rpmhead, TAG_PATCHESNAME))
	{
	  /* this is a patch rpm, ignore */
	  fclose(fp);
	  continue;
	}
      payloadformat = headstring(rpmhead, TAG_PAYLOADFORMAT);
      if (payloadformat && !strcmp(payloadformat, "drpm"))
	{
	  /* this is a delta rpm */
	  fclose(fp);
	  continue;
	}
      if (chksumh)
	while ((l = fread(lead, 1, sizeof(lead), fp)) > 0)
	  sat_chksum_add(chksumh, lead, l);
      fclose(fp);
      s = pool_id2solvable(pool, repo_add_solvable(repo));
      rpm2solv(pool, repo, data, s, rpmhead, flags);
      if (data)
	{
	  Id handle = s - pool->solvables;
	  repodata_set_location(data, handle, 0, 0, rpms[i]);
	  if (S_ISREG(stb.st_mode))
	    repodata_set_num(data, handle, SOLVABLE_DOWNLOADSIZE, (unsigned int)((stb.st_size + 1023) / 1024));
	  repodata_set_num(data, handle, SOLVABLE_HEADEREND, headerend);
	  if (gotpkgid)
	    repodata_set_bin_checksum(data, handle, SOLVABLE_PKGID, REPOKEY_TYPE_MD5, pkgid);
	  if (chksumh)
	    repodata_set_bin_checksum(data, handle, SOLVABLE_CHECKSUM, chksumtype, sat_chksum_get(chksumh, 0));
	}
    }
  if (chksumh)
    chksumh = sat_chksum_free(chksumh, 0);
  if (rpmhead)
    sat_free(rpmhead);
  if (!(flags & REPO_NO_INTERNALIZE))
    repodata_internalize(data);
}

static inline void
linkhash(const char *lt, char *hash)
{
  unsigned int r = 0;
  const unsigned char *str = (const unsigned char *)lt;
  int l, c;

  l = strlen(lt);
  while ((c = *str++) != 0)
    r += (r << 3) + c;
  sprintf(hash, "%08x", r);
  sprintf(hash + 8, "%08x", l);
  sprintf(hash + 16, "%08x", 0);
  sprintf(hash + 24, "%08x", 0);
}

void
rpm_iterate_filelist(void *rpmhandle, int flags, void (*cb)(void *, const char *, int, const char *), void *cbdata)
{
  RpmHead *rpmhead = rpmhandle;
  char **bn;
  char **dn;
  char **md = 0;
  char **lt = 0;
  unsigned int *di, diidx;
  unsigned int *co = 0;
  unsigned int lastdir;
  int lastdirl;
  unsigned int *fm;
  int cnt, dcnt, cnt2;
  int i, l1, l;
  char *space = 0;
  int spacen = 0;
  char md5[33], *md5p = 0;

  dn = headstringarray(rpmhead, TAG_DIRNAMES, &dcnt);
  if (!dn)
    return;
  if ((flags & RPM_ITERATE_FILELIST_ONLYDIRS) != 0)
    {
      for (i = 0; i < dcnt; i++)
	(*cb)(cbdata, dn[i], 0, (char *)0);
      sat_free(dn);
      return;
    }
  bn = headstringarray(rpmhead, TAG_BASENAMES, &cnt);
  if (!bn)
    {
      sat_free(dn);
      return;
    }
  di = headint32array(rpmhead, TAG_DIRINDEXES, &cnt2);
  if (!di || cnt != cnt2)
    {
      sat_free(di);
      sat_free(bn);
      sat_free(dn);
      return;
    }
  fm = headint16array(rpmhead, TAG_FILEMODES, &cnt2);
  if (!fm || cnt != cnt2)
    {
      sat_free(fm);
      sat_free(di);
      sat_free(bn);
      sat_free(dn);
      return;
    }
  if ((flags & RPM_ITERATE_FILELIST_WITHMD5) != 0)
    {
      md = headstringarray(rpmhead, TAG_FILEMD5S, &cnt2);
      if (!md || cnt != cnt2)
	{
	  sat_free(md);
	  sat_free(fm);
	  sat_free(di);
	  sat_free(bn);
	  sat_free(dn);
	  return;
	}
    }
  if ((flags & RPM_ITERATE_FILELIST_WITHCOL) != 0)
    {
      co = headint32array(rpmhead, TAG_FILECOLORS, &cnt2);
      if (!co || cnt != cnt2)
	{
	  sat_free(co);
	  sat_free(md);
	  sat_free(fm);
	  sat_free(di);
	  sat_free(bn);
	  sat_free(dn);
	  return;
	}
    }
  lastdir = dcnt;
  lastdirl = 0;
  for (i = 0; i < cnt; i++)
    {
      diidx = di[i];
      if (diidx >= dcnt)
	continue;
      l1 = lastdir == diidx ? lastdirl : strlen(dn[diidx]);
      if (l1 == 0)
	continue;
      l = l1 + strlen(bn[i]) + 1;
      if (l > spacen)
	{
	  spacen = l + 16;
	  space = sat_realloc(space, spacen);
	}
      if (lastdir != diidx)
	{
          strcpy(space, dn[diidx]);
	  lastdir = diidx;
	  lastdirl = l1;
	}
      strcpy(space + l1, bn[i]);
      if (md)
	{
	  md5p = md[i];
	  if (S_ISLNK(fm[i]))
	    {
	      md5p = 0;
	      if (!lt)
		{
		  lt = headstringarray(rpmhead, TAG_FILELINKTOS, &cnt2);
		  if (cnt != cnt2)
		    lt = sat_free(lt);
		}
	      if (lt)
		{
		  linkhash(lt[i], md5);
		  md5p = md5;
		}
	    }
	  if (!md5p)
	    {
	      sprintf(md5, "%08x%08x", (fm[i] >> 12) & 65535, 0);
	      md5p = md5;
	    }
	}
      (*cb)(cbdata, space, co ? (fm[i] | co[i] << 24) : fm[i], md5p);
    }
  sat_free(space);
  sat_free(lt);
  sat_free(md);
  sat_free(fm);
  sat_free(di);
  sat_free(bn);
  sat_free(dn);
  sat_free(co);
}

char *
rpm_query(void *rpmhandle, Id what)
{
  const char *name, *arch, *sourcerpm;
  char *evr, *r;
  int l;

  RpmHead *rpmhead = rpmhandle;
  r = 0;
  switch (what)
    {
    case 0:
      name = headstring(rpmhead, TAG_NAME);
      if (!name)
	name = "";
      sourcerpm = headstring(rpmhead, TAG_SOURCERPM);
      if (sourcerpm)
	arch = headstring(rpmhead, TAG_ARCH);
      else
	{
	  if (headexists(rpmhead, TAG_NOSOURCE) || headexists(rpmhead, TAG_NOPATCH))
	    arch = "nosrc";
	  else
	    arch = "src";
	}
      if (!arch)
	arch = "noarch";
      evr = headtoevr(rpmhead);
      l = strlen(name) + 1 + strlen(evr) + 1 + strlen(arch) + 1;
      r = sat_malloc(l);
      sprintf(r, "%s-%s.%s", name, evr, arch);
      free(evr);
      break;
    case SOLVABLE_NAME:
      name = headstring(rpmhead, TAG_NAME);
      r = strdup(name);
      break;
    case SOLVABLE_EVR:
      r = headtoevr(rpmhead);
      break;
    }
  return r;
}


struct rpm_by_state {
  RpmHead *rpmhead;
  int rpmheadsize;

  int dbopened;
  DB_ENV *dbenv;
  DB *db;
  int byteswapped;
};

struct rpmdbentry {
  Id rpmdbid;
  Id nameoff;
};

#define ENTRIES_BLOCK 255
#define NAMEDATA_BLOCK 1023

static struct rpmdbentry *
getinstalledrpmdbids(struct rpm_by_state *state, const char *index, const char *match, int *nentriesp, char **namedatap)
{
  DB_ENV *dbenv = 0;
  DB *db = 0;
  DBC *dbc = 0;
  int byteswapped;
  DBT dbkey;
  DBT dbdata;
  Id rpmdbid;
  unsigned char *dp;
  int dl;

  char *namedata = 0;
  int namedatal = 0;
  struct rpmdbentry *entries = 0;
  int nentries = 0;

  *nentriesp = 0;
  *namedatap = 0;

  dbenv = state->dbenv;
  if (db_create(&db, dbenv, 0))
    {
      perror("db_create");
      return 0;
    }
  if (db->open(db, 0, index, 0, DB_UNKNOWN, DB_RDONLY, 0664))
    {
      perror("db->open index");
      db->close(db, 0);
      return 0;
    }
  if (db->get_byteswapped(db, &byteswapped))
    {
      perror("db->get_byteswapped");
      db->close(db, 0);
      return 0;
    }
  if (db->cursor(db, NULL, &dbc, 0))
    {
      perror("db->cursor");
      db->close(db, 0);
      return 0;
    }
  memset(&dbkey, 0, sizeof(dbkey));
  memset(&dbdata, 0, sizeof(dbdata));
  if (match)
    {
      dbkey.data = (void *)match;
      dbkey.size = strlen(match);
    }
  while (dbc->c_get(dbc, &dbkey, &dbdata, match ? DB_SET : DB_NEXT) == 0)
    {
      if (!match && dbkey.size == 10 && !memcmp(dbkey.data, "gpg-pubkey", 10))
	continue;
      dl = dbdata.size;
      dp = dbdata.data;
      while(dl >= 8)
	{
	  if (byteswapped)
	    {
	      ((char *)&rpmdbid)[0] = dp[3];
	      ((char *)&rpmdbid)[1] = dp[2];
	      ((char *)&rpmdbid)[2] = dp[1];
	      ((char *)&rpmdbid)[3] = dp[0];
	    }
	  else
	    memcpy((char *)&rpmdbid, dp, 4);
	  entries = sat_extend(entries, nentries, 1, sizeof(*entries), ENTRIES_BLOCK);
	  entries[nentries].rpmdbid = rpmdbid;
	  entries[nentries].nameoff = namedatal;
	  nentries++;
	  namedata = sat_extend(namedata, namedatal, dbkey.size + 1, 1, NAMEDATA_BLOCK);
	  memcpy(namedata + namedatal, dbkey.data, dbkey.size);
	  namedata[namedatal + dbkey.size] = 0;
	  namedatal += dbkey.size + 1;
	  dp += 8;
	  dl -= 8;
	}
      if (match)
	break;
    }
  dbc->c_close(dbc);
  db->close(db, 0);
  *nentriesp = nentries;
  *namedatap = namedata;
  return entries;
}

static void
freestate(struct rpm_by_state *state)
{
  /* close down */
  if (!state)
    return;
  if (state->db)
    state->db->close(state->db, 0);
  if (state->dbenv)
    state->dbenv->close(state->dbenv, 0);
  sat_free(state->rpmhead);
}

int
rpm_installedrpmdbids(const char *rootdir, const char *index, const char *match, Queue *rpmdbidq)
{
  struct rpm_by_state state;
  struct rpmdbentry *entries;
  int nentries, i;
  char *namedata;

  if (!index)
    index = "Name";
  if (rpmdbidq)
    queue_empty(rpmdbidq);
  memset(&state, 0, sizeof(state));
  if (!(state.dbenv = opendbenv(rootdir)))
    return 0;
  entries = getinstalledrpmdbids(&state, index, match, &nentries, &namedata);
  if (rpmdbidq)
    for (i = 0; i < nentries; i++)
      queue_push(rpmdbidq, entries[i].rpmdbid);
  sat_free(entries);
  sat_free(namedata);
  freestate(&state);
  return nentries;
}

void *
rpm_byrpmdbid(Id rpmdbid, const char *rootdir, void **statep)
{
  struct rpm_by_state *state = *statep;
  unsigned char buf[16];
  DBT dbkey;
  DBT dbdata;
  RpmHead *rpmhead;

  if (!rpmdbid)
    {
      /* close down */
      freestate(state);
      sat_free(state);
      *statep = (void *)0;
      return 0;
    }

  if (!state)
    {
      state = sat_calloc(1, sizeof(*state));
      *statep = state;
    }
  if (!state->dbopened)
    {
      state->dbopened = 1;
      if (!state->dbenv && !(state->dbenv = opendbenv(rootdir)))
	return 0;
      if (db_create(&state->db, state->dbenv, 0))
	{
	  perror("db_create");
	  state->db = 0;
	  state->dbenv->close(state->dbenv, 0);
	  state->dbenv = 0;
	  return 0;
	}
      if (state->db->open(state->db, 0, "Packages", 0, DB_UNKNOWN, DB_RDONLY, 0664))
	{
	  perror("db->open var/lib/rpm/Packages");
	  state->db->close(state->db, 0);
	  state->db = 0;
	  state->dbenv->close(state->dbenv, 0);
	  state->dbenv = 0;
	  return 0;
	}
      if (state->db->get_byteswapped(state->db, &state->byteswapped))
	{
	  perror("db->get_byteswapped");
	  state->db->close(state->db, 0);
	  state->db = 0;
	  state->dbenv->close(state->dbenv, 0);
	  state->dbenv = 0;
	  return 0;
	}
    }
  memcpy(buf, &rpmdbid, 4);
  if (state->byteswapped)
    {
      unsigned char bx;
      bx = buf[0]; buf[0] = buf[3]; buf[3] = bx;
      bx = buf[1]; buf[1] = buf[2]; buf[2] = bx;
    }
  memset(&dbkey, 0, sizeof(dbkey));
  memset(&dbdata, 0, sizeof(dbdata));
  dbkey.data = buf;
  dbkey.size = 4;
  dbdata.data = 0;
  dbdata.size = 0;
  if (state->db->get(state->db, NULL, &dbkey, &dbdata, 0))
    {
      perror("db->get");
      return 0;
    }
  if (dbdata.size < 8)
    {
      fprintf(stderr, "corrupt rpm database (size)\n");
      return 0;
    }
  if (dbdata.size > state->rpmheadsize)
    {
      state->rpmheadsize = dbdata.size + 128;
      state->rpmhead = sat_realloc(state->rpmhead, sizeof(*rpmhead) + state->rpmheadsize);
    }
  rpmhead = state->rpmhead;
  memcpy(buf, dbdata.data, 8);
  rpmhead->cnt = buf[0] << 24  | buf[1] << 16  | buf[2] << 8 | buf[3];
  rpmhead->dcnt = buf[4] << 24  | buf[5] << 16  | buf[6] << 8 | buf[7];
  if (8 + rpmhead->cnt * 16 + rpmhead->dcnt > dbdata.size)
    {
      fprintf(stderr, "corrupt rpm database (data size)\n");
      return 0;
    }
  memcpy(rpmhead->data, (unsigned char *)dbdata.data + 8, rpmhead->cnt * 16 + rpmhead->dcnt);
  rpmhead->dp = rpmhead->data + rpmhead->cnt * 16;
  return rpmhead;
}
 
void *
rpm_byfp(FILE *fp, const char *name, void **statep)
{
  struct rpm_by_state *state = *statep;
  int headerstart, headerend;
  RpmHead *rpmhead;
  int sigdsize, sigcnt, l;
  unsigned char lead[4096];

  if (!fp)
    return rpm_byrpmdbid(0, 0, statep);
  if (!state)
    {
      state = sat_calloc(1, sizeof(*state));
      *statep = state;
    }
  if (fread(lead, 96 + 16, 1, fp) != 1 || getu32(lead) != 0xedabeedb)
    {
      fprintf(stderr, "%s: not a rpm\n", name);
      return 0;
    }
  if (lead[78] != 0 || lead[79] != 5)
    {
      fprintf(stderr, "%s: not a V5 header\n", name);
      return 0;
    }
  if (getu32(lead + 96) != 0x8eade801)
    {
      fprintf(stderr, "%s: bad signature header\n", name);
      return 0;
    }
  sigcnt = getu32(lead + 96 + 8);
  sigdsize = getu32(lead + 96 + 12);
  if (sigcnt >= 0x4000000 || sigdsize >= 0x40000000)
    {
      fprintf(stderr, "%s: bad signature header\n", name);
      return 0;
    }
  sigdsize += sigcnt * 16;
  sigdsize = (sigdsize + 7) & ~7;
  headerstart = 96 + 16 + sigdsize;
  while (sigdsize)
    {
      l = sigdsize > 4096 ? 4096 : sigdsize;
      if (fread(lead, l, 1, fp) != 1)
	{
	  fprintf(stderr, "%s: unexpected EOF\n", name);
	  return 0;
	}
      sigdsize -= l;
    }
  if (fread(lead, 16, 1, fp) != 1)
    {
      fprintf(stderr, "%s: unexpected EOF\n", name);
      return 0;
    }
  if (getu32(lead) != 0x8eade801)
    {
      fprintf(stderr, "%s: bad header\n", name);
      fclose(fp);
      return 0;
    }
  sigcnt = getu32(lead + 8);
  sigdsize = getu32(lead + 12);
  if (sigcnt >= 0x4000000 || sigdsize >= 0x40000000)
    {
      fprintf(stderr, "%s: bad header\n", name);
      fclose(fp);
      return 0;
    }
  l = sigdsize + sigcnt * 16;
  headerend = headerstart + 16 + l;
  if (l > state->rpmheadsize)
    {
      state->rpmheadsize = l + 128;
      state->rpmhead = sat_realloc(state->rpmhead, sizeof(*state->rpmhead) + state->rpmheadsize);
    }
  rpmhead = state->rpmhead;
  if (fread(rpmhead->data, l, 1, fp) != 1)
    {
      fprintf(stderr, "%s: unexpected EOF\n", name);
      fclose(fp);
      return 0;
    }
  rpmhead->cnt = sigcnt;
  rpmhead->dcnt = sigdsize;
  rpmhead->dp = rpmhead->data + rpmhead->cnt * 16;
  return rpmhead;
}

void *
rpm_byrpmh(Header h, void **statep)
{
  struct rpm_by_state *state = *statep;
  const unsigned char *uh;
  int sigdsize, sigcnt, l;
  RpmHead *rpmhead;

  uh = headerUnload(h);
  if (!uh)
    return 0;
  sigcnt = getu32(uh);
  sigdsize = getu32(uh + 4);
  l = sigdsize + sigcnt * 16;
  if (!state)
    {
      state = sat_calloc(1, sizeof(*state));
      *statep = state;
    }
  if (l > state->rpmheadsize)
    {
      state->rpmheadsize = l + 128;
      state->rpmhead = sat_realloc(state->rpmhead, sizeof(*state->rpmhead) + state->rpmheadsize);
    }
  rpmhead = state->rpmhead;
  memcpy(rpmhead->data, uh + 8, l - 8);
  free((void *)uh);
  rpmhead->cnt = sigcnt;
  rpmhead->dcnt = sigdsize;
  rpmhead->dp = rpmhead->data + rpmhead->cnt * 16;
  return rpmhead;
}


static char *
r64dec1(char *p, unsigned int *vp, int *eofp)
{
  int i, x;
  unsigned int v = 0;

  for (i = 0; i < 4; )
    {
      x = *p++;
      if (!x)
	return 0;
      if (x >= 'A' && x <= 'Z')
	x -= 'A';
      else if (x >= 'a' && x <= 'z')
	x -= 'a' - 26;
      else if (x >= '0' && x <= '9')
	x -= '0' - 52;
      else if (x == '+')
	x = 62;
      else if (x == '/')
	x = 63;
      else if (x == '=')
	{
	  x = 0;
	  if (i == 0)
	    {
	      *eofp = 3;
	      *vp = 0;
	      return p - 1;
	    }
	  *eofp += 1;
	}
      else
	continue;
      v = v << 6 | x;
      i++;
    }
  *vp = v;
  return p;
}

static unsigned int 
crc24(unsigned char *p, int len)
{
  unsigned int crc = 0xb704ceL;
  int i;

  while (len--)
    {
      crc ^= (*p++) << 16;
      for (i = 0; i < 8; i++)
        if ((crc <<= 1) & 0x1000000)
	  crc ^= 0x1864cfbL;
    }
  return crc & 0xffffffL;
}

static unsigned char *
unarmor(char *pubkey, int *pktlp)
{
  char *p;
  int l, eof;
  unsigned char *buf, *bp;
  unsigned int v;

  *pktlp = 0;
  while (strncmp(pubkey, "-----BEGIN PGP PUBLIC KEY BLOCK-----", 36) != 0)
    {
      pubkey = strchr(pubkey, '\n');
      if (!pubkey)
	return 0;
      pubkey++;
    }
  pubkey = strchr(pubkey, '\n');
  if (!pubkey++)
    return 0;
  /* skip header lines */
  for (;;)
    {
      while (*pubkey == ' ' || *pubkey == '\t')
	pubkey++;
      if (*pubkey == '\n')
	break;
      pubkey = strchr(pubkey, '\n');
      if (!pubkey++)
	return 0;
    }
  pubkey++;
  p = strchr(pubkey, '=');
  if (!p)
    return 0;
  l = p - pubkey;
  bp = buf = sat_malloc(l * 3 / 4 + 4);
  eof = 0;
  while (!eof)
    {
      pubkey = r64dec1(pubkey, &v, &eof);
      if (!pubkey)
	{
	  sat_free(buf);
	  return 0;
	}
      *bp++ = v >> 16;
      *bp++ = v >> 8;
      *bp++ = v;
    }
  while (*pubkey == ' ' || *pubkey == '\t' || *pubkey == '\n' || *pubkey == '\r')
    pubkey++;
  bp -= eof;
  if (*pubkey != '=' || (pubkey = r64dec1(pubkey + 1, &v, &eof)) == 0)
    {
      sat_free(buf);
      return 0;
    }
  if (v != crc24(buf, bp - buf))
    {
      sat_free(buf);
      return 0;
    }
  while (*pubkey == ' ' || *pubkey == '\t' || *pubkey == '\n' || *pubkey == '\r')
    pubkey++;
  if (strncmp(pubkey, "-----END PGP PUBLIC KEY BLOCK-----", 34) != 0)
    {
      sat_free(buf);
      return 0;
    }
  *pktlp = bp - buf;
  return buf;
}

static void
parsekeydata(Solvable *s, Repodata *data, unsigned char *p, int pl)
{
  int x, tag, l;
  unsigned char keyid[8];
  unsigned int kcr = 0, maxex = 0;
  unsigned char *pubkey = 0;
  int pubkeyl = 0;
  unsigned char *userid = 0;
  int useridl = 0;

  for (; pl; p += l, pl -= l)
    {
      x = *p++;
      pl--;
      if (!(x & 128) || pl <= 0)
	return;
      if ((x & 64) == 0)
	{
	  /* old format */
	  tag = (x & 0x3c) >> 2;
	  x &= 3;
	  if (x == 3)
	    return;
	  l = 1 << x;
	  if (pl < l)
	    return;
	  x = 0;
	  while (l--)
	    {
	      x = x << 8 | *p++;
	      pl--;
	    }
	  l = x;
	}
      else
	{
	  tag = (x & 0x3f);
	  x = *p++;
	  pl--;
	  if (x < 192)
	    l = x;
	  else if (x >= 192 && x < 224)
	    {
	      if (pl <= 0)
		return;
	      l = ((x - 192) << 8) + *p++ + 192;
	      pl--;
	    }
	  else if (x == 255)
	    {
	      if (pl <= 4)
		return;
	      l = p[0] << 24 | p[1] << 16 | p[2] << 8 | p[3];
	      p += 4;
	      pl -= 4;
	    }
	  else
	    return;
	}
      if (pl < l)
	return;
      if (tag == 6)
	{
	  pubkey = sat_realloc(pubkey, l);
	  if (l)
	    memcpy(pubkey, p, l);
	  pubkeyl = l;
	  kcr = 0;
	  if (p[0] == 3)
	    {
	      unsigned int ex;
	      void *h;
	      kcr = p[1] << 24 | p[2] << 16 | p[3] << 8 | p[4];
	      ex = 0;
	      if (p[5] || p[6])
		{
		  ex = kcr + 24*3600 * (p[5] << 8 | p[6]);
		  if (ex > maxex)
		    maxex = ex;
		}
	      memset(keyid, 0, 8);
	      if (p[7] == 1)	/* RSA */
		{
		  int i, ql;
		  unsigned char fp[16];
		  char fpx[32 + 1];
		  unsigned char *q;

		  ql = ((p[8] << 8 | p[9]) + 7) / 8;
		  memcpy(keyid, p + 10 + ql - 8, 8);
		  h = sat_chksum_create(REPOKEY_TYPE_MD5);
		  sat_chksum_add(h, p + 10, ql);
		  q = p + 10 + ql;
		  ql = ((q[0] << 8 | q[1]) + 7) / 8;
		  sat_chksum_add(h, q + 2, ql);
		  sat_chksum_free(h, fp);
		  for (i = 0; i < 16; i++)
		    sprintf(fpx + i * 2, "%02x", fp[i]);
		  setutf8string(data, s - s->repo->pool->solvables, PUBKEY_FINGERPRINT, fpx);
		}
	    }
	  else if (p[0] == 4)
	    {
	      int i;
	      void *h;
	      unsigned char hdr[3];
	      unsigned char fp[20];
	      char fpx[40 + 1];

	      kcr = p[1] << 24 | p[2] << 16 | p[3] << 8 | p[4];
	      hdr[0] = 0x99;
	      hdr[1] = l >> 8;
	      hdr[2] = l;
	      h = sat_chksum_create(REPOKEY_TYPE_SHA1);
	      sat_chksum_add(h, hdr, 3);
	      sat_chksum_add(h, p, l);
	      sat_chksum_free(h, fp);
	      for (i = 0; i < 20; i++)
		sprintf(fpx + i * 2, "%02x", fp[i]);
	      setutf8string(data, s - s->repo->pool->solvables, PUBKEY_FINGERPRINT, fpx);
	      memcpy(keyid, fp + 12, 8);
	    }
	}
      if (tag == 2)
	{
	  if (p[0] == 3 && p[1] == 5)
	    {
#if 0
	      Id htype = 0;
#endif
	      // printf("V3 signature packet\n");
	      if (p[2] != 0x10 && p[2] != 0x11 && p[2] != 0x12 && p[2] != 0x13 && p[2] != 0x1f)
		continue;
	      if (!memcmp(keyid, p + 6, 8))
		{
		  // printf("SELF SIG\n");
		}
	      else
		{
		  // printf("OTHER SIG\n");
		}
#if 0
	      if (p[16] == 1)
		htype = REPOKEY_TYPE_MD5;
	      else if (p[16] == 2)
		htype = REPOKEY_TYPE_SHA1;
	      else if (p[16] == 8)
		htype = REPOKEY_TYPE_SHA256;
	      if (htype)
		{
		  void *h = sat_chksum_create(htype);
		  unsigned char b[3], *cs;

		  b[0] = 0x99;
		  b[1] = pubkeyl >> 8;
		  b[2] = pubkeyl;
		  sat_chksum_add(h, b, 3);
		  sat_chksum_add(h, pubkey, pubkeyl);
		  if (p[2] >= 0x10 && p[2] <= 0x13)
		    sat_chksum_add(h, userid, useridl);
		  sat_chksum_add(h, p + 2, 5);
		  cs = sat_chksum_get(h, 0);
		  sat_chksum_free(h, 0);
		}
#endif
	    }
	  if (p[0] == 4)
	    {
	      int j, ql, haveissuer;
	      unsigned char *q;
	      unsigned int ex = 0, scr = 0;
	      unsigned char issuer[8];

	      // printf("V4 signature packet\n");
	      if (p[1] != 0x10 && p[1] != 0x11 && p[1] != 0x12 && p[1] != 0x13 && p[1] != 0x1f)
		continue;
	      haveissuer = 0;
	      ex = 0;
	      q = p + 4;
	      for (j = 0; q && j < 2; j++)
		{
		  ql = q[0] << 8 | q[1];
		  q += 2;
		  while (ql)
		    {
		      int sl;
		      x = *q++;
		      ql--;
		      if (x < 192)
			sl = x;
		      else if (x == 255)
			{
			  if (ql < 4)
			    {
			      q = 0;
			      break;
			    }
			  sl = q[0] << 24 | q[1] << 16 | q[2] << 8 | q[3];
			  q += 4;
			  ql -= 4;
			}
		      else
			{
			  if (ql < 1)
			    {
			      q = 0;
			      break;
			    }
			  sl = ((x - 192) << 8) + *q++ + 192;
			  ql--;
			}
		      if (ql < sl)
			{
			  q = 0;
			  break;
			}
		      x = q[0] & 127;
		      // printf("%d SIGSUB %d %d\n", j, x, sl);
		      if (x == 16 && sl == 9 && !haveissuer)
			{
			  memcpy(issuer, q + 1, 8);
			  haveissuer = 1;
			}
		      if (x == 2 && j == 0)
			scr = q[1] << 24 | q[2] << 16 | q[3] << 8 | q[4];
		      if (x == 9 && j == 0)
			ex = q[1] << 24 | q[2] << 16 | q[3] << 8 | q[4];
		      q += sl;
		      ql -= sl;
		    }
		}
	      if (ex)
	        ex += kcr;
	      if (haveissuer)
		{
#if 0
		  Id htype = 0;
		  if (p[3] == 1)
		    htype = REPOKEY_TYPE_MD5;
		  else if (p[3] == 2)
		    htype = REPOKEY_TYPE_SHA1;
		  else if (p[3] == 8)
		    htype = REPOKEY_TYPE_SHA256;
		  if (htype && pubkeyl)
		    {
		      void *h = sat_chksum_create(htype);
		      unsigned char b[6], *cs;
		      unsigned int hl;

		      b[0] = 0x99;
		      b[1] = pubkeyl >> 8;
		      b[2] = pubkeyl;
		      sat_chksum_add(h, b, 3);
		      sat_chksum_add(h, pubkey, pubkeyl);
		      if (p[1] >= 0x10 && p[1] <= 0x13)
			{
			  b[0] = 0xb4;
			  b[1] = useridl >> 24;
			  b[2] = useridl >> 16;
			  b[3] = useridl >> 8;
			  b[4] = useridl;
			  sat_chksum_add(h, b, 5);
			  sat_chksum_add(h, userid, useridl);
			}
		      hl = 6 + (p[4] << 8 | p[5]);
		      sat_chksum_add(h, p, hl);
		      b[0] = 4;
		      b[1] = 0xff;
		      b[2] = hl >> 24;
		      b[3] = hl >> 16;
		      b[4] = hl >> 8;
		      b[5] = hl;
		      sat_chksum_add(h, b, 6);
		      cs = sat_chksum_get(h, 0);
		      sat_chksum_free(h, 0);
		    }
#endif
		  if (!memcmp(keyid, issuer, 8))
		    {
		      // printf("SELF SIG cr %d ex %d\n", cr, ex);
		      if (ex > maxex)
			maxex = ex;
		    }
		  else
		    {
		      // printf("OTHER SIG cr %d ex %d\n", cr, ex);
		    }
		}
	    }
	}
      if (tag == 13)
	{
	  userid = sat_realloc(userid, l);
	  if (l)
	    memcpy(userid, p, l);
	  useridl = l;
	}
    }
  if (maxex)
    repodata_set_num(data, s - s->repo->pool->solvables, PUBKEY_EXPIRES, maxex);
  sat_free(pubkey);
  sat_free(userid);
}

/* this is private to rpm, but rpm lacks an interface to retrieve
 * the values. Sigh. */
struct pgpDigParams_s {
    const char * userid;
    const unsigned char * hash;
    const char * params[4];
    unsigned char tag;
    unsigned char version;               /*!< version number. */
    unsigned char time[4];               /*!< time that the key was created. */
    unsigned char pubkey_algo;           /*!< public key algorithm. */
    unsigned char hash_algo;
    unsigned char sigtype;
    unsigned char hashlen;
    unsigned char signhash16[2];
    unsigned char signid[8];
    unsigned char saved;
};

struct pgpDig_s {
    struct pgpDigParams_s signature;
    struct pgpDigParams_s pubkey;
};

static int
pubkey2solvable(Solvable *s, Repodata *data, char *pubkey)
{
  Pool *pool = s->repo->pool;
  unsigned char *pkts;
  unsigned int btime;
  int pktsl, i;
  pgpDig dig = 0;
  char keyid[16 + 1];
  char evrbuf[8 + 1 + 8 + 1];

  pkts = unarmor(pubkey, &pktsl);
  if (!pkts)
    return 0;
  setutf8string(data, s - s->repo->pool->solvables, SOLVABLE_DESCRIPTION, pubkey);
  parsekeydata(s, data, pkts, pktsl);
  /* only rpm knows how to do the release calculation, we don't dare
   * to recreate all the bugs */
  dig = pgpNewDig();
  (void) pgpPrtPkts(pkts, pktsl, dig, 0);
  btime = dig->pubkey.time[0] << 24 | dig->pubkey.time[1] << 16 | dig->pubkey.time[2] << 8 | dig->pubkey.signid[3];
  sprintf(evrbuf, "%02x%02x%02x%02x-%02x%02x%02x%02x", dig->pubkey.signid[4], dig->pubkey.signid[5], dig->pubkey.signid[6], dig->pubkey.signid[7], dig->pubkey.time[0], dig->pubkey.time[1], dig->pubkey.time[2], dig->pubkey.time[3]);
  repodata_set_num(data, s - s->repo->pool->solvables, SOLVABLE_BUILDTIME, btime);

  s->name = str2id(pool, "gpg-pubkey", 1);
  s->evr = str2id(pool, evrbuf, 1);
  s->arch = 1;
  for (i = 0; i < 8; i++)
    sprintf(keyid + 2 * i, "%02x", dig->pubkey.signid[i]);
  repodata_set_str(data, s - s->repo->pool->solvables, PUBKEY_KEYID, keyid);
  if (dig->pubkey.userid)
    setutf8string(data, s - s->repo->pool->solvables, SOLVABLE_SUMMARY, dig->pubkey.userid);
  pgpFreeDig(dig);
  sat_free((void *)pkts);
  return 1;
}

void
repo_add_rpmdb_pubkeys(Repo *repo, const char *rootdir, int flags)
{
  Pool *pool = repo->pool;
  struct rpm_by_state state;
  struct rpmdbentry *entries;
  int nentries, i;
  char *namedata, *str;
  unsigned int u32;
  Repodata *data;
  Solvable *s;

  data = repo_add_repodata(repo, flags);

  memset(&state, 0, sizeof(state));
  if (!(state.dbenv = opendbenv(rootdir)))
    return;
  entries = getinstalledrpmdbids(&state, "Name", "gpg-pubkey", &nentries, &namedata);
  for (i = 0 ; i < nentries; i++)
    {
      void *statep = &state;
      RpmHead *rpmhead = rpm_byrpmdbid(entries[i].rpmdbid, rootdir, &statep);
      if (!rpmhead)
	continue;
      str = headstring(rpmhead, TAG_DESCRIPTION);
      if (!str)
	continue;
      s = pool_id2solvable(pool, repo_add_solvable(repo));
      pubkey2solvable(s, data, str);
      u32 = headint32(rpmhead, TAG_INSTALLTIME);
      if (u32)
        repodata_set_num(data, s - pool->solvables, SOLVABLE_INSTALLTIME, u32);
      if (!repo->rpmdbid)
	repo->rpmdbid = repo_sidedata_create(repo, sizeof(Id));
      repo->rpmdbid[s - pool->solvables - repo->start] = entries[i].rpmdbid;
    }
  freestate(&state);
  if (!(flags & REPO_NO_INTERNALIZE))
    repodata_internalize(data);
}

void
repo_add_pubkeys(Repo *repo, const char **keys, int nkeys, int flags)
{
  Pool *pool = repo->pool;
  Repodata *data;
  Solvable *s;
  char *buf;
  int i, bufl, l, ll;
  FILE *fp;

  data = repo_add_repodata(repo, flags);
  buf = 0;
  bufl = 0;
  for (i = 0; i < nkeys; i++)
    {
      if ((fp = fopen(keys[i], "r")) == 0)
	{
	  perror(keys[i]);
	  continue;
	}
      for (l = 0; ;)
	{
	  if (bufl - l < 4096)
	    {
	      bufl += 4096;
	      buf = sat_realloc(buf, bufl);
	    }
	  ll = fread(buf, 1, bufl - l, fp);
	  if (ll <= 0)
	    break;
	  l += ll;
	}
      buf[l] = 0;
      fclose(fp);
      s = pool_id2solvable(pool, repo_add_solvable(repo));
      pubkey2solvable(s, data, buf);
    }
  sat_free(buf);
  if (!(flags & REPO_NO_INTERNALIZE))
    repodata_internalize(data);
}
