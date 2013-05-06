/*
 * Copyright (c) 2007-2013, Novell Inc.
 *
 * This program is licensed under the BSD license, read LICENSE.BSD
 * for further information
 */

/*
 * repo_rpmdb_pubkey
 *
 * support for pubkeys stored in the rpmdb database
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

#include <rpm/rpmio.h>
#include <rpm/rpmpgp.h>
#ifndef RPM5
#include <rpm/header.h>
#endif
#include <rpm/rpmdb.h>

#include "pool.h"
#include "repo.h"
#include "hash.h"
#include "util.h"
#include "queue.h"
#include "chksum.h"
#include "repo_rpmdb.h"

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
  bp = buf = solv_malloc(l * 3 / 4 + 4);
  eof = 0;
  while (!eof)
    {
      pubkey = r64dec1(pubkey, &v, &eof);
      if (!pubkey)
	{
	  solv_free(buf);
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
      solv_free(buf);
      return 0;
    }
  if (v != crc24(buf, bp - buf))
    {
      solv_free(buf);
      return 0;
    }
  while (*pubkey == ' ' || *pubkey == '\t' || *pubkey == '\n' || *pubkey == '\r')
    pubkey++;
  if (strncmp(pubkey, "-----END PGP PUBLIC KEY BLOCK-----", 34) != 0)
    {
      solv_free(buf);
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
#if 0
  unsigned char *pubkey = 0;
  unsigned char *userid = 0;
  int pubkeyl = 0;
  int useridl = 0;
#endif

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
	      /* sanity: p[0] must be zero */
	      if (pl <= 4 || p[0] != 0)
		return;
	      l = p[1] << 16 | p[2] << 8 | p[3];
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
#if 0
	  pubkey = solv_realloc(pubkey, l);
	  if (l)
	    memcpy(pubkey, p, l);
	  pubkeyl = l;
#endif
	  kcr = 0;
	  if (p[0] == 3 && l >= 10)
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
		  int i, ql, ql2;
		  unsigned char fp[16];
		  char fpx[32 + 1];
		  unsigned char *q;

		  ql = ((p[8] << 8 | p[9]) + 7) / 8;	/* length of public modulus */
		  if (ql >= 8 && 10 + ql + 2 <= l)
		    {
		      memcpy(keyid, p + 10 + ql - 8, 8);	/* keyid is last 64 bits of public modulus */
		      q = p + 10 + ql;
		      ql2 = ((q[0] << 8 | q[1]) + 7) / 8;	/* length of encryption exponent */
		      if (10 + ql + 2 + ql2 <= l)
			{
			  /* fingerprint is the md5 over the two MPI bodies */
			  h = solv_chksum_create(REPOKEY_TYPE_MD5);
			  solv_chksum_add(h, p + 10, ql);
			  solv_chksum_add(h, q + 2, ql2);
			  solv_chksum_free(h, fp);
			  for (i = 0; i < 16; i++)
			    sprintf(fpx + i * 2, "%02x", fp[i]);
			  repodata_set_str(data, s - s->repo->pool->solvables, PUBKEY_FINGERPRINT, fpx);
			}
		    }
		}
	    }
	  else if (p[0] == 4 && l >= 6)
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
	      /* fingerprint is the sha1 over the packet */
	      h = solv_chksum_create(REPOKEY_TYPE_SHA1);
	      solv_chksum_add(h, hdr, 3);
	      solv_chksum_add(h, p, l);
	      solv_chksum_free(h, fp);
	      for (i = 0; i < 20; i++)
		sprintf(fpx + i * 2, "%02x", fp[i]);
	      repodata_set_str(data, s - s->repo->pool->solvables, PUBKEY_FINGERPRINT, fpx);
	      memcpy(keyid, fp + 12, 8);	/* keyid is last 64 bits of fingerprint */
	    }
	}
      if (tag == 2)
	{
	  if (p[0] == 3 && p[1] == 5)
	    {
#if 0
	      Id htype = 0;
#endif
	      /* printf("V3 signature packet\n"); */
	      if (l < 17)
		continue;
	      if (p[2] != 0x10 && p[2] != 0x11 && p[2] != 0x12 && p[2] != 0x13 && p[2] != 0x1f)
		continue;
	      if (!memcmp(keyid, p + 6, 8))
		{
		  /* printf("SELF SIG\n"); */
		}
	      else
		{
		  /* printf("OTHER SIG\n"); */
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
		  void *h = solv_chksum_create(htype);
		  unsigned char b[3], *cs;

		  b[0] = 0x99;
		  b[1] = pubkeyl >> 8;
		  b[2] = pubkeyl;
		  solv_chksum_add(h, b, 3);
		  solv_chksum_add(h, pubkey, pubkeyl);
		  if (p[2] >= 0x10 && p[2] <= 0x13)
		    solv_chksum_add(h, userid, useridl);
		  solv_chksum_add(h, p + 2, 5);
		  cs = solv_chksum_get(h, 0);
		  solv_chksum_free(h, 0);
		}
#endif
	    }
	  if (p[0] == 4)
	    {
	      int j, ql, haveissuer;
	      unsigned char *q;
	      unsigned int ex = 0;
#if 0
	      unsigned int scr = 0;
#endif
	      unsigned char issuer[8];

	      /* printf("V4 signature packet\n"); */
	      if (l < 6)
		continue;
	      if (p[1] != 0x10 && p[1] != 0x11 && p[1] != 0x12 && p[1] != 0x13 && p[1] != 0x1f)
		continue;
	      haveissuer = 0;
	      ex = 0;
	      q = p + 4;
	      for (j = 0; q && j < 2; j++)
		{
		  if (q + 2 > p + l)
		    {
		      q = 0;
		      break;
		    }
		  ql = q[0] << 8 | q[1];
		  q += 2;
		  if (q + ql > p + l)
		    {
		      q = 0;
		      break;
		    }
		  while (ql)
		    {
		      int sl;
		      /* decode sub-packet length */
		      x = *q++;
		      ql--;
		      if (x < 192)
			sl = x;
		      else if (x == 255)
			{
			  if (ql < 4 || q[0] != 0)
			    {
			      q = 0;
			      break;
			    }
			  sl = q[1] << 16 | q[2] << 8 | q[3];
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
		      /* printf("%d SIGSUB %d %d\n", j, x, sl); */
		      if (x == 16 && sl == 9 && !haveissuer)
			{
			  memcpy(issuer, q + 1, 8);
			  haveissuer = 1;
			}
#if 0
		      if (x == 2 && j == 0)
			scr = q[1] << 24 | q[2] << 16 | q[3] << 8 | q[4];
#endif
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
		      void *h = solv_chksum_create(htype);
		      unsigned char b[6], *cs;
		      unsigned int hl;

		      b[0] = 0x99;
		      b[1] = pubkeyl >> 8;
		      b[2] = pubkeyl;
		      solv_chksum_add(h, b, 3);
		      solv_chksum_add(h, pubkey, pubkeyl);
		      if (p[1] >= 0x10 && p[1] <= 0x13)
			{
			  b[0] = 0xb4;
			  b[1] = useridl >> 24;
			  b[2] = useridl >> 16;
			  b[3] = useridl >> 8;
			  b[4] = useridl;
			  solv_chksum_add(h, b, 5);
			  solv_chksum_add(h, userid, useridl);
			}
		      hl = 6 + (p[4] << 8 | p[5]);
		      solv_chksum_add(h, p, hl);
		      b[0] = 4;
		      b[1] = 0xff;
		      b[2] = hl >> 24;
		      b[3] = hl >> 16;
		      b[4] = hl >> 8;
		      b[5] = hl;
		      solv_chksum_add(h, b, 6);
		      cs = solv_chksum_get(h, 0);
		      solv_chksum_free(h, 0);
		    }
#endif
		  if (!memcmp(keyid, issuer, 8))
		    {
		      /* printf("SELF SIG cr %d ex %d\n", cr, ex); */
		      if (ex > maxex)
			maxex = ex;
		    }
		  else
		    {
		      /* printf("OTHER SIG cr %d ex %d\n", cr, ex); */
		    }
		}
	    }
	}
#if 0
      if (tag == 13)
	{
	  userid = solv_realloc(userid, l);
	  if (l)
	    memcpy(userid, p, l);
	  useridl = l;
	}
#endif
    }
  if (maxex)
    repodata_set_num(data, s - s->repo->pool->solvables, PUBKEY_EXPIRES, maxex);
#if 0
  solv_free(pubkey);
  solv_free(userid);
#endif
}

/* this is private to rpm, but rpm lacks an interface to retrieve
 * the values. Sigh. */
struct pgpDigParams_s {
    const char * userid;
    const unsigned char * hash;
#ifndef HAVE_PGPDIGGETPARAMS
    const char * params[4];
#endif
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

#ifndef HAVE_PGPDIGGETPARAMS
struct pgpDig_s {
    struct pgpDigParams_s signature;
    struct pgpDigParams_s pubkey;
};
#endif

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
  struct pgpDigParams_s *digpubkey;

  pkts = unarmor(pubkey, &pktsl);
  if (!pkts)
    return 0;
  setutf8string(data, s - s->repo->pool->solvables, SOLVABLE_DESCRIPTION, pubkey);
  parsekeydata(s, data, pkts, pktsl);
  /* only rpm knows how to do the release calculation, we don't dare
   * to recreate all the bugs */
#ifndef RPM5
  dig = pgpNewDig();
#else
  dig = pgpDigNew(RPMVSF_DEFAULT, 0);
#endif
  (void) pgpPrtPkts(pkts, pktsl, dig, 0);

#ifdef HAVE_PGPDIGGETPARAMS
  digpubkey = pgpDigGetParams(dig, PGPTAG_PUBLIC_KEY);
#else
  digpubkey = &dig->pubkey;
#endif
  btime = digpubkey->time[0] << 24 | digpubkey->time[1] << 16 | digpubkey->time[2] << 8 | digpubkey->signid[3];
  sprintf(evrbuf, "%02x%02x%02x%02x-%02x%02x%02x%02x", digpubkey->signid[4], digpubkey->signid[5], digpubkey->signid[6], digpubkey->signid[7], digpubkey->time[0], digpubkey->time[1], digpubkey->time[2], digpubkey->time[3]);

  repodata_set_num(data, s - s->repo->pool->solvables, SOLVABLE_BUILDTIME, btime);

  s->name = pool_str2id(pool, "gpg-pubkey", 1);
  s->evr = pool_str2id(pool, evrbuf, 1);
  s->arch = 1;
  for (i = 0; i < 8; i++)
    sprintf(keyid + 2 * i, "%02x", digpubkey->signid[i]);
  repodata_set_str(data, s - s->repo->pool->solvables, PUBKEY_KEYID, keyid);
  if (digpubkey->userid)
    setutf8string(data, s - s->repo->pool->solvables, SOLVABLE_SUMMARY, digpubkey->userid);
#ifndef RPM5
  (void)pgpFreeDig(dig);
#else
  (void)pgpDigFree(dig);
#endif
  solv_free((void *)pkts);
  return 1;
}

int
repo_add_rpmdb_pubkeys(Repo *repo, int flags)
{
  Pool *pool = repo->pool;
  Queue q;
  int i;
  char *str;
  Repodata *data;
  Solvable *s;
  const char *rootdir = 0;
  void *state;

  data = repo_add_repodata(repo, flags);
  if (flags & REPO_USE_ROOTDIR)
    rootdir = pool_get_rootdir(pool);
  state = rpm_state_create(repo->pool, rootdir);
  queue_init(&q);
  rpm_installedrpmdbids(state, "Name", "gpg-pubkey", &q);
  for (i = 0; i < q.count; i++)
    {
      void *handle;
      unsigned long long itime;

      handle = rpm_byrpmdbid(state, q.elements[i]);
      if (!handle)
	continue;
      str = rpm_query(handle, SOLVABLE_DESCRIPTION);
      if (!str)
	continue;
      s = pool_id2solvable(pool, repo_add_solvable(repo));
      pubkey2solvable(s, data, str);
      solv_free(str);
      itime = rpm_query_num(handle, SOLVABLE_INSTALLTIME, 0);
      if (itime)
        repodata_set_num(data, s - pool->solvables, SOLVABLE_INSTALLTIME, itime);
      if (!repo->rpmdbid)
	repo->rpmdbid = repo_sidedata_create(repo, sizeof(Id));
      repo->rpmdbid[s - pool->solvables - repo->start] = q.elements[i];
    }
  queue_free(&q);
  rpm_state_free(state);
  if (!(flags & REPO_NO_INTERNALIZE))
    repodata_internalize(data);
  return 0;
}

Id
repo_add_pubkey(Repo *repo, const char *key, int flags)
{
  Pool *pool = repo->pool;
  Repodata *data;
  Solvable *s;
  char *buf;
  int bufl, l, ll;
  FILE *fp;

  data = repo_add_repodata(repo, flags);
  buf = 0;
  bufl = 0;
  if ((fp = fopen(flags & REPO_USE_ROOTDIR ? pool_prepend_rootdir_tmp(pool, key) : key, "r")) == 0)
    {
      pool_error(pool, -1, "%s: %s", key, strerror(errno));
      return 0;
    }
  for (l = 0; ;)
    {
      if (bufl - l < 4096)
	{
	  bufl += 4096;
	  buf = solv_realloc(buf, bufl);
	}
      ll = fread(buf, 1, bufl - l, fp);
      if (ll < 0)
	{
	  fclose(fp);
	  pool_error(pool, -1, "%s: %s", key, strerror(errno));
	  return 0;
	}
      if (ll == 0)
	break;
      l += ll;
    }
  buf[l] = 0;
  fclose(fp);
  s = pool_id2solvable(pool, repo_add_solvable(repo));
  if (!pubkey2solvable(s, data, buf))
    {
      repo_free_solvable(repo, s - pool->solvables, 1);
      solv_free(buf);
      return 0;
    }
  solv_free(buf);
  if (!(flags & REPO_NO_INTERNALIZE))
    repodata_internalize(data);
  return s - pool->solvables;
}

