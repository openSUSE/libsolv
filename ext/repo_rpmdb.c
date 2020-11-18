/*
 * Copyright (c) 2007-2018, SUSE Inc.
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
#include <stdint.h>
#include <errno.h>

#ifdef ENABLE_RPMDB

#include <rpm/rpmio.h>
#include <rpm/rpmpgp.h>
#ifndef RPM5
#include <rpm/header.h>
#endif
#include <rpm/rpmdb.h>

#endif

#include "pool.h"
#include "repo.h"
#include "hash.h"
#include "util.h"
#include "queue.h"
#include "chksum.h"
#include "repo_rpmdb.h"
#include "repo_solv.h"
#ifdef ENABLE_COMPLEX_DEPS
#include "pool_parserpmrichdep.h"
#endif

/* 3: added triggers */
/* 4: fixed triggers */
/* 5: fixed checksum copying */
/* 6: add SOLVABLE_PREREQ_IGNOREINST support */
/* 7: fix bug in ignoreinst logic */
#define RPMDB_COOKIE_VERSION 7

#define TAG_NAME		1000
#define TAG_VERSION		1001
#define TAG_RELEASE		1002
#define TAG_EPOCH		1003
#define TAG_SUMMARY		1004
#define TAG_DESCRIPTION		1005
#define TAG_BUILDTIME		1006
#define TAG_BUILDHOST		1007
#define TAG_INSTALLTIME		1008
#define TAG_SIZE		1009
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
#define TAG_FILEFLAGS		1037
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
#define TAG_CHANGELOGTIME	1080
#define TAG_CHANGELOGNAME	1081
#define TAG_CHANGELOGTEXT	1082
#define TAG_OBSOLETENAME	1090
#define TAG_FILEDEVICES		1095
#define TAG_FILEINODES		1096
#define TAG_SOURCEPACKAGE	1106
#define TAG_PROVIDEFLAGS	1112
#define TAG_PROVIDEVERSION	1113
#define TAG_OBSOLETEFLAGS	1114
#define TAG_OBSOLETEVERSION	1115
#define TAG_DIRINDEXES		1116
#define TAG_BASENAMES		1117
#define TAG_DIRNAMES		1118
#define TAG_PAYLOADFORMAT	1124
#define TAG_PATCHESNAME		1133
#define TAG_FILECOLORS		1140
#define TAG_OLDSUGGESTSNAME	1156
#define TAG_OLDSUGGESTSVERSION	1157
#define TAG_OLDSUGGESTSFLAGS	1158
#define TAG_OLDENHANCESNAME	1159
#define TAG_OLDENHANCESVERSION	1160
#define TAG_OLDENHANCESFLAGS	1161

/* rpm5 tags */
#define TAG_DISTEPOCH		1218

/* rpm4 tags */
#define TAG_LONGFILESIZES	5008
#define TAG_LONGSIZE		5009
#define TAG_RECOMMENDNAME	5046
#define TAG_RECOMMENDVERSION	5047
#define TAG_RECOMMENDFLAGS	5048
#define TAG_SUGGESTNAME		5049
#define TAG_SUGGESTVERSION	5050
#define TAG_SUGGESTFLAGS	5051
#define TAG_SUPPLEMENTNAME	5052
#define TAG_SUPPLEMENTVERSION	5053
#define TAG_SUPPLEMENTFLAGS	5054
#define TAG_ENHANCENAME		5055
#define TAG_ENHANCEVERSION	5056
#define TAG_ENHANCEFLAGS	5057

/* signature tags */
#define	TAG_SIGBASE		256
#define TAG_SIGMD5		(TAG_SIGBASE + 5)
#define TAG_SHA1HEADER		(TAG_SIGBASE + 13)
#define TAG_SHA256HEADER	(TAG_SIGBASE + 17)

#define SIGTAG_SIZE		1000
#define SIGTAG_PGP		1002	/* RSA signature */
#define SIGTAG_MD5		1004	/* header+payload md5 checksum */
#define SIGTAG_GPG		1005	/* DSA signature */

#define DEP_LESS		(1 << 1)
#define DEP_GREATER		(1 << 2)
#define DEP_EQUAL		(1 << 3)
#define DEP_STRONG		(1 << 27)
#define DEP_PRE_IN		((1 << 6) | (1 << 9) | (1 << 10))
#define DEP_PRE_UN		((1 << 6) | (1 << 11) | (1 << 12))

#define FILEFLAG_GHOST		(1 << 6)


/* some limits to guard against corrupt rpms */
/* dsize limits taken from rpm's lib/header.c */
#define MAX_SIG_CNT		0x10000
#define MAX_SIG_DSIZE		0x4000000

#define MAX_HDR_CNT		0x10000
#define MAX_HDR_DSIZE		0x10000000


#ifndef ENABLE_RPMPKG_LIBRPM

typedef struct rpmhead {
  int cnt;
  unsigned int dcnt;
  unsigned char *dp;
  unsigned char data[1];
} RpmHead;


static inline void
headinit(RpmHead *h, unsigned int cnt, unsigned int dcnt)
{
  h->cnt = (int)cnt;
  h->dcnt = dcnt;
  h->dp = h->data + 16 * cnt;
  h->dp[dcnt] = 0;
}

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

static uint32_t *
headint32array(RpmHead *h, int tag, int *cnt)
{
  uint32_t *r;
  unsigned int i, o;
  unsigned char *d = headfindtag(h, tag);

  if (!d || d[4] != 0 || d[5] != 0 || d[6] != 0 || d[7] != 4)
    return 0;
  o = d[8] << 24 | d[9] << 16 | d[10] << 8 | d[11];
  i = d[12] << 24 | d[13] << 16 | d[14] << 8 | d[15];
  if (o > h->dcnt || i > h->dcnt || o + 4 * i > h->dcnt)
    return 0;
  d = h->dp + o;
  r = solv_calloc(i ? i : 1, sizeof(uint32_t));
  if (cnt)
    *cnt = i;
  for (o = 0; o < i; o++, d += 4)
    r[o] = d[0] << 24 | d[1] << 16 | d[2] << 8 | d[3];
  return r;
}

/* returns the first entry of an integer array */
static uint32_t
headint32(RpmHead *h, int tag)
{
  unsigned int i, o;
  unsigned char *d = headfindtag(h, tag);

  if (!d || d[4] != 0 || d[5] != 0 || d[6] != 0 || d[7] != 4)
    return 0;
  o = d[8] << 24 | d[9] << 16 | d[10] << 8 | d[11];
  i = d[12] << 24 | d[13] << 16 | d[14] << 8 | d[15];
  if (i == 0 || o > h->dcnt || i > h->dcnt || o + 4 * i > h->dcnt)
    return 0;
  d = h->dp + o;
  return d[0] << 24 | d[1] << 16 | d[2] << 8 | d[3];
}

static uint64_t *
headint64array(RpmHead *h, int tag, int *cnt)
{
  uint64_t *r;
  unsigned int i, o;
  unsigned char *d = headfindtag(h, tag);

  if (!d || d[4] != 0 || d[5] != 0 || d[6] != 0 || d[7] != 5)
    return 0;
  o = d[8] << 24 | d[9] << 16 | d[10] << 8 | d[11];
  i = d[12] << 24 | d[13] << 16 | d[14] << 8 | d[15];
  if (o > h->dcnt || i > h->dcnt || o + 8 * i > h->dcnt)
    return 0;
  d = h->dp + o;
  r = solv_calloc(i ? i : 1, sizeof(uint64_t));
  if (cnt)
    *cnt = i;
  for (o = 0; o < i; o++, d += 8)
    {
      uint32_t x = d[0] << 24 | d[1] << 16 | d[2] << 8 | d[3];
      r[o] = (uint64_t)x << 32 | (uint32_t)(d[4] << 24 | d[5] << 16 | d[6] << 8 | d[7]);
    }
  return r;
}

/* returns the first entry of an 64bit integer array */
static uint64_t
headint64(RpmHead *h, int tag)
{
  uint32_t x;
  unsigned int i, o;
  unsigned char *d = headfindtag(h, tag);

  if (!d || d[4] != 0 || d[5] != 0 || d[6] != 0 || d[7] != 5)
    return 0;
  o = d[8] << 24 | d[9] << 16 | d[10] << 8 | d[11];
  i = d[12] << 24 | d[13] << 16 | d[14] << 8 | d[15];
  if (i == 0 || o > h->dcnt || i > h->dcnt || o + 8 * i > h->dcnt)
    return 0;
  d = h->dp + o;
  x = d[0] << 24 | d[1] << 16 | d[2] << 8 | d[3];
  return (uint64_t)x << 32 | (uint32_t)(d[4] << 24 | d[5] << 16 | d[6] << 8 | d[7]);
}

static uint16_t *
headint16array(RpmHead *h, int tag, int *cnt)
{
  uint16_t *r;
  unsigned int i, o;
  unsigned char *d = headfindtag(h, tag);

  if (!d || d[4] != 0 || d[5] != 0 || d[6] != 0 || d[7] != 3)
    return 0;
  o = d[8] << 24 | d[9] << 16 | d[10] << 8 | d[11];
  i = d[12] << 24 | d[13] << 16 | d[14] << 8 | d[15];
  if (o > h->dcnt || i > h->dcnt || o + 2 * i > h->dcnt)
    return 0;
  d = h->dp + o;
  r = solv_calloc(i ? i : 1, sizeof(uint16_t));
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
  if (o > h->dcnt || i > h->dcnt)
    return 0;
  r = solv_calloc(i ? i : 1, sizeof(char *));
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
          solv_free(r);
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
  if (o > h->dcnt || i > h->dcnt || o + i > h->dcnt)
    return 0;
  if (sizep)
    *sizep = i;
  return h->dp + o;
}

static int
headissourceheuristic(RpmHead *h)
{
  unsigned int i, o;
  unsigned char *d = headfindtag(h, TAG_DIRNAMES);
  if (!d || d[4] != 0 || d[5] != 0 || d[6] != 0 || d[7] != 8)
    return 0;
  o = d[8] << 24 | d[9] << 16 | d[10] << 8 | d[11];
  i = d[12] << 24 | d[13] << 16 | d[14] << 8 | d[15];
  return i == 1 && o < h->dcnt && !h->dp[o] ? 1 : 0;
}

static inline void
headfree(RpmHead *h)
{
  solv_free(h);
}

#else

typedef struct headerToken_s RpmHead;

static int
headexists(RpmHead *h, int tag)
{
  return headerIsEntry(h, tag);
}

static void *headget(RpmHead *h, int tag, int *cnt, int alloc)
{
  struct rpmtd_s td;
  if (!headerGet(h, tag, &td, alloc ? HEADERGET_ALLOC : HEADERGET_MINMEM))
    return 0;
  if (cnt)
    *cnt = td.count;
  return td.data;
}

static uint32_t *
headint32array(RpmHead *h, int tag, int *cnt)
{
  return headget(h, tag, cnt, 1);
}

static uint32_t
headint32(RpmHead *h, int tag)
{
  uint32_t *arr = headget(h, tag, 0, 0);
  return arr ? arr[0] : 0;
}

static uint64_t *
headint64array(RpmHead *h, int tag, int *cnt)
{
  return headget(h, tag, cnt, 1);
}

/* returns the first entry of an 64bit integer array */
static uint64_t
headint64(RpmHead *h, int tag)
{
  uint64_t *arr = headget(h, tag, 0, 0);
  return arr ? arr[0] : 0;
}

static uint16_t *
headint16array(RpmHead *h, int tag, int *cnt)
{
  return headget(h, tag, cnt, 1);
}

static char *
headstring(RpmHead *h, int tag)
{
  return headget(h, tag, 0, 0);
}

static char **
headstringarray(RpmHead *h, int tag, int *cnt)
{
  return headget(h, tag, cnt, 1);
}

static unsigned char *
headbinary(RpmHead *h, int tag, unsigned int *sizep)
{
  unsigned char *b = headget(h, tag, (int *)sizep, 0);
  if (b && sizep && (tag == TAG_SIGMD5 || tag == SIGTAG_MD5) && *sizep > 16) {
    /* due to a bug in rpm the count may be bigger if HEADERIMPORT_FAST is used */
    *sizep = 16;
  }
  return b;
}

static int
headissourceheuristic(RpmHead *h)
{
  struct rpmtd_s td;
  int issource;
  if (!headerGet(h, TAG_DIRNAMES, &td, HEADERGET_MINMEM))
    return 0;
  issource = td.count == 1 && td.data && ((char **)td.data)[0] && !((char **)td.data)[0][0];
  rpmtdFreeData(&td);
  return issource;
}

static inline void
headfree(RpmHead *h)
{
  headerFree(h);
}

#endif

static char *headtoevr(RpmHead *h)
{
  unsigned int epoch;
  char *version, *v;
  char *release;
  char *evr;
  char *distepoch;

  version  = headstring(h, TAG_VERSION);
  release  = headstring(h, TAG_RELEASE);
  epoch = headint32(h, TAG_EPOCH);
  if (!version || !release)
    return 0;
  for (v = version; *v >= '0' && *v <= '9'; v++)
    ;
  if (epoch || (v != version && *v == ':'))
    {
      char epochbuf[11];        /* 32bit decimal will fit in */
      sprintf(epochbuf, "%u", epoch);
      evr = solv_malloc(strlen(epochbuf) + 1 + strlen(version) + 1 + strlen(release) + 1);
      sprintf(evr, "%s:%s-%s", epochbuf, version, release);
    }
  else
    {
      evr = solv_malloc(strlen(version) + 1 + strlen(release) + 1);
      sprintf(evr, "%s-%s", version, release);
    }
  distepoch = headstring(h, TAG_DISTEPOCH);
  if (distepoch && *distepoch)
    {
      int l = strlen(evr);
      evr = solv_realloc(evr, l + strlen(distepoch) + 2);
      evr[l++] = ':';
      strcpy(evr + l, distepoch);
    }
  return evr;
}


static void
setutf8string(Repodata *repodata, Id handle, Id tag, const char *str)
{
  if (str[solv_validutf8(str)])
    {
      char *ustr = solv_latin1toutf8(str);	/* not utf8, assume latin1 */
      repodata_set_str(repodata, handle, tag, ustr);
      solv_free(ustr);
    }
  else
    repodata_set_str(repodata, handle, tag, str);
}

static int
ignq_sortcmp(const void *va, const void *vb, void *dp)
{
  int r = *(Id *)va - *(Id *)vb;
  if (!r)
    r = ((Id *)va)[1] - ((Id *)vb)[1];
  return r;
}

/*
 * strong: 0: ignore strongness
 *         1: filter to strong
 *         2: filter to weak
 */
static unsigned int
makedeps(Pool *pool, Repo *repo, RpmHead *rpmhead, int tagn, int tagv, int tagf, int flags, Queue *ignq)
{
  char **n, **v;
  uint32_t *f;
  int i, cc, nc, vc, fc;
  int haspre, premask, has_ign;
  unsigned int olddeps;
  Id *ida;
  int strong = 0;

  n = headstringarray(rpmhead, tagn, &nc);
  if (!n)
    {
      switch (tagn)
	{
	case TAG_SUGGESTNAME:
	  tagn = TAG_OLDSUGGESTSNAME;
	  tagv = TAG_OLDSUGGESTSVERSION;
	  tagf = TAG_OLDSUGGESTSFLAGS;
	  strong = -1;
	  break;
	case TAG_ENHANCENAME:
	  tagn = TAG_OLDENHANCESNAME;
	  tagv = TAG_OLDENHANCESVERSION;
	  tagf = TAG_OLDENHANCESFLAGS;
	  strong = -1;
	  break;
	case TAG_RECOMMENDNAME:
	  tagn = TAG_OLDSUGGESTSNAME;
	  tagv = TAG_OLDSUGGESTSVERSION;
	  tagf = TAG_OLDSUGGESTSFLAGS;
	  strong = 1;
	  break;
	case TAG_SUPPLEMENTNAME:
	  tagn = TAG_OLDENHANCESNAME;
	  tagv = TAG_OLDENHANCESVERSION;
	  tagf = TAG_OLDENHANCESFLAGS;
	  strong = 1;
	  break;
	default:
	  return 0;
	}
      n = headstringarray(rpmhead, tagn, &nc);
    }
  if (!n || !nc)
    return 0;
  vc = fc = 0;
  v = headstringarray(rpmhead, tagv, &vc);
  f = headint32array(rpmhead, tagf, &fc);
  if (!v || !f || nc != vc || nc != fc)
    {
      char *pkgname = rpm_query(rpmhead, 0);
      pool_error(pool, 0, "bad dependency entries for %s: %d %d %d", pkgname ? pkgname : "<NULL>", nc, vc, fc);
      solv_free(pkgname);
      solv_free(n);
      solv_free(v);
      solv_free(f);
      return 0;
    }

  cc = nc;
  haspre = 0;	/* add no prereq marker */
  premask = tagn == TAG_REQUIRENAME ? DEP_PRE_IN | DEP_PRE_UN : 0;
  if ((flags & RPM_ADD_NO_RPMLIBREQS) || strong)
    {
      /* we do filtering */
      cc = 0;
      for (i = 0; i < nc; i++)
	{
	  if (strong && (f[i] & DEP_STRONG) != (strong < 0 ? 0 : DEP_STRONG))
	    continue;
	  if ((flags & RPM_ADD_NO_RPMLIBREQS) != 0)
	    if (!strncmp(n[i], "rpmlib(", 7))
	      continue;
	  if ((f[i] & premask) != 0)
	    haspre = 1;
	  cc++;
	}
    }
  else if (premask)
    {
      /* no filtering, just look for the first prereq */
      for (i = 0; i < nc; i++)
	if ((f[i] & premask) != 0)
	  {
	    haspre = 1;
	    break;
	  }
    }
  if (cc == 0)
    {
      solv_free(n);
      solv_free(v);
      solv_free(f);
      return 0;
    }
  cc += haspre;		/* add slot for the prereq marker */
  olddeps = repo_reserve_ids(repo, 0, cc);
  ida = repo->idarraydata + olddeps;

  has_ign = 0;
  for (i = 0; ; i++)
    {
      Id id;
      if (i == nc)
	{
	  if (haspre != 1)
	    break;
	  haspre = 2;	/* pass two: prereqs */
	  i = 0;
	  *ida++ = SOLVABLE_PREREQMARKER;
	}
      if (strong && (f[i] & DEP_STRONG) != (strong < 0 ? 0 : DEP_STRONG))
	continue;
      if (haspre)
	{
	  if (haspre == 1 && (f[i] & premask) != 0)
	    continue;
	  if (haspre == 2 && (f[i] & premask) == 0)
	    continue;
	}
      if ((flags & RPM_ADD_NO_RPMLIBREQS) != 0)
	if (!strncmp(n[i], "rpmlib(", 7))
	  continue;
#ifdef ENABLE_COMPLEX_DEPS
      if ((f[i] & (DEP_LESS|DEP_EQUAL|DEP_GREATER)) == 0 && n[i][0] == '(')
	{
	  id = pool_parserpmrichdep(pool, n[i]);
	  if (id)
	    *ida++ = id;
	  else
	    cc--;
	  continue;
	}
#endif
      id = pool_str2id(pool, n[i], 1);
      if (f[i] & (DEP_LESS|DEP_GREATER|DEP_EQUAL))
	{
	  Id evr;
	  int fl = 0;
	  if ((f[i] & DEP_LESS) != 0)
	    fl |= REL_LT;
	  if ((f[i] & DEP_EQUAL) != 0)
	    fl |= REL_EQ;
	  if ((f[i] & DEP_GREATER) != 0)
	    fl |= REL_GT;
	  if (v[i][0] == '0' && v[i][1] == ':' && v[i][2])
	    evr = pool_str2id(pool, v[i] + 2, 1);
	  else
	    evr = pool_str2id(pool, v[i], 1);
	  id = pool_rel2id(pool, id, evr, fl, 1);
	}
      *ida++ = id;
      if (haspre == 2 && ignq)
	{
	  int is_ign = (f[i] & DEP_PRE_IN) != 0 && (f[i] & DEP_PRE_UN) == 0 ? 1 : 0;
	  has_ign |= is_ign;
	  queue_push2(ignq, id, is_ign);
	}
    }
  *ida++ = 0;
  repo->idarraysize += cc + 1;
  solv_free(n);
  solv_free(v);
  solv_free(f);
  if (ignq && ignq->count)
    {
      int j = 0;
      if (has_ign && ignq->count == 2)
	j = 1;
      else if (has_ign)
	{
	  Id id, lastid = 0;

	  solv_sort(ignq->elements, ignq->count / 2, sizeof(Id) * 2, ignq_sortcmp, 0);
	  for (i = j = 0; i < ignq->count; i += 2)
	    {
	      id = ignq->elements[i];
	      if (id != lastid && ignq->elements[i + 1] > 0)
		ignq->elements[j++] = id;
	      lastid = id;
	    }
	}
      queue_truncate(ignq, j);
    }
  return olddeps;
}

static Id
repodata_str2dir_rooted(Repodata *data, char *str, int create)
{
  char buf[256], *bp;
  int l = strlen(str);
  Id id;

  if (l + 2 <= sizeof(buf))
    bp = buf;
  else
    bp = solv_malloc(l + 2);
  bp[0] = '/';
  strcpy(bp + 1, str);
  id = repodata_str2dir(data, bp, create);
  if (bp != buf)
    solv_free(bp);
  return id;
}

static void
adddudata(Repodata *data, Id handle, RpmHead *rpmhead, char **dn, uint32_t *di, int fc, int dc)
{
  Id did;
  int i, fszc;
  unsigned int *fkb, *fn;
  uint64_t *fsz64;
  uint32_t *fsz, *fino;
  uint16_t *fm;
  unsigned int inotest[256], inotestok;

  if (!fc)
    return;
  if ((fsz64 = headint64array(rpmhead, TAG_LONGFILESIZES, &fszc)) != 0)
    {
      /* convert to kbyte */
      fsz = solv_malloc2(fszc, sizeof(*fsz));
      for (i = 0; i < fszc; i++)
        fsz[i] = fsz64[i] ? fsz64[i] / 1024 + 1 : 0;
      solv_free(fsz64);
    }
  else if ((fsz = headint32array(rpmhead, TAG_FILESIZES, &fszc)) != 0)
    {
      /* convert to kbyte */
      for (i = 0; i < fszc; i++)
        if (fsz[i])
	  fsz[i] = fsz[i] / 1024 + 1;
    }
  else
    return;
  if (fc != fszc)
    {
      solv_free(fsz);
      return;
    }

  /* stupid rpm records sizes of directories, so we have to check the mode */
  fm = headint16array(rpmhead, TAG_FILEMODES, &fszc);
  if (!fm || fc != fszc)
    {
      solv_free(fsz);
      solv_free(fm);
      return;
    }
  fino = headint32array(rpmhead, TAG_FILEINODES, &fszc);
  if (!fino || fc != fszc)
    {
      solv_free(fsz);
      solv_free(fm);
      solv_free(fino);
      return;
    }

  /* kill hardlinked entries */
  inotestok = 0;
  if (fc < sizeof(inotest))
    {
      /* quick test just hashing the inode numbers */
      memset(inotest, 0, sizeof(inotest));
      for (i = 0; i < fc; i++)
	{
	  int off, bit;
	  if (fsz[i] == 0 || !S_ISREG(fm[i]))
	    continue;	/* does not matter */
	  off = (fino[i] >> 5) & (sizeof(inotest)/sizeof(*inotest) - 1);
	  bit = 1 << (fino[i] & 31);
	  if ((inotest[off] & bit) != 0)
	    break;
	  inotest[off] |= bit;
	}
      if (i == fc)
	inotestok = 1;	/* no conflict found */
    }
  if (!inotestok)
    {
      /* hardlinked files are possible, check ino/dev pairs */
      unsigned int *fdev = headint32array(rpmhead, TAG_FILEDEVICES, &fszc);
      unsigned int *fx, j;
      unsigned int mask, hash, hh;
      if (!fdev || fc != fszc)
	{
	  solv_free(fsz);
	  solv_free(fm);
	  solv_free(fdev);
	  solv_free(fino);
	  return;
	}
      mask = fc;
      while ((mask & (mask - 1)) != 0)
	mask = mask & (mask - 1);
      mask <<= 2;
      if (mask > sizeof(inotest)/sizeof(*inotest))
        fx = solv_calloc(mask, sizeof(unsigned int));
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
        solv_free(fx);
      solv_free(fdev);
    }
  solv_free(fino);

  /* sum up inode count and kbytes for each directory */
  fn = solv_calloc(dc, sizeof(unsigned int));
  fkb = solv_calloc(dc, sizeof(unsigned int));
  for (i = 0; i < fc; i++)
    {
      if (di[i] >= dc)
	continue;	/* corrupt entry */
      fn[di[i]]++;
      if (fsz[i] == 0 || !S_ISREG(fm[i]))
	continue;
      fkb[di[i]] += fsz[i];
    }
  solv_free(fsz);
  solv_free(fm);
  /* commit */
  for (i = 0; i < dc; i++)
    {
      if (!fn[i])
	continue;
      if (dn[i][0] != '/')
	{
          Solvable *s = data->repo->pool->solvables + handle;
          if (s->arch == ARCH_SRC || s->arch == ARCH_NOSRC)
	    did = repodata_str2dir(data, "/usr/src", 1);
	  else
	    did = repodata_str2dir_rooted(data, dn[i], 1);
	}
      else
        did = repodata_str2dir(data, dn[i], 1);
      repodata_add_dirnumnum(data, handle, SOLVABLE_DISKUSAGE, did, fkb[i], fn[i]);
    }
  solv_free(fn);
  solv_free(fkb);
}

static int
is_filtered(const char *dir)
{
  if (!dir)
    return 1;
  /* the dirs always have a trailing / in rpm */
  if (strstr(dir, "bin/"))
    return 0;
  if (!strncmp(dir, "/etc/", 5))
    return 0;
  if (!strcmp(dir, "/usr/lib/"))
    return 2;
  return 1;
}

static void
addfilelist(Repodata *data, Id handle, RpmHead *rpmhead, int flags)
{
  char **bn;
  char **dn;
  uint32_t *di;
  int bnc, dnc, dic;
  int i;
  Id did;
  uint32_t lastdii = -1;
  int lastfiltered = 0;

  if (!data)
    return;
  bn = headstringarray(rpmhead, TAG_BASENAMES, &bnc);
  if (!bn)
    return;
  dn = headstringarray(rpmhead, TAG_DIRNAMES, &dnc);
  if (!dn)
    {
      solv_free(bn);
      return;
    }
  di = headint32array(rpmhead, TAG_DIRINDEXES, &dic);
  if (!di)
    {
      solv_free(bn);
      solv_free(dn);
      return;
    }
  if (bnc != dic)
    {
      pool_error(data->repo->pool, 0, "bad filelist");
      return;
    }

  adddudata(data, handle, rpmhead, dn, di, bnc, dnc);

  did = -1;
  for (i = 0; i < bnc; i++)
    {
      char *b = bn[i];

      if (did < 0 || di[i] != lastdii)
	{
	  if (di[i] >= dnc)
	    continue;	/* corrupt entry */
	  did = 0;
	  lastdii = di[i];
	  if ((flags & RPM_ADD_FILTERED_FILELIST) != 0)
	    {
	      lastfiltered = is_filtered(dn[di[i]]);
	      if (lastfiltered == 1)
		continue;
	    }
	  if (dn[lastdii][0] != '/')
	    did = repodata_str2dir_rooted(data, dn[lastdii], 1);
	  else
	    did = repodata_str2dir(data, dn[lastdii], 1);
	}
      if (!b)
	continue;
      if (*b == '/')	/* work around rpm bug */
	b++;
      if (lastfiltered && (lastfiltered != 2 || strcmp(b, "sendmail")))
        continue;
      repodata_add_dirstr(data, handle, SOLVABLE_FILELIST, did, b);
    }
  solv_free(bn);
  solv_free(dn);
  solv_free(di);
}

static void
addchangelog(Repodata *data, Id handle, RpmHead *rpmhead)
{
  char **cn;
  char **cx;
  uint32_t *ct;
  int i, cnc, cxc, ctc = 0;
  Queue hq;

  ct = headint32array(rpmhead, TAG_CHANGELOGTIME, &ctc);
  cx = headstringarray(rpmhead, TAG_CHANGELOGTEXT, &cxc);
  cn = headstringarray(rpmhead, TAG_CHANGELOGNAME, &cnc);
  if (!ct || !cx || !cn || !ctc || ctc != cxc || ctc != cnc)
    {
      solv_free(ct);
      solv_free(cx);
      solv_free(cn);
      return;
    }
  queue_init(&hq);
  for (i = 0; i < ctc; i++)
    {
      Id h = repodata_new_handle(data);
      if (ct[i])
        repodata_set_num(data, h, SOLVABLE_CHANGELOG_TIME, ct[i]);
      if (cn[i])
        setutf8string(data, h, SOLVABLE_CHANGELOG_AUTHOR, cn[i]);
      if (cx[i])
        setutf8string(data, h, SOLVABLE_CHANGELOG_TEXT, cx[i]);
      queue_push(&hq, h);
    }
  for (i = 0; i < hq.count; i++)
    repodata_add_flexarray(data, handle, SOLVABLE_CHANGELOG, hq.elements[i]);
  queue_free(&hq);
  solv_free(ct);
  solv_free(cx);
  solv_free(cn);
}

static void
set_description_author(Repodata *data, Id handle, char *str)
{
  char *aut, *p;
  for (aut = str; (aut = strchr(aut, '\n')) != 0; aut++)
    if (!strncmp(aut, "\nAuthors:\n--------\n", 19))
      break;
  if (aut)
    {
      /* oh my, found SUSE special author section */
      int l = aut - str;
      str = solv_strdup(str);
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

static int
rpmhead2solv(Pool *pool, Repo *repo, Repodata *data, Solvable *s, RpmHead *rpmhead, int flags)
{
  char *name;
  char *evr;
  char *sourcerpm;
  Queue ignq;
  Id ignqbuf[64];

  name = headstring(rpmhead, TAG_NAME);
  if (!name)
    {
      pool_error(pool, 0, "package has no name");
      return 0;
    }
  if (!(flags & RPMDB_KEEP_GPG_PUBKEY) && !strcmp(name, "gpg-pubkey"))
    return 0;
  s->name = pool_str2id(pool, name, 1);
  sourcerpm = headstring(rpmhead, TAG_SOURCERPM);
  if (sourcerpm || !(headexists(rpmhead, TAG_SOURCEPACKAGE) || headissourceheuristic(rpmhead)))
    s->arch = pool_str2id(pool, headstring(rpmhead, TAG_ARCH), 1);
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
  s->evr = pool_str2id(pool, evr, 1);
  solv_free(evr);
  s->vendor = pool_str2id(pool, headstring(rpmhead, TAG_VENDOR), 1);

  queue_init_buffer(&ignq, ignqbuf, sizeof(ignqbuf)/sizeof(*ignqbuf));

  s->provides = makedeps(pool, repo, rpmhead, TAG_PROVIDENAME, TAG_PROVIDEVERSION, TAG_PROVIDEFLAGS, 0, 0);
  if (s->arch != ARCH_SRC && s->arch != ARCH_NOSRC)
    s->provides = repo_addid_dep(repo, s->provides, pool_rel2id(pool, s->name, s->evr, REL_EQ, 1), 0);
  s->requires = makedeps(pool, repo, rpmhead, TAG_REQUIRENAME, TAG_REQUIREVERSION, TAG_REQUIREFLAGS, flags, &ignq);
  s->conflicts = makedeps(pool, repo, rpmhead, TAG_CONFLICTNAME, TAG_CONFLICTVERSION, TAG_CONFLICTFLAGS, 0, 0);
  s->obsoletes = makedeps(pool, repo, rpmhead, TAG_OBSOLETENAME, TAG_OBSOLETEVERSION, TAG_OBSOLETEFLAGS, 0, 0);

  s->recommends = makedeps(pool, repo, rpmhead, TAG_RECOMMENDNAME, TAG_RECOMMENDVERSION, TAG_RECOMMENDFLAGS, 0, 0);
  s->suggests = makedeps(pool, repo, rpmhead, TAG_SUGGESTNAME, TAG_SUGGESTVERSION, TAG_SUGGESTFLAGS, 0, 0);
  s->supplements = makedeps(pool, repo, rpmhead, TAG_SUPPLEMENTNAME, TAG_SUPPLEMENTVERSION, TAG_SUPPLEMENTFLAGS, 0, 0);
  s->enhances  = makedeps(pool, repo, rpmhead, TAG_ENHANCENAME, TAG_ENHANCEVERSION, TAG_ENHANCEFLAGS, 0, 0);

  repo_rewrite_suse_deps(s, 0);

  if (data && ignq.count)
    repodata_set_idarray(data, s - pool->solvables, SOLVABLE_PREREQ_IGNOREINST, &ignq);
  queue_free(&ignq);

  if (data)
    {
      Id handle;
      char *str;
      unsigned int u32;
      unsigned long long u64;

      handle = s - pool->solvables;
      str = headstring(rpmhead, TAG_SUMMARY);
      if (str)
        setutf8string(data, handle, SOLVABLE_SUMMARY, str);
      str = headstring(rpmhead, TAG_DESCRIPTION);
      if (str)
	set_description_author(data, handle, str);
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
      if ((flags & RPM_ADD_WITH_PKGID) != 0)
	{
	  unsigned char *chksum;
	  unsigned int chksumsize;
	  chksum = headbinary(rpmhead, TAG_SIGMD5, &chksumsize);
	  if (chksum && chksumsize == 16)
	    repodata_set_bin_checksum(data, handle, SOLVABLE_PKGID, REPOKEY_TYPE_MD5, chksum);
	}
      if ((flags & RPM_ADD_WITH_HDRID) != 0)
	{
	  str = headstring(rpmhead, TAG_SHA1HEADER);
	  if (str && strlen(str) == 40)
	    repodata_set_checksum(data, handle, SOLVABLE_HDRID, REPOKEY_TYPE_SHA1, str);
	  else if (str && strlen(str) == 64)
	    repodata_set_checksum(data, handle, SOLVABLE_HDRID, REPOKEY_TYPE_SHA256, str);
	}
      u32 = headint32(rpmhead, TAG_BUILDTIME);
      if (u32)
        repodata_set_num(data, handle, SOLVABLE_BUILDTIME, u32);
      str = headstring(rpmhead, TAG_BUILDHOST);
      if (str)
	repodata_set_str(data, handle, SOLVABLE_BUILDHOST, str);
      u32 = headint32(rpmhead, TAG_INSTALLTIME);
      if (u32)
        repodata_set_num(data, handle, SOLVABLE_INSTALLTIME, u32);
      u64 = headint64(rpmhead, TAG_LONGSIZE);
      if (u64)
        repodata_set_num(data, handle, SOLVABLE_INSTALLSIZE, u64);
      else
	{
	  u32 = headint32(rpmhead, TAG_SIZE);
	  if (u32)
	    repodata_set_num(data, handle, SOLVABLE_INSTALLSIZE, u32);
	}
      if (sourcerpm)
	repodata_set_sourcepkg(data, handle, sourcerpm);
      if ((flags & RPM_ADD_TRIGGERS) != 0)
	{
	  unsigned int ida = makedeps(pool, repo, rpmhead, TAG_TRIGGERNAME, TAG_TRIGGERVERSION, TAG_TRIGGERFLAGS, 0, 0);
	  Id id, lastid = 0;
	  for (lastid = 0; (id = repo->idarraydata[ida]) != 0; ida++, lastid = id)
	    if (id != lastid)
	      repodata_add_idarray(data, handle, SOLVABLE_TRIGGERS, id);
	}
      if ((flags & RPM_ADD_NO_FILELIST) == 0)
	addfilelist(data, handle, rpmhead, flags);
      if ((flags & RPM_ADD_WITH_CHANGELOG) != 0)
	addchangelog(data, handle, rpmhead);
    }
  return 1;
}

static inline unsigned int
getu32(const unsigned char *dp)
{
  return dp[0] << 24 | dp[1] << 16 | dp[2] << 8 | dp[3];
}

#ifdef ENABLE_RPMDB

struct rpmdbentry {
  Id rpmdbid;
  Id nameoff;
};

#define ENTRIES_BLOCK 255
#define NAMEDATA_BLOCK 1023

# ifdef ENABLE_RPMDB_LIBRPM
#  include "repo_rpmdb_librpm.h"
# else
#  include "repo_rpmdb_bdb.h"
# endif

#else

/* dummy state just to store pool/rootdir and header data */
struct rpmdbstate {
  Pool *pool;
  char *rootdir;

  RpmHead *rpmhead;	/* header storage space */
  unsigned int rpmheadsize;
};

#endif


#ifndef ENABLE_RPMPKG_LIBRPM

static inline RpmHead *
realloc_head(struct rpmdbstate *state, unsigned int len)
{
  if (len > state->rpmheadsize)
    {
      state->rpmheadsize = len + 128;
      state->rpmhead = solv_realloc(state->rpmhead, sizeof(*state->rpmhead) + state->rpmheadsize);
    }
  return state->rpmhead;
}

static int
headfromfp(struct rpmdbstate *state, const char *name, FILE *fp, unsigned char *lead, unsigned int cnt, unsigned int dsize, unsigned int pad, Chksum *chk1, Chksum *chk2)
{
  unsigned int len = 16 * cnt + dsize + pad;
  RpmHead *rpmhead = realloc_head(state, len + 1);
  if (fread(rpmhead->data, len, 1, fp) != 1)
    return pool_error(state->pool, 0, "%s: unexpected EOF", name);
  if (chk1)
    solv_chksum_add(chk1, rpmhead->data, len);
  if (chk2)
    solv_chksum_add(chk2, rpmhead->data, len);
  headinit(rpmhead, cnt, dsize);
  return 1;
}

# if defined(ENABLE_RPMDB) && (!defined(ENABLE_RPMDB_LIBRPM) || defined(HAVE_RPMDBNEXTITERATORHEADERBLOB))

static int
headfromhdrblob(struct rpmdbstate *state, const unsigned char *data, unsigned int size)
{
  unsigned int dsize, cnt, len;
  RpmHead *rpmhead;
  if (size < 8)
    return pool_error(state->pool, 0, "corrupt rpm database (size)");
  cnt = getu32(data);
  dsize = getu32(data + 4);
  if (cnt >= MAX_HDR_CNT || dsize >= MAX_HDR_DSIZE)
    return pool_error(state->pool, 0, "corrupt rpm database (cnt/dcnt)");
  if (8 + cnt * 16 + dsize > size)
    return pool_error(state->pool, 0, "corrupt rpm database (data size)");
  len = 16 * cnt + dsize;
  rpmhead = realloc_head(state, len + 1);
  memcpy(rpmhead->data, data + 8, len);
  headinit(rpmhead, cnt, dsize);
  return 1;
}

# endif

#else

static int
headfromfp(struct rpmdbstate *state, const char *name, FILE *fp, unsigned char *lead, unsigned int cnt, unsigned int dsize, unsigned int pad, Chksum *chk1, Chksum *chk2)
{
  unsigned int len = 16 * cnt + dsize + pad;
  char *buf = solv_malloc(8 + len);
  Header h;
  memcpy(buf, lead + 8, 8);
  if (fread(buf + 8, len, 1, fp) != 1)
    {
      solv_free(buf);
      return pool_error(state->pool, 0, "%s: unexpected EOF", name);
    }
  if (chk1)
    solv_chksum_add(chk1, buf + 8, len);
  if (chk2)
    solv_chksum_add(chk2, buf + 8, len);
  h = headerImport(buf, 8 + len - pad, HEADERIMPORT_FAST);
  if (!h)
    {
      solv_free(buf);
      return pool_error(state->pool, 0, "%s: headerImport error", name);
    }
  if (state->rpmhead)
    headfree(state->rpmhead);
  state->rpmhead = h;
  return 1;
}

#endif

static void
freestate(struct rpmdbstate *state)
{
  /* close down */
#ifdef ENABLE_RPMDB
  if (state->dbenvopened)
    closedbenv(state);
  if (state->dbpath_allocated)
    solv_free((char *)state->dbpath);
#endif
  if (state->rootdir)
    solv_free(state->rootdir);
  headfree(state->rpmhead);
}

void *
rpm_state_create(Pool *pool, const char *rootdir)
{
  struct rpmdbstate *state;
  state = solv_calloc(1, sizeof(*state));
  state->pool = pool;
  if (rootdir)
    state->rootdir = solv_strdup(rootdir);
  return state;
}

void *
rpm_state_free(void *state)
{
  if (state)
    freestate(state);
  return solv_free(state);
}


#ifdef ENABLE_RPMDB


/******************************************************************/

static Offset
copydeps(Pool *pool, Repo *repo, Offset fromoff, Repo *fromrepo)
{
  int cc;
  Id *ida, *from;
  Offset ido;

  if (!fromoff)
    return 0;
  from = fromrepo->idarraydata + fromoff;
  for (ida = from, cc = 0; *ida; ida++, cc++)
    ;
  if (cc == 0)
    return 0;
  ido = repo_reserve_ids(repo, 0, cc);
  ida = repo->idarraydata + ido;
  memcpy(ida, from, (cc + 1) * sizeof(Id));
  repo->idarraysize += cc + 1;
  return ido;
}

struct solvable_copy_cbdata {
  Repodata *data;
  Id handle;
  Id subhandle;
  Id *dircache;
  int bad;
};

static int
solvable_copy_cb(void *vcbdata, Solvable *r, Repodata *fromdata, Repokey *key, KeyValue *kv)
{
  struct solvable_copy_cbdata *cbdata = vcbdata;
  Repodata *data = cbdata->data;
  Id handle = cbdata->handle;

  switch (key->type)
    {
    case REPOKEY_TYPE_ID:
    case REPOKEY_TYPE_CONSTANTID:
    case REPOKEY_TYPE_IDARRAY:	/* used for triggers */
      if (data->localpool || fromdata->localpool)
	kv->id = repodata_translate_id(data, fromdata, kv->id, 1);
      break;
    case REPOKEY_TYPE_DIRNUMNUMARRAY:
    case REPOKEY_TYPE_DIRSTRARRAY:
      kv->id = repodata_translate_dir(data, fromdata, kv->id, 1, fromdata->repodataid == 1 ? cbdata->dircache : 0);
      if (!kv->id)
	{
	  cbdata->bad = 1;	/* oops, cannot copy this */
	  return 0;
	}
      break;
    case REPOKEY_TYPE_FIXARRAY:
      cbdata->handle = repodata_new_handle(data);
      repodata_add_fixarray(data, handle, key->name, cbdata->handle);
      repodata_search_arrayelement(fromdata, 0, 0, 0, kv, &solvable_copy_cb, cbdata);
      cbdata->handle = handle;
      return 0;
    case REPOKEY_TYPE_FLEXARRAY:
      cbdata->handle = repodata_new_handle(data);
      repodata_add_flexarray(data, handle, key->name, cbdata->handle);
      repodata_search_arrayelement(fromdata, 0, 0, 0, kv, &solvable_copy_cb, cbdata);
      cbdata->handle = handle;
      return 0;
    default:
      break;
    }
  repodata_set_kv(data, handle, key->name, key->type, kv);
  return 0;
}

static int
solvable_copy(Solvable *s, Solvable *r, Repodata *data, Id *dircache, Id **oldkeyskip)
{
  int p, i;
  Repo *repo = s->repo;
  Pool *pool = repo->pool;
  Repo *fromrepo = r->repo;
  struct solvable_copy_cbdata cbdata;
  Id *keyskip;

  /* copy solvable data */
  s->name = r->name;
  s->evr = r->evr;
  s->arch = r->arch;
  s->vendor = r->vendor;
  s->provides = copydeps(pool, repo, r->provides, fromrepo);
  s->requires = copydeps(pool, repo, r->requires, fromrepo);
  s->conflicts = copydeps(pool, repo, r->conflicts, fromrepo);
  s->obsoletes = copydeps(pool, repo, r->obsoletes, fromrepo);
  s->recommends = copydeps(pool, repo, r->recommends, fromrepo);
  s->suggests = copydeps(pool, repo, r->suggests, fromrepo);
  s->supplements = copydeps(pool, repo, r->supplements, fromrepo);
  s->enhances  = copydeps(pool, repo, r->enhances, fromrepo);

  /* copy all attributes */
  if (!data || fromrepo->nrepodata < 2)
    return 1;
  cbdata.data = data;
  cbdata.handle = s - pool->solvables;
  cbdata.subhandle = 0;
  cbdata.dircache = dircache;
  cbdata.bad = 0;
  p = r - fromrepo->pool->solvables;
  if (fromrepo->nrepodata == 2)
    {
      Repodata *fromdata = repo_id2repodata(fromrepo, 1);
      if (p >= fromdata->start && p < fromdata->end)
        repodata_search(fromdata, p, 0, 0, solvable_copy_cb, &cbdata);
    }
  else
    {
      keyskip = repo_create_keyskip(repo, p, oldkeyskip);
      FOR_REPODATAS(fromrepo, i, data)
	{
	  if (p >= data->start && p < data->end)
	    repodata_search_keyskip(data, p, 0, 0, keyskip, solvable_copy_cb, &cbdata);
	}
    }
  if (cbdata.bad)
    {
      repodata_unset_uninternalized(data, cbdata.handle, 0);
      memset(s, 0, sizeof(*s));
      s->repo = repo;
      return 0;
    }
  return 1;
}

/* used to sort entries by package name that got returned in some database order */
static int
rpmids_sort_cmp(const void *va, const void *vb, void *dp)
{
  struct rpmdbentry const *a = va, *b = vb;
  char *namedata = dp;
  int r;
  r = strcmp(namedata + a->nameoff, namedata + b->nameoff);
  if (r)
    return r;
  return a->rpmdbid - b->rpmdbid;
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
    return strcmp(pool_id2str(pool, a->name), pool_id2str(pool, b->name));
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
  if (data)
    repodata_swap_attrs(data, pa, pb);
}

static void
mkrpmdbcookie(struct stat *st, unsigned char *cookie, int flags)
{
  int f = 0;
  memset(cookie, 0, 32);
  cookie[3] = RPMDB_COOKIE_VERSION;
  memcpy(cookie + 16, &st->st_ino, sizeof(st->st_ino));
  memcpy(cookie + 24, &st->st_dev, sizeof(st->st_dev));
  if ((flags & RPM_ADD_WITH_PKGID) != 0)
    f |= 1;
  if ((flags & RPM_ADD_WITH_HDRID) != 0)
    f |= 2;
  if ((flags & RPM_ADD_WITH_CHANGELOG) != 0)
    f |= 4;
  if ((flags & RPM_ADD_NO_FILELIST) == 0)
    f |= 8;
  if ((flags & RPM_ADD_NO_RPMLIBREQS) != 0)
    cookie[1] = 1;
  cookie[0] = f;
}

/*
 * read rpm db as repo
 *
 */

int
repo_add_rpmdb(Repo *repo, Repo *ref, int flags)
{
  Pool *pool = repo->pool;
  struct stat packagesstat;
  unsigned char newcookie[32];
  const unsigned char *oldcookie = 0;
  Id oldcookietype = 0;
  Repodata *data;
  int count = 0, done = 0;
  struct rpmdbstate state;
  int i;
  Solvable *s;
  unsigned int now;

  now = solv_timems(0);
  memset(&state, 0, sizeof(state));
  state.pool = pool;
  if (flags & REPO_USE_ROOTDIR)
    state.rootdir = solv_strdup(pool_get_rootdir(pool));

  data = repo_add_repodata(repo, flags);

  if (ref && !(ref->nsolvables && ref->rpmdbid && ref->pool == repo->pool))
    {
      if ((flags & RPMDB_EMPTY_REFREPO) != 0)
	repo_empty(ref, 1);
      ref = 0;
    }

  if (!opendbenv(&state))
    {
      solv_free(state.rootdir);
      return -1;
    }

  /* XXX: should get ro lock of Packages database! */
  if (stat_database(&state, &packagesstat))
    {
      freestate(&state);
      return -1;
    }
  mkrpmdbcookie(&packagesstat, newcookie, flags);
  repodata_set_bin_checksum(data, SOLVID_META, REPOSITORY_RPMDBCOOKIE, REPOKEY_TYPE_SHA256, newcookie);

  if (ref)
    oldcookie = repo_lookup_bin_checksum(ref, SOLVID_META, REPOSITORY_RPMDBCOOKIE, &oldcookietype);
  if (!ref || !oldcookie || oldcookietype != REPOKEY_TYPE_SHA256 || memcmp(oldcookie, newcookie, 32) != 0)
    {
      int solvstart = 0, solvend = 0;
      Id dbid;

      if (ref && (flags & RPMDB_EMPTY_REFREPO) != 0)
	repo_empty(ref, 1);	/* get it out of the way */
      if ((flags & RPMDB_REPORT_PROGRESS) != 0)
	count = count_headers(&state);
      if (pkgdb_cursor_open(&state))
	{
	  freestate(&state);
	  return -1;
	}
      i = 0;
      s = 0;
      while ((dbid = pkgdb_cursor_getrpm(&state)) != 0)
	{
	  if (dbid == -1)
	    {
	      pkgdb_cursor_close(&state);
	      freestate(&state);
	      return -1;
	    }
	  if (!s)
	    {
	      s = pool_id2solvable(pool, repo_add_solvable(repo));
	      if (!solvstart)
		solvstart = s - pool->solvables;
	      solvend = s - pool->solvables + 1;
	    }
	  if (!repo->rpmdbid)
	    repo->rpmdbid = repo_sidedata_create(repo, sizeof(Id));
	  repo->rpmdbid[(s - pool->solvables) - repo->start] = dbid;
	  if (rpmhead2solv(pool, repo, data, s, state.rpmhead, flags | RPM_ADD_TRIGGERS))
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
	        pool_debug(pool, SOLV_ERROR, "%%%% %d\n", done * 100 / count);
	    }
	}
      pkgdb_cursor_close(&state);
      if (s)
	{
	  /* oops, could not reuse. free it instead */
          s = solvable_free(s, 1);
	  solvend--;
	}
      /* now sort all solvables in the new solvstart..solvend block */
      if (solvend - solvstart > 1)
	{
	  Id *pkgids = solv_malloc2(solvend - solvstart, sizeof(Id));
	  for (i = solvstart; i < solvend; i++)
	    pkgids[i - solvstart] = i;
	  solv_sort(pkgids, solvend - solvstart, sizeof(Id), pkgids_sort_cmp, repo);
	  /* adapt order */
	  for (i = solvstart; i < solvend; i++)
	    {
	      int j = pkgids[i - solvstart];
	      while (j < i)
		j = pkgids[i - solvstart] = pkgids[j - solvstart];
	      if (j != i)
	        swap_solvables(repo, data, i, j);
	    }
	  solv_free(pkgids);
	}
    }
  else
    {
      Id *dircache;
      Id *oldkeyskip = 0;
      struct rpmdbentry *entries = 0, *rp;
      int nentries = 0;
      char *namedata = 0;
      unsigned int refmask, h;
      Id id, *refhash;
      int res;

      /* get ids of installed rpms */
      entries = getinstalledrpmdbids(&state, "Name", 0, &nentries, &namedata, flags & RPMDB_KEEP_GPG_PUBKEY);
      if (!entries)
	{
	  freestate(&state);
	  return -1;
	}

      /* sort by name */
      if (nentries > 1)
        solv_sort(entries, nentries, sizeof(*entries), rpmids_sort_cmp, namedata);

      /* create hash from dbid to ref */
      refmask = mkmask(ref->nsolvables);
      refhash = solv_calloc(refmask + 1, sizeof(Id));
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
	  for (i = 0, rp = entries; i < nentries; i++, rp++)
	    {
	      if (refhash)
		{
		  Id dbid = rp->rpmdbid;
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

      if (ref && (flags & RPMDB_EMPTY_REFREPO) != 0)
        s = pool_id2solvable(pool, repo_add_solvable_block_before(repo, nentries, ref));
      else
        s = pool_id2solvable(pool, repo_add_solvable_block(repo, nentries));
      if (!repo->rpmdbid)
        repo->rpmdbid = repo_sidedata_create(repo, sizeof(Id));

      dircache = repodata_create_dirtranscache(data);
      for (i = 0, rp = entries; i < nentries; i++, rp++, s++)
	{
	  Id dbid = rp->rpmdbid;
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
		  if (r->repo == ref && solvable_copy(s, r, data, dircache, &oldkeyskip))
		    continue;
		}
	    }
	  res = getrpm_dbid(&state, dbid);
	  if (res <= 0)
	    {
	      if (!res)
	        pool_error(pool, -1, "inconsistent rpm database, key %d not found. run 'rpm --rebuilddb' to fix.", dbid);
	      freestate(&state);
	      solv_free(entries);
	      solv_free(namedata);
	      solv_free(refhash);
	      dircache = repodata_free_dirtranscache(dircache);
	      return -1;
	    }
	  rpmhead2solv(pool, repo, data, s, state.rpmhead, flags | RPM_ADD_TRIGGERS);
	  if ((flags & RPMDB_REPORT_PROGRESS) != 0)
	    {
	      if (done < count)
		done++;
	      if (done < count && (done - 1) * 100 / count != done * 100 / count)
		pool_debug(pool, SOLV_ERROR, "%%%% %d\n", done * 100 / count);
	    }
	}
      dircache = repodata_free_dirtranscache(dircache);

      solv_free(oldkeyskip);
      solv_free(entries);
      solv_free(namedata);
      solv_free(refhash);
      if (ref && (flags & RPMDB_EMPTY_REFREPO) != 0)
	repo_empty(ref, 1);
    }

  freestate(&state);
  if (!(flags & REPO_NO_INTERNALIZE))
    repodata_internalize(data);
  if ((flags & RPMDB_REPORT_PROGRESS) != 0)
    pool_debug(pool, SOLV_ERROR, "%%%% 100\n");
  POOL_DEBUG(SOLV_DEBUG_STATS, "repo_add_rpmdb took %d ms\n", solv_timems(now));
  POOL_DEBUG(SOLV_DEBUG_STATS, "repo size: %d solvables\n", repo->nsolvables);
  POOL_DEBUG(SOLV_DEBUG_STATS, "repo memory used: %d K incore, %d K idarray\n", repodata_memused(data)/1024, repo->idarraysize / (int)(1024/sizeof(Id)));
  return 0;
}

int
repo_add_rpmdb_reffp(Repo *repo, FILE *fp, int flags)
{
  int res;
  Repo *ref = 0;

  if (!fp)
    return repo_add_rpmdb(repo, 0, flags);
  ref = repo_create(repo->pool, "add_rpmdb_reffp");
  if (repo_add_solv(ref, fp, 0) != 0)
    {
      repo_free(ref, 1);
      ref = 0;
    }
  if (ref && ref->start == ref->end)
    {
      repo_free(ref, 1);
      ref = 0;
    }
  if (ref)
    repo_disable_paging(ref);
  res = repo_add_rpmdb(repo, ref, flags | RPMDB_EMPTY_REFREPO);
  if (ref)
    repo_free(ref, 1);
  return res;
}

#endif	/* ENABLE_RPMDB */

Id
repo_add_rpm(Repo *repo, const char *rpm, int flags)
{
  unsigned int sigdsize, sigcnt, sigpad, l;
  Pool *pool = repo->pool;
  Solvable *s;
  struct rpmdbstate state;
  char *payloadformat;
  FILE *fp;
  unsigned char lead[4096];
  int headerstart, headerend;
  struct stat stb;
  Repodata *data;
  unsigned char pkgid[16];
  unsigned char leadsigid[16];
  unsigned char hdrid[32];
  int pkgidtype, leadsigidtype, hdridtype;
  Id chksumtype = 0;
  Chksum *chksumh = 0;
  Chksum *leadsigchksumh = 0;

  data = repo_add_repodata(repo, flags);

  if ((flags & RPM_ADD_WITH_SHA256SUM) != 0)
    chksumtype = REPOKEY_TYPE_SHA256;
  else if ((flags & RPM_ADD_WITH_SHA1SUM) != 0)
    chksumtype = REPOKEY_TYPE_SHA1;

  /* open rpm */
  if ((fp = fopen(flags & REPO_USE_ROOTDIR ? pool_prepend_rootdir_tmp(pool, rpm) : rpm, "r")) == 0)
    {
      pool_error(pool, -1, "%s: %s", rpm, strerror(errno));
      return 0;
    }
  if (fstat(fileno(fp), &stb))
    {
      pool_error(pool, -1, "fstat: %s", strerror(errno));
      fclose(fp);
      return 0;
    }

  /* setup state */
  memset(&state, 0, sizeof(state));
  state.pool = pool;

  /* process lead */
  if (chksumtype)
    chksumh = solv_chksum_create(chksumtype);
  if ((flags & RPM_ADD_WITH_LEADSIGID) != 0)
    leadsigchksumh = solv_chksum_create(REPOKEY_TYPE_MD5);
  if (fread(lead, 96 + 16, 1, fp) != 1 || getu32(lead) != 0xedabeedb)
    {
      pool_error(pool, -1, "%s: not a rpm", rpm);
      solv_chksum_free(leadsigchksumh, 0);
      solv_chksum_free(chksumh, 0);
      fclose(fp);
      return 0;
    }
  if (chksumh)
    solv_chksum_add(chksumh, lead, 96 + 16);
  if (leadsigchksumh)
    solv_chksum_add(leadsigchksumh, lead, 96 + 16);

  /* process signature header */
  if (lead[78] != 0 || lead[79] != 5)
    {
      pool_error(pool, -1, "%s: not a rpm v5 header", rpm);
      solv_chksum_free(leadsigchksumh, 0);
      solv_chksum_free(chksumh, 0);
      fclose(fp);
      return 0;
    }
  if (getu32(lead + 96) != 0x8eade801)
    {
      pool_error(pool, -1, "%s: bad signature header", rpm);
      solv_chksum_free(leadsigchksumh, 0);
      solv_chksum_free(chksumh, 0);
      fclose(fp);
      return 0;
    }
  sigcnt = getu32(lead + 96 + 8);
  sigdsize = getu32(lead + 96 + 12);
  if (sigcnt >= MAX_SIG_CNT || sigdsize >= MAX_SIG_DSIZE)
    {
      pool_error(pool, -1, "%s: bad signature header", rpm);
      solv_chksum_free(leadsigchksumh, 0);
      solv_chksum_free(chksumh, 0);
      fclose(fp);
      return 0;
    }
  sigpad = sigdsize & 7 ? 8 - (sigdsize & 7) : 0;
  headerstart = 96 + 16 + sigcnt * 16 + sigdsize + sigpad;
  pkgidtype = leadsigidtype = hdridtype = 0;
  if ((flags & (RPM_ADD_WITH_PKGID | RPM_ADD_WITH_HDRID)) != 0)
    {
      if (!headfromfp(&state, rpm, fp, lead + 96, sigcnt, sigdsize, sigpad, chksumh, leadsigchksumh))
	{
      solv_chksum_free(leadsigchksumh, 0);
      solv_chksum_free(chksumh, 0);
	  fclose(fp);
	  return 0;
	}
      if ((flags & RPM_ADD_WITH_PKGID) != 0)
	{
	  unsigned char *chksum;
	  unsigned int chksumsize;
	  chksum = headbinary(state.rpmhead, SIGTAG_MD5, &chksumsize);
	  if (chksum && chksumsize == 16)
	    {
	      pkgidtype = REPOKEY_TYPE_MD5;
	      memcpy(pkgid, chksum, 16);
	    }
	}
      if ((flags & RPM_ADD_WITH_HDRID) != 0)
	{
	  const char *str = headstring(state.rpmhead, TAG_SHA1HEADER);
	  if (str && strlen(str) == 40)
	    {
	      if (solv_hex2bin(&str, hdrid, 20) == 20)
	        hdridtype = REPOKEY_TYPE_SHA1;
	    }
	  else if (str && strlen(str) == 64)
	    {
	      if (solv_hex2bin(&str, hdrid, 32) == 32)
	        hdridtype = REPOKEY_TYPE_SHA256;
	    }
	}
    }
  else
    {
      /* just skip the signature header */
      unsigned int len = sigcnt * 16 + sigdsize + sigpad;
      while (len)
	{
	  l = len > 4096 ? 4096 : len;
	  if (fread(lead, l, 1, fp) != 1)
	    {
	      pool_error(pool, -1, "%s: unexpected EOF", rpm);
	      solv_chksum_free(leadsigchksumh, 0);
	      solv_chksum_free(chksumh, 0);
	      fclose(fp);
	      return 0;
	    }
	  if (chksumh)
	    solv_chksum_add(chksumh, lead, l);
	  if (leadsigchksumh)
	    solv_chksum_add(leadsigchksumh, lead, l);
	  len -= l;
	}
    }
  if (leadsigchksumh)
    {
      leadsigchksumh = solv_chksum_free(leadsigchksumh, leadsigid);
      leadsigidtype = REPOKEY_TYPE_MD5;
    }

  /* process main header */
  if (fread(lead, 16, 1, fp) != 1)
    {
      pool_error(pool, -1, "%s: unexpected EOF", rpm);
      solv_chksum_free(chksumh, 0);
      fclose(fp);
      return 0;
    }
  if (chksumh)
    solv_chksum_add(chksumh, lead, 16);
  if (getu32(lead) != 0x8eade801)
    {
      pool_error(pool, -1, "%s: bad header", rpm);
      solv_chksum_free(chksumh, 0);
      fclose(fp);
      return 0;
    }
  sigcnt = getu32(lead + 8);
  sigdsize = getu32(lead + 12);
  if (sigcnt >= MAX_HDR_CNT || sigdsize >= MAX_HDR_DSIZE)
    {
      pool_error(pool, -1, "%s: bad header", rpm);
      solv_chksum_free(chksumh, 0);
      fclose(fp);
      return 0;
    }
  headerend = headerstart + 16 + sigdsize + sigcnt * 16;

  if (!headfromfp(&state, rpm, fp, lead, sigcnt, sigdsize, 0, chksumh, 0))
    {
      solv_chksum_free(chksumh, 0);
      fclose(fp);
      return 0;
    }
  if (headexists(state.rpmhead, TAG_PATCHESNAME))
    {
      /* this is a patch rpm, ignore */
      pool_error(pool, -1, "%s: is patch rpm", rpm);
      fclose(fp);
      solv_chksum_free(chksumh, 0);
      headfree(state.rpmhead);
      return 0;
    }
  payloadformat = headstring(state.rpmhead, TAG_PAYLOADFORMAT);
  if (payloadformat && !strcmp(payloadformat, "drpm"))
    {
      /* this is a delta rpm */
      pool_error(pool, -1, "%s: is delta rpm", rpm);
      fclose(fp);
      solv_chksum_free(chksumh, 0);
      headfree(state.rpmhead);
      return 0;
    }
  if (chksumh)
    while ((l = fread(lead, 1, sizeof(lead), fp)) > 0)
      solv_chksum_add(chksumh, lead, l);
  fclose(fp);
  s = pool_id2solvable(pool, repo_add_solvable(repo));
  if (!rpmhead2solv(pool, repo, data, s, state.rpmhead, flags & ~(RPM_ADD_WITH_HDRID | RPM_ADD_WITH_PKGID)))
    {
      s = solvable_free(s, 1);
      solv_chksum_free(chksumh, 0);
      headfree(state.rpmhead);
      return 0;
    }
  if (!(flags & REPO_NO_LOCATION))
    repodata_set_location(data, s - pool->solvables, 0, 0, rpm);
  if (S_ISREG(stb.st_mode))
    repodata_set_num(data, s - pool->solvables, SOLVABLE_DOWNLOADSIZE, (unsigned long long)stb.st_size);
  repodata_set_num(data, s - pool->solvables, SOLVABLE_HEADEREND, headerend);
  if (pkgidtype)
    repodata_set_bin_checksum(data, s - pool->solvables, SOLVABLE_PKGID, pkgidtype, pkgid);
  if (hdridtype)
    repodata_set_bin_checksum(data, s - pool->solvables, SOLVABLE_HDRID, hdridtype, hdrid);
  if (leadsigidtype)
    repodata_set_bin_checksum(data, s - pool->solvables, SOLVABLE_LEADSIGID, leadsigidtype, leadsigid);
  if (chksumh)
    {
      repodata_set_bin_checksum(data, s - pool->solvables, SOLVABLE_CHECKSUM, chksumtype, solv_chksum_get(chksumh, 0));
      chksumh = solv_chksum_free(chksumh, 0);
    }
  headfree(state.rpmhead);
  if (!(flags & REPO_NO_INTERNALIZE))
    repodata_internalize(data);
  return s - pool->solvables;
}

Id
repo_add_rpm_handle(Repo *repo, void *rpmhandle, int flags)
{
  Pool *pool = repo->pool;
  Repodata *data;
  RpmHead *rpmhead = rpmhandle;
  Solvable *s;
  char *payloadformat;

  data = repo_add_repodata(repo, flags);
  if (headexists(rpmhead, TAG_PATCHESNAME))
    {
      pool_error(pool, -1, "is a patch rpm");
      return 0;
    }
  payloadformat = headstring(rpmhead, TAG_PAYLOADFORMAT);
  if (payloadformat && !strcmp(payloadformat, "drpm"))
    {
      /* this is a delta rpm */
      pool_error(pool, -1, "is a delta rpm");
      return 0;
    }
  s = pool_id2solvable(pool, repo_add_solvable(repo));
  if (!rpmhead2solv(pool, repo, data, s, rpmhead, flags))
    {
      s = solvable_free(s, 1);
      return 0;
    }
  if (!(flags & REPO_NO_INTERNALIZE))
    repodata_internalize(data);
  return s - pool->solvables;
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
  sprintf(hash, "%08x%08x%08x%08x", r, l, 0, 0);
}

void
rpm_iterate_filelist(void *rpmhandle, int flags, void (*cb)(void *, const char *, struct filelistinfo *), void *cbdata)
{
  RpmHead *rpmhead = rpmhandle;
  char **bn;
  char **dn;
  char **md = 0;
  char **lt = 0;
  uint32_t *di, diidx;
  uint32_t *co = 0;
  uint32_t *ff = 0;
  uint16_t *fm;
  unsigned int lastdir;
  int lastdirl;
  int cnt, dcnt, cnt2;
  int i, l1, l;
  char *space = 0;
  int spacen = 0;
  char md5[33];
  struct filelistinfo info;

  dn = headstringarray(rpmhead, TAG_DIRNAMES, &dcnt);
  if (!dn)
    return;
  if ((flags & RPM_ITERATE_FILELIST_ONLYDIRS) != 0)
    {
      for (i = 0; i < dcnt; i++)
	(*cb)(cbdata, dn[i], 0);
      solv_free(dn);
      return;
    }
  bn = headstringarray(rpmhead, TAG_BASENAMES, &cnt);
  if (!bn)
    {
      solv_free(dn);
      return;
    }
  di = headint32array(rpmhead, TAG_DIRINDEXES, &cnt2);
  if (!di || cnt != cnt2)
    {
      solv_free(di);
      solv_free(bn);
      solv_free(dn);
      return;
    }
  fm = headint16array(rpmhead, TAG_FILEMODES, &cnt2);
  if (!fm || cnt != cnt2)
    {
      solv_free(fm);
      solv_free(di);
      solv_free(bn);
      solv_free(dn);
      return;
    }
  if ((flags & RPM_ITERATE_FILELIST_WITHMD5) != 0)
    {
      md = headstringarray(rpmhead, TAG_FILEMD5S, &cnt2);
      if (!md || cnt != cnt2)
	{
	  solv_free(md);
	  solv_free(fm);
	  solv_free(di);
	  solv_free(bn);
	  solv_free(dn);
	  return;
	}
    }
  if ((flags & RPM_ITERATE_FILELIST_WITHCOL) != 0)
    {
      co = headint32array(rpmhead, TAG_FILECOLORS, &cnt2);
      if (co && cnt != cnt2)
	{
	  solv_free(co);
	  solv_free(md);
	  solv_free(fm);
	  solv_free(di);
	  solv_free(bn);
	  solv_free(dn);
	  return;
	}
    }
  if ((flags & RPM_ITERATE_FILELIST_NOGHOSTS) != 0)
    {
      ff = headint32array(rpmhead, TAG_FILEFLAGS, &cnt2);
      if (!ff || cnt != cnt2)
	{
	  solv_free(ff);
	  solv_free(co);
	  solv_free(md);
	  solv_free(fm);
	  solv_free(di);
	  solv_free(bn);
	  solv_free(dn);
	  return;
	}
    }
  lastdir = dcnt;
  lastdirl = 0;
  memset(&info, 0, sizeof(info));
  for (i = 0; i < cnt; i++)
    {
      if (ff && (ff[i] & FILEFLAG_GHOST) != 0)
	continue;
      diidx = di[i];
      if (diidx >= dcnt)
	continue;
      l1 = lastdir == diidx ? lastdirl : strlen(dn[diidx]);
      l = l1 + strlen(bn[i]) + 1;
      if (l > spacen)
	{
	  spacen = l + 16;
	  space = solv_realloc(space, spacen);
	}
      if (lastdir != diidx)
	{
          strcpy(space, dn[diidx]);
	  lastdir = diidx;
	  lastdirl = l1;
	}
      strcpy(space + l1, bn[i]);
      info.diridx = diidx;
      info.dirlen = l1;
      if (fm)
        info.mode = fm[i];
      if (md)
	{
	  info.digest = md[i];
	  if (fm && S_ISLNK(fm[i]))
	    {
	      info.digest = 0;
	      if (!lt)
		{
		  lt = headstringarray(rpmhead, TAG_FILELINKTOS, &cnt2);
		  if (cnt != cnt2)
		    lt = solv_free(lt);
		}
	      if (lt)
		{
		  linkhash(lt[i], md5);
		  info.digest = md5;
		}
	    }
	  if (!info.digest)
	    {
	      sprintf(md5, "%08x%08x%08x%08x", (fm[i] >> 12) & 65535, 0, 0, 0);
	      info.digest = md5;
	    }
	}
      info.color = co ? co[i] : 0;
      (*cb)(cbdata, space, &info);
    }
  solv_free(space);
  solv_free(lt);
  solv_free(md);
  solv_free(fm);
  solv_free(di);
  solv_free(bn);
  solv_free(dn);
  solv_free(co);
  solv_free(ff);
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
    case 0:	/* return canonical name of rpm */
      name = headstring(rpmhead, TAG_NAME);
      if (!name)
	name = "";
      sourcerpm = headstring(rpmhead, TAG_SOURCERPM);
      if (sourcerpm || !(headexists(rpmhead, TAG_SOURCEPACKAGE) || headissourceheuristic(rpmhead)))
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
      l = strlen(name) + 1 + strlen(evr ? evr : "") + 1 + strlen(arch) + 1;
      r = solv_malloc(l);
      sprintf(r, "%s-%s.%s", name, evr ? evr : "", arch);
      solv_free(evr);
      break;
    case SOLVABLE_NAME:
      name = headstring(rpmhead, TAG_NAME);
      r = solv_strdup(name);
      break;
    case SOLVABLE_SUMMARY:
      name = headstring(rpmhead, TAG_SUMMARY);
      r = solv_strdup(name);
      break;
    case SOLVABLE_DESCRIPTION:
      name = headstring(rpmhead, TAG_DESCRIPTION);
      r = solv_strdup(name);
      break;
    case SOLVABLE_EVR:
      r = headtoevr(rpmhead);
      break;
    }
  return r;
}

unsigned long long
rpm_query_num(void *rpmhandle, Id what, unsigned long long notfound)
{
  RpmHead *rpmhead = rpmhandle;
  unsigned int u32;

  switch (what)
    {
    case SOLVABLE_INSTALLTIME:
      u32 = headint32(rpmhead, TAG_INSTALLTIME);
      return u32 ? u32 : notfound;
    }
  return notfound;
}

#ifdef ENABLE_RPMDB

int
rpm_installedrpmdbids(void *rpmstate, const char *index, const char *match, Queue *rpmdbidq)
{
  struct rpmdbentry *entries;
  int nentries, i;

  entries = getinstalledrpmdbids(rpmstate, index ? index : "Name", match, &nentries, 0, 0);
  if (rpmdbidq)
    {
      queue_empty(rpmdbidq);
      for (i = 0; i < nentries; i++)
        queue_push(rpmdbidq, entries[i].rpmdbid);
    }
  solv_free(entries);
  return nentries;
}

int
rpm_hash_database_state(void *rpmstate, Chksum *chk)
{
  struct rpmdbstate *state = rpmstate;
  struct stat stb;
  if (stat_database(state, &stb))
    return -1;
  if (state->dbenvopened != 1 && !opendbenv(state))
    return -1;
  solv_chksum_add(chk, &stb.st_mtime, sizeof(stb.st_mtime));
  solv_chksum_add(chk, &stb.st_size, sizeof(stb.st_size));
  solv_chksum_add(chk, &stb.st_ino, sizeof(stb.st_ino));
  hash_name_index(rpmstate, chk);
  return 0;
}

int
rpm_stat_database(void *rpmstate, void *stb)
{
  return stat_database((struct rpmdbstate *)rpmstate, (struct stat *)stb) ? -1 : 0;
}

void *
rpm_byrpmdbid(void *rpmstate, Id rpmdbid)
{
  struct rpmdbstate *state = rpmstate;
  int r;

  r = getrpm_dbid(state, rpmdbid);
  if (!r)
    pool_error(state->pool, 0, "header #%d not in database", rpmdbid);
  return r <= 0 ? 0 : state->rpmhead;
}

#endif	/* ENABLE_RPMDB */

void *
rpm_byfp(void *rpmstate, FILE *fp, const char *name)
{
  struct rpmdbstate *state = rpmstate;
  unsigned int sigdsize, sigcnt, l;
  unsigned char lead[4096];

  if (fread(lead, 96 + 16, 1, fp) != 1 || getu32(lead) != 0xedabeedb)
    {
      pool_error(state->pool, 0, "%s: not a rpm", name);
      return 0;
    }
  if (lead[78] != 0 || lead[79] != 5)
    {
      pool_error(state->pool, 0, "%s: not a V5 header", name);
      return 0;
    }

  /* skip signature header */
  if (getu32(lead + 96) != 0x8eade801)
    {
      pool_error(state->pool, 0, "%s: bad signature header", name);
      return 0;
    }
  sigcnt = getu32(lead + 96 + 8);
  sigdsize = getu32(lead + 96 + 12);
  if (sigcnt >= MAX_SIG_CNT || sigdsize >= MAX_SIG_DSIZE)
    {
      pool_error(state->pool, 0, "%s: bad signature header", name);
      return 0;
    }
  sigdsize += sigcnt * 16;
  sigdsize = (sigdsize + 7) & ~7;
  while (sigdsize)
    {
      l = sigdsize > 4096 ? 4096 : sigdsize;
      if (fread(lead, l, 1, fp) != 1)
	{
	  pool_error(state->pool, 0, "%s: unexpected EOF", name);
	  return 0;
	}
      sigdsize -= l;
    }

  if (fread(lead, 16, 1, fp) != 1)
    {
      pool_error(state->pool, 0, "%s: unexpected EOF", name);
      return 0;
    }
  if (getu32(lead) != 0x8eade801)
    {
      pool_error(state->pool, 0, "%s: bad header", name);
      return 0;
    }
  sigcnt = getu32(lead + 8);
  sigdsize = getu32(lead + 12);
  if (sigcnt >= MAX_HDR_CNT || sigdsize >= MAX_HDR_DSIZE)
    {
      pool_error(state->pool, 0, "%s: bad header", name);
      return 0;
    }
  if (!headfromfp(state, name, fp, lead, sigcnt, sigdsize, 0, 0, 0))
    return 0;
  return state->rpmhead;
}

#if defined(ENABLE_RPMDB_BYRPMHEADER) || defined(ENABLE_RPMDB_LIBRPM)

void *
rpm_byrpmh(void *rpmstate, Header h)
{
  struct rpmdbstate *state = rpmstate;
#ifndef ENABLE_RPMPKG_LIBRPM
  const unsigned char *uh;
  unsigned int dsize, cnt, len;
  RpmHead *rpmhead;

  if (!h)
    return 0;
#ifndef RPM5
  uh = headerUnload(h);
#else
  uh = headerUnload(h, NULL);
#endif
  if (!uh)
    return 0;
  cnt = getu32(uh);
  dsize = getu32(uh + 4);
  if (cnt >= MAX_HDR_CNT || dsize >= MAX_HDR_DSIZE)
    {
      free((void *)uh);
      return 0;
    }
  len = 16 * cnt + dsize;
  rpmhead = realloc_head(state, len + 1);;
  memcpy(rpmhead->data, uh + 8, len);
  headinit(rpmhead, cnt, dsize);
  free((void *)uh);
#else
  if (!h)
    return 0;
  if (state->rpmhead)
    headfree(state->rpmhead);
  state->rpmhead = headerLink(h);
#endif
  return state->rpmhead;
}

#endif	/* defined(ENABLE_RPMDB_BYRPMHEADER) || defined(ENABLE_RPMDB_LIBRPM) */

