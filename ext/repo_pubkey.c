/*
 * Copyright (c) 2007-2013, Novell Inc.
 *
 * This program is licensed under the BSD license, read LICENSE.BSD
 * for further information
 */

/*
 * repo_pubkey
 *
 * support for pubkey reading
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
#ifdef ENABLE_PGPVRFY
#include "solv_pgpvrfy.h"
#endif

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
unarmor(char *pubkey, int *pktlp, char *startstr, char *endstr)
{
  char *p;
  int l, eof;
  unsigned char *buf, *bp;
  unsigned int v;

  *pktlp = 0;
  if (!pubkey)
    return 0;
  l = strlen(startstr);
  while (strncmp(pubkey, startstr, l) != 0)
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
  if (strncmp(pubkey, endstr, strlen(endstr)) != 0)
    {
      solv_free(buf);
      return 0;
    }
  *pktlp = bp - buf;
  return buf;
}

struct pgpsig {
  int type;
  Id hashalgo;
  unsigned char issuer[8];
  int haveissuer;
  unsigned int created;
  unsigned int expires;
  unsigned int keyexpires;
  unsigned char *sigdata;
  int sigdatal;
  int mpioff;
};

static Id
pgphashalgo2type(int algo)
{
  if (algo == 1)
    return REPOKEY_TYPE_MD5;
  if (algo == 2)
    return REPOKEY_TYPE_SHA1;
  if (algo == 8)
    return REPOKEY_TYPE_SHA256;
  return 0;
}

static void
createsigdata(struct pgpsig *sig, unsigned char *p, int l, unsigned char *pubkey, int pubkeyl, unsigned char *userid, int useridl, void *h)
{
  int type = sig->type;
  unsigned char b[6];
  const unsigned char *cs;
  int csl;

  if (!sig->mpioff || l <= sig->mpioff)
    return;
  if ((type >= 0x10 && type <= 0x13) || type == 0x1f || type == 0x18 || type == 0x20 || type == 0x28)
    {
      b[0] = 0x99;
      b[1] = pubkeyl >> 8;
      b[2] = pubkeyl;
      solv_chksum_add(h, b, 3);
      solv_chksum_add(h, pubkey, pubkeyl);
    }
  if ((type >= 0x10 && type <= 0x13))
    {
      if (p[0] != 3)
	{
	  b[0] = 0xb4;
	  b[1] = useridl >> 24;
	  b[2] = useridl >> 16;
	  b[3] = useridl >> 8;
	  b[4] = useridl;
	  solv_chksum_add(h, b, 5);
	}
      solv_chksum_add(h, userid, useridl);
    }
  /* add trailer */
  if (p[0] == 3)
    solv_chksum_add(h, p + 2, 5);
  else
    {
      int hl = 6 + (p[4] << 8 | p[5]);
      solv_chksum_add(h, p, hl);
      b[0] = 4;
      b[1] = 0xff;
      b[2] = hl >> 24;
      b[3] = hl >> 16;
      b[4] = hl >> 8;
      b[5] = hl;
      solv_chksum_add(h, b, 6);
    }
  cs = solv_chksum_get(h, &csl);
  if (cs[0] == p[sig->mpioff - 2] && cs[1] == p[sig->mpioff - 1])
    {
      int ml = l - sig->mpioff;
      sig->sigdata = solv_malloc(2 + csl + ml);
      sig->sigdatal = 2 + csl + ml;
      sig->sigdata[0] = p[0] == 3 ? p[15] : p[2];
      sig->sigdata[1] = p[0] == 3 ? p[16] : p[3];
      memcpy(sig->sigdata + 2, cs, csl);
      memcpy(sig->sigdata + 2 + csl, p + sig->mpioff, ml);
    }
}

static void
parsesigpacket(struct pgpsig *sig, unsigned char *p, int l)
{
  sig->type = -1;
  if (p[0] == 3)
    {
      /* printf("V3 signature packet\n"); */
      if (l <= 19 || p[1] != 5)
	return;
      sig->type = p[2];
      sig->haveissuer = 1;
      memcpy(sig->issuer, p + 7, 8);
      sig->created = p[3] << 24 | p[4] << 16 | p[5] << 8 | p[6];
      sig->hashalgo = p[16];
      sig->mpioff = 19;
    }
  else if (p[0] == 4)
    {
      int j, ql, x;
      unsigned char *q;

      /* printf("V4 signature packet\n"); */
      if (l < 6)
	return;
      sig->type = p[1];
      sig->hashalgo = p[3];
      q = p + 4;
      sig->keyexpires = -1;
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
	      if (x == 16 && sl == 9 && !sig->haveissuer)
		{
		  sig->haveissuer = 1;
		  memcpy(sig->issuer, q + 1, 8);
		}
	      if (x == 2 && j == 0)
		sig->created = q[1] << 24 | q[2] << 16 | q[3] << 8 | q[4];
	      if (x == 3 && j == 0)
		sig->expires = q[1] << 24 | q[2] << 16 | q[3] << 8 | q[4];
	      if (x == 9 && j == 0)
		sig->keyexpires = q[1] << 24 | q[2] << 16 | q[3] << 8 | q[4];
	      q += sl;
	      ql -= sl;
	    }
	}
      if (q && q - p + 2 < l)
	sig->mpioff = q - p + 2;
    }
}

static int
parsepkgheader(unsigned char *p, int pl, int *tagp, int *pktlp)
{
  unsigned char *op = p;
  int x, l;

  if (!pl)
    return 0;
  x = *p++;
  pl--;
  if (!(x & 128) || pl <= 0)
    return 0;
  if ((x & 64) == 0)
    {
      *tagp = (x & 0x3c) >> 2;		/* old format */
      x = 1 << (x & 3);
      if (x > 4 || pl < x || (x == 4 && p[0]))
	return 0;
      pl -= x;
      for (l = 0; x--;)
	l = l << 8 | *p++;
    }
  else
    {
      *tagp = (x & 0x3f);		/* new format */
      x = *p++;
      pl--;
      if (x < 192)
	l = x;
      else if (x >= 192 && x < 224)
	{
	  if (pl <= 0)
	    return 0;
	  l = ((x - 192) << 8) + *p++ + 192;
	  pl--;
	}
      else if (x == 255)
	{
	  if (pl <= 4 || p[0] != 0)	/* sanity: p[0] must be zero */
	    return 0;
	  l = p[1] << 16 | p[2] << 8 | p[3];
	  p += 4;
	  pl -= 4;
	}
      else
	return 0;
    }
  if (l > pl)
    return 0;
  *pktlp = l;
  return p - op;
}


static void
parsekeydata(Solvable *s, Repodata *data, unsigned char *p, int pl)
{
  int tag, l;
  unsigned char keyid[8];
  unsigned int kcr = 0, maxex = 0, maxsigcr = 0;
  unsigned char *pubkey = 0;
  int pubkeyl = 0;
  unsigned char *userid = 0;
  int useridl = 0;
  unsigned char *pubdata = 0;
  int pubdatal = 0;

  for (; pl; p += l, pl -= l)
    {
      int hl = parsepkgheader(p, pl, &tag, &l);
      if (!hl)
	break;
      p += hl;
      pl -= hl;
      if (tag == 6)
	{
	  if (pubkey)
	    break;	/* one key at a time, please */
	  pubkey = solv_malloc(l);
	  if (l)
	    memcpy(pubkey, p, l);
	  pubkeyl = l;
	  if (p[0] == 3 && l >= 10)
	    {
	      unsigned int ex;
	      void *h;
	      maxsigcr = kcr = p[1] << 24 | p[2] << 16 | p[3] << 8 | p[4];
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
		  int ql, ql2;
		  unsigned char fp[16];
		  char fpx[32 + 1];
		  unsigned char *q;

		  ql = ((p[8] << 8 | p[9]) + 7) / 8;		/* length of public modulus */
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
			  solv_bin2hex(fp, 16, fpx);
			  repodata_set_str(data, s - s->repo->pool->solvables, PUBKEY_FINGERPRINT, fpx);
			}
		    }
		  pubdata = p + 7;
		  pubdatal = l - 7;
		}
	    }
	  else if (p[0] == 4 && l >= 6)
	    {
	      void *h;
	      unsigned char hdr[3];
	      unsigned char fp[20];
	      char fpx[40 + 1];

	      maxsigcr = kcr = p[1] << 24 | p[2] << 16 | p[3] << 8 | p[4];
	      hdr[0] = 0x99;
	      hdr[1] = l >> 8;
	      hdr[2] = l;
	      /* fingerprint is the sha1 over the packet */
	      h = solv_chksum_create(REPOKEY_TYPE_SHA1);
	      solv_chksum_add(h, hdr, 3);
	      solv_chksum_add(h, p, l);
	      solv_chksum_free(h, fp);
	      solv_bin2hex(fp, 20, fpx);
	      repodata_set_str(data, s - s->repo->pool->solvables, PUBKEY_FINGERPRINT, fpx);
	      memcpy(keyid, fp + 12, 8);	/* keyid is last 64 bits of fingerprint */
	      pubdata = p + 5;
	      pubdatal = l - 5;
	    }
	}
      if (tag == 2)
	{
	  struct pgpsig sig;
	  Id htype;
	  if (!pubdata)
	    continue;
	  memset(&sig, 0, sizeof(sig));
	  parsesigpacket(&sig, p, l);
	  if (!sig.haveissuer || !((sig.type >= 0x10 && sig.type <= 0x13) || sig.type == 0x1f))
	    continue;
	  if (sig.type >= 0x10 && sig.type <= 0x13 && !userid)
	    continue;
	  htype = pgphashalgo2type(sig.hashalgo);
	  if (htype && sig.mpioff)
	    {
	      void *h = solv_chksum_create(htype);
	      createsigdata(&sig, p, l, pubkey, pubkeyl, userid, useridl, h);
	      solv_chksum_free(h, 0);
	    }
	  if (!memcmp(keyid, sig.issuer, 8))
	    {
#ifdef ENABLE_PGPVRFY
	      /* found self sig, verify */
	      if (solv_pgpvrfy(pubdata, pubdatal, sig.sigdata, sig.sigdatal))
#endif
		{
		  if (sig.keyexpires && maxex != -1)
		    {
		      if (sig.keyexpires == -1)
			maxex = -1;
		      else if (sig.keyexpires + kcr > maxex)
			maxex = sig.keyexpires + kcr;
		    }
		  if (sig.created > maxsigcr)
		    maxsigcr = sig.created;
		}
	    }
	  else
	    {
	      char issuerstr[17];
	      Id shandle = repodata_new_handle(data);
	      solv_bin2hex(sig.issuer, 8, issuerstr);
	      repodata_set_str(data, shandle, SIGNATURE_ISSUER, issuerstr);
	      if (sig.created)
	        repodata_set_num(data, shandle, SIGNATURE_TIME, sig.created);
	      if (sig.expires)
	        repodata_set_num(data, shandle, SIGNATURE_EXPIRES, sig.created + sig.expires);
	      if (sig.sigdata)
	        repodata_set_binary(data, shandle, SIGNATURE_DATA, sig.sigdata, sig.sigdatal);
	      repodata_add_flexarray(data, s - s->repo->pool->solvables, PUBKEY_SIGNATURES, shandle);
	    }
	  solv_free(sig.sigdata);
	}
      if (tag == 13)
	{
	  userid = solv_realloc(userid, l);
	  if (l)
	    memcpy(userid, p, l);
	  useridl = l;
	}
    }
  if (kcr)
    repodata_set_num(data, s - s->repo->pool->solvables, SOLVABLE_BUILDTIME, kcr);
  if (maxex && maxex != -1)
    repodata_set_num(data, s - s->repo->pool->solvables, PUBKEY_EXPIRES, maxex);
  s->name = pool_str2id(s->repo->pool, "gpg-pubkey", 1);
  s->evr = 1;
  s->arch = 1;
  if (userid && useridl)
    {
      char *useridstr = solv_malloc(useridl + 1);
      memcpy(useridstr, userid, useridl);
      useridstr[useridl] = 0;
      setutf8string(data, s - s->repo->pool->solvables, SOLVABLE_SUMMARY, useridstr);
      free(useridstr);
    }
  if (pubdata)
    {
      char keyidstr[17];
      solv_bin2hex(keyid, 8, keyidstr);
      repodata_set_str(data, s - s->repo->pool->solvables, PUBKEY_KEYID, keyidstr);
    }
  if (pubdata)
    {
      /* build rpm-style evr */
      char evr[8 + 1 + 8 + 1];
      solv_bin2hex(keyid + 4, 4, evr);
      sprintf(evr + 8, "-%08x", maxsigcr);
      s->evr = pool_str2id(s->repo->pool, evr, 1);
      /* set data blob */
      repodata_set_binary(data, s - s->repo->pool->solvables, PUBKEY_DATA, pubdata, pubdatal);
    }
  solv_free(pubkey);
  solv_free(userid);
}


#ifdef ENABLE_RPMDB

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


/* only rpm knows how to do the release calculation, we don't dare
 * to recreate all the bugs in libsolv */
static void
parsekeydata_rpm(Solvable *s, Repodata *data, unsigned char *pkts, int pktsl)
{
  Pool *pool = s->repo->pool;
  struct pgpDigParams_s *digpubkey;
  pgpDig dig = 0;
  char keyid[16 + 1];
  char evrbuf[8 + 1 + 8 + 1];
  unsigned int btime;

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
  if (digpubkey)
    {
      btime = digpubkey->time[0] << 24 | digpubkey->time[1] << 16 | digpubkey->time[2] << 8 | digpubkey->time[3];
      solv_bin2hex(digpubkey->signid, 8, keyid);
      solv_bin2hex(digpubkey->signid + 4, 4, evrbuf);
      evrbuf[8] = '-';
      solv_bin2hex(digpubkey->time, 4, evrbuf + 9);
      s->evr = pool_str2id(pool, evrbuf, 1);
      repodata_set_str(data, s - s->repo->pool->solvables, PUBKEY_KEYID, keyid);
      if (digpubkey->userid)
	setutf8string(data, s - s->repo->pool->solvables, SOLVABLE_SUMMARY, digpubkey->userid);
      if (btime)
	repodata_set_num(data, s - s->repo->pool->solvables, SOLVABLE_BUILDTIME, btime);
    }
#ifndef RPM5
  (void)pgpFreeDig(dig);
#else
  (void)pgpDigFree(dig);
#endif
}

#endif	/* ENABLE_RPMDB */

static int
pubkey2solvable(Solvable *s, Repodata *data, char *pubkey)
{
  unsigned char *pkts;
  int pktsl;

  pkts = unarmor(pubkey, &pktsl, "-----BEGIN PGP PUBLIC KEY BLOCK-----", "-----END PGP PUBLIC KEY BLOCK-----");
  if (!pkts)
    {
      pool_error(s->repo->pool, 0, "unarmor failure");
      return 0;
    }
  setutf8string(data, s - s->repo->pool->solvables, SOLVABLE_DESCRIPTION, pubkey);
  parsekeydata(s, data, pkts, pktsl);
#ifdef ENABLE_RPMDB
  parsekeydata_rpm(s, data, pkts, pktsl);
#endif
  solv_free((void *)pkts);
  return 1;
}

#ifdef ENABLE_RPMDB

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

#endif

static char *
solv_slurp(FILE *fp, int *lenp)
{
  int l, ll;
  char *buf = 0;
  int bufl = 0;

  for (l = 0; ; l += ll)
    {
      if (bufl - l < 4096)
	{
	  bufl += 4096;
	  buf = solv_realloc(buf, bufl);
	}
      ll = fread(buf + l, 1, bufl - l, fp);
      if (ll < 0)
	{
	  buf = solv_free(buf);
	  l = 0;
	  break;
	}
      if (ll == 0)
	{
	  buf[l] = 0;	/* always zero-terminate */
	  break;
	}
    }
  if (lenp)
    *lenp = l;
  return buf;
}

Id
repo_add_pubkey(Repo *repo, const char *key, int flags)
{
  Pool *pool = repo->pool;
  Repodata *data;
  Solvable *s;
  char *buf;
  FILE *fp;

  data = repo_add_repodata(repo, flags);
  buf = 0;
  if ((fp = fopen(flags & REPO_USE_ROOTDIR ? pool_prepend_rootdir_tmp(pool, key) : key, "r")) == 0)
    {
      pool_error(pool, -1, "%s: %s", key, strerror(errno));
      return 0;
    }
  if ((buf = solv_slurp(fp, 0)) == 0)
    {
      pool_error(pool, -1, "%s: %s", key, strerror(errno));
      fclose(fp);
      return 0;
    }
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

static int
is_sig_packet(unsigned char *sig, int sigl)
{
  if (!sigl)
    return 0;
  if ((sig[0] & 0x80) == 0 || (sig[0] & 0x40 ? sig[0] & 0x3f : sig[0] >> 2 & 0x0f) != 2)
    return 0;
  return 1;
}

Id
solv_parse_sig(FILE *fp, unsigned char **sigpkgp, int *sigpkglp, char *keyidstr)
{
  unsigned char *sig;
  int sigl, hl, tag, pktl;
  struct pgpsig pgpsig;
  Id htype;

  if (sigpkgp)
    {
      *sigpkgp = 0;
      *sigpkglp = 0;
    }
  if ((sig = (unsigned char *)solv_slurp(fp, &sigl)) == 0)
    return 0;
  if (!is_sig_packet(sig, sigl))
    {
      /* not a raw sig, check armored */
      unsigned char *nsig;
      nsig = unarmor((char *)sig, &sigl, "-----BEGIN PGP SIGNATURE-----", "-----END PGP SIGNATURE-----");
      solv_free(sig);
      if (!nsig)
	return 0;
      sig = nsig;
      if (!is_sig_packet(sig, sigl))
	{
	  solv_free(sig);
	  return 0;
	}
    }
  hl = parsepkgheader(sig, sigl, &tag, &pktl);
  if (!hl || tag != 2)
    {
      solv_free(sig);
      return 0;
    }
  memset(&pgpsig, 0, sizeof(pgpsig));
  parsesigpacket(&pgpsig, sig + hl, pktl);
  htype = pgphashalgo2type(pgpsig.hashalgo);
  if (pgpsig.type != 0 || !htype)
    {
      solv_free(sig);
      return 0;
    }
  if (sigpkgp)
    {
      *sigpkgp = sig + hl;
      *sigpkglp = pktl;
    }
  else
    solv_free(sig);
  if (keyidstr)
    solv_bin2hex(pgpsig.issuer, 8, keyidstr);
  return htype;
}

#ifdef ENABLE_PGPVRFY
int
solv_verify_sig(const unsigned char *pubdata, int pubdatal, unsigned char *sigpkg, int sigpkgl, void *chk)
{
  struct pgpsig pgpsig;
  int res;
  Id htype;

  memset(&pgpsig, 0, sizeof(pgpsig));
  parsesigpacket(&pgpsig, sigpkg, sigpkgl);
  if (pgpsig.type != 0)
    return 0;
  htype = pgphashalgo2type(pgpsig.hashalgo);
  if (htype != solv_chksum_get_type(chk))
     return 0;	/* wrong hash type? */
  createsigdata(&pgpsig, sigpkg, sigpkgl, 0, 0, 0, 0, chk);
  if (!pgpsig.sigdata)
    return 0;
  res = solv_pgpvrfy(pubdata, pubdatal, pgpsig.sigdata, pgpsig.sigdatal);
  solv_free(pgpsig.sigdata);
  return res;
}
#endif

