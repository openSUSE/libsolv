/*
 * Copyright (c) 2024, SUSE LLC
 *
 * This program is licensed under the BSD license, read LICENSE.BSD
 * for further information
 */

#include <sys/types.h>
#include <sys/stat.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <errno.h>
#include <fcntl.h>

#include <zlib.h>
#include <zstd.h>

#include "pool.h"
#include "repo.h"
#include "util.h"
#include "chksum.h"
#include "solv_xfopen.h"
#include "tarhead.h"
#include "repo_apk.h"
#include "repo_apkv3.h"

static inline ssize_t
apk_fillbuf(unsigned char *buf, size_t count, int fd, FILE *fp)
{
  if (fp)
    {
      ssize_t rr = fread(buf, 1, count, fp);
      return rr <= 0 && ferror(fp) ? -1 : rr;
    }
  return read(fd, buf, count);
}

/* zlib decompression */

struct zstream {
  int fd;
  FILE *fp;
  int eof;
  z_stream zs;
  unsigned char buf[65536];
  void (*readcb)(void *, const void *, int);
  void *readcb_data;
  int doall;
};

static struct zstream *
apkz_open(int fd, FILE *fp, int raw)
{
  struct zstream *zstream = solv_calloc(1, sizeof(*zstream));
  zstream->fd = fd;
  zstream->fp = fp;
  if (inflateInit2(&zstream->zs, raw ? -15 : 15 + 32) != Z_OK)	/* 32: enable gzip */
    {
      solv_free(zstream);
      return 0;
    }
  return zstream;
}

static int
apkz_close(void *cookie)
{
  struct zstream *zstream = cookie;
  inflateEnd(&zstream->zs);
  if (zstream->fd != -1)
    close(zstream->fd);
  solv_free(zstream);
  return 0;
}

static inline ssize_t
apkz_fillbuf(struct zstream *zstream)
{
  ssize_t rr = apk_fillbuf(zstream->buf, sizeof(zstream->buf), zstream->fd, zstream->fp);
  if (rr >= 0)
    {
      zstream->zs.avail_in = rr;
      zstream->zs.next_in = zstream->buf;
    }
  return rr;
}

static int
apkz_reset(struct zstream *zstream)
{
  zstream->eof = 0;
  if (zstream->zs.avail_in == 0)
    {
      ssize_t rr = apkz_fillbuf(zstream);
      if (rr <= 0)
	return rr < 0 ? -1 : 0;
    }
  inflateReset(&zstream->zs);
  return 1;
}

static ssize_t
apkz_read(void *cookie, char *buf, size_t len)
{
  struct zstream *zstream = cookie;
  int r, eof = 0;
  ssize_t old_avail_in;

  if (!zstream)
    return -1;
  if (zstream->eof)
    return 0;
  zstream->zs.avail_out = len;
  zstream->zs.next_out = (unsigned char *)buf;
  for (;;)
    {
      if (zstream->zs.avail_in == 0)
	{
	  ssize_t rr = apkz_fillbuf(zstream);
	  if (rr < 0)
	    return rr;
	  if (rr == 0)
	    eof = 1;
	}
      old_avail_in = zstream->zs.avail_in;
      r = inflate(&zstream->zs, Z_NO_FLUSH);

      if ((r == Z_OK || r == Z_STREAM_END) && zstream->readcb)
	if (zstream->zs.avail_in < old_avail_in)
	  {
	    int l = old_avail_in - zstream->zs.avail_in;
	    zstream->readcb(zstream->readcb_data, (const void *)(zstream->zs.next_in - l), l);
	  }

      if (r == Z_STREAM_END)
	{
	  if (zstream->doall && apkz_reset(zstream) > 0)
	    continue;
	  zstream->eof = 1;
	  return len - zstream->zs.avail_out;
	}
      if (r != Z_OK)
	return -1;
      if (zstream->zs.avail_out == 0)
	return len;
      if (eof)
	return -1;
    }
}

/* zstd decompression */

struct zstdstream {
  int fd;
  FILE *fp;
  int eof;
  ZSTD_DCtx *ctx;
  ZSTD_inBuffer in;
  unsigned char buf[65536];
};

static struct zstdstream *
apkzstd_open(int fd, FILE *fp)
{
  struct zstdstream *zstdstream = solv_calloc(1, sizeof(*zstdstream));
  zstdstream->fd = fd;
  zstdstream->fp = fp;
  zstdstream->in.src = zstdstream->buf;
  zstdstream->in.size = zstdstream->in.pos = 0;
  if (!(zstdstream->ctx =  ZSTD_createDCtx()))
    {
      solv_free(zstdstream);
      return 0;
    }
  return zstdstream;
}

static int
apkzstd_close(void *cookie)
{
  struct zstdstream *zstdstream = cookie;
  ZSTD_freeDCtx(zstdstream->ctx);
  if (zstdstream->fd != -1)
    close(zstdstream->fd);
  solv_free(zstdstream);
  return 0;
}

static inline ssize_t
apkzstd_fillbuf(struct zstdstream *zstdstream)
{
  ssize_t rr = apk_fillbuf(zstdstream->buf, sizeof(zstdstream->buf), zstdstream->fd, zstdstream->fp);
  if (rr >= 0)
    {
      zstdstream->in.pos = 0;
      zstdstream->in.size = rr;
    }
  return rr;
}

static ssize_t
apkzstd_read(void *cookie, char *buf, size_t len)
{
  struct zstdstream *zstdstream = cookie;
  ZSTD_outBuffer out;
  int eof = 0;
  size_t r;

  if (!zstdstream)
    return -1;
  if (zstdstream->eof)
    return 0;
  out.dst = buf;
  out.pos = 0;
  out.size = len;
  for (;;)
    {
      if (zstdstream->in.pos >= zstdstream->in.size)
	{
	  ssize_t rr = apkzstd_fillbuf(zstdstream);
	  if (rr < 0)
	    return rr;
	  if (rr == 0)
	    eof = 1;
	}
      r = ZSTD_decompressStream(zstdstream->ctx, &out, &zstdstream->in);
      if (ZSTD_isError(r))
	return -1;
      if (out.pos == out.size || eof)
	return out.pos;
    }
}


/* apkv3 handling */

static FILE *
open_apkv3_error(Pool *pool, int fd, const char *fn, const char *msg)
{
  pool_error(pool, -1, "%s: %s", fn, msg);
  if (fd != -1)
    close(fd);
  return 0;
}

static FILE *
open_apkv3(Pool *pool, int fd, FILE *fp, const char *fn, int adbchar)
{
  unsigned char comp[2];
  char buf[4];
  FILE *cfp;

  comp[0] = comp[1] = 0;
  if (adbchar == 'c')
    {
      ssize_t r;
      if (fp)
	r = fread(comp, 2, 1, fp) == 1 ? 2 : feof(fp) ? 0 : -1;
      else
	r = read(fd, comp, 2);
      if (r != 2)
	return open_apkv3_error(pool, fd, fn, "compression header read error");
    }
  else if (adbchar == 'd')
    comp[0] = 1;
  else if (adbchar != '.')
    return open_apkv3_error(pool, fd, fn, "not an apkv3 file");
  if (comp[0] == 0)
    cfp = fp ? fp : fdopen(fd, "r");
  else if (comp[0] == 1)
    {
      struct zstream *zstream = apkz_open(fd, fp, 1);
      if (!zstream)
	return open_apkv3_error(pool, fd, fn, "zstream setup error");
      if ((cfp = solv_cookieopen(zstream, "r", apkz_read, 0, apkz_close)) == 0)
        return open_apkv3_error(pool, fd, fn, "zstream cookie setup error");
    }
  else if (comp[0] == 2)
    {
      struct zstdstream *zstdstream = apkzstd_open(fd, fp);
      if (!zstdstream)
	return open_apkv3_error(pool, fd, fn, "zstdstream setup error");
      if ((cfp = solv_cookieopen(zstdstream, "r", apkzstd_read, 0, apkzstd_close)) == 0)
	return open_apkv3_error(pool, fd, fn, "zstdstream cookie setup error");
    }
  else
    return open_apkv3_error(pool, fd, fn, "unsupported apkv3 compression");

  if (adbchar != '.')
    {
      if (fread(buf, 4, 1, cfp) != 1 || buf[0] != 'A' || buf[1] != 'D' || buf[2] != 'B' || buf[3] != '.')
	{
	  pool_error(pool, -1, "%s: not an apkv3 file", fn);
	  if (cfp != fp)
	    fclose(cfp);
	  return 0;
	}
    }
  return cfp;
}

static Id
add_apkv3_pkg(Repo *repo, Repodata *data, const char *fn, int flags, int fd, int adbchar)
{
  FILE *fp;
  Id p;
  if (!(fp = open_apkv3(repo->pool, fd, 0, fn, adbchar)))
    return 0;
  p = apkv3_add_pkg(repo, data, fn, fp, flags);
  fclose(fp);
  return p;
}

static int
add_apkv3_idx(Repo *repo, Repodata *data, FILE *fp, int flags, int adbchar)
{
  FILE *cfp;
  int r;
  if (!(cfp = open_apkv3(repo->pool, -1, fp, (flags & APK_ADD_INSTALLED_DB ? "installed database" : "package index"), adbchar)))
    return -1;
  r = apkv3_add_idx(repo, data, cfp, flags);
  fclose(cfp);
  return r;
}


/* apkv2 handling */

static void
add_deps(Repo *repo, Solvable *s, Id what, char *p)
{
  Pool *pool = repo->pool;
  Id oldwhat = what;
  Id supplements = 0;
  while (*p)
    {
      char *pn, *pv;
      int flags = 0;
      Id id;
      while (*p == ' ' || *p == '\t')
	p++;
      what = oldwhat;
      if (what == SOLVABLE_REQUIRES && *p == '!')
	{
	  what = SOLVABLE_CONFLICTS;
	  p++;
	}
      pn = p;
      while (*p && *p != ' ' && *p != '\t' && *p != '<' && *p != '>' && *p != '=' && *p != '~')
	p++;
      id =  pool_strn2id(pool, pn, p - pn, 1);
      for (; *p; p++)
	{
	  if (*p == '<')
	    flags |= REL_LT;
	  else if (*p == '>')
	    flags |= REL_GT;
	  else if (*p == '=')
	    flags |= REL_EQ;
	  else
	    break;
	}
      if (*p == '~')
	flags |= REL_EQ;
      if (flags)
	{
	  pv = p;
	  while (*p && *p != ' ' && *p != '\t')
	    p++;
	  id = pool_rel2id(pool, id, pool_strn2id(pool, pv, p - pv, 1), flags, 1);
	}
      if (what == SOLVABLE_PROVIDES)
        s->provides = repo_addid_dep(repo, s->provides, id, 0);
      else if (what == SOLVABLE_REQUIRES)
        s->requires = repo_addid_dep(repo, s->requires, id, 0);
      else if (what == SOLVABLE_CONFLICTS)
        s->conflicts = repo_addid_dep(repo, s->conflicts, id, 0);
      else if (what == SOLVABLE_SUPPLEMENTS)
	supplements = supplements ? pool_rel2id(pool, id, supplements, REL_AND, 1) : id;
    }
  if (supplements)
    s->supplements = repo_addid_dep(repo, s->supplements, supplements, 0);
}

Id
repo_add_apk_pkg(Repo *repo, const char *fn, int flags)
{
  Pool *pool = repo->pool;
  Repodata *data;
  int fd;
  FILE *fp;
  struct zstream *zstream;
  struct tarhead th;
  Solvable *s = 0;
  Chksum *pkgidchk = 0;
  Chksum *q1chk = 0;
  char *line = 0;
  size_t l, line_alloc = 0;
  int haveorigin = 0;
  char first[4];

  data = repo_add_repodata(repo, flags);
  if ((fd = open(flags & REPO_USE_ROOTDIR ? pool_prepend_rootdir_tmp(pool, fn) : fn, O_RDONLY)) == -1)
    {
      pool_error(pool, -1, "%s: %s", fn, strerror(errno));
      return 0;
    }
  if (read(fd, first, 4) == 4 && first[0] == 'A' && first[1] == 'D' && first[2] == 'B')
    return add_apkv3_pkg(repo, data, fn, flags, fd, first[3]);
  if (lseek(fd, 0, SEEK_SET)) 
    {
      pool_error(pool, -1, "%s: lseek: %s", fn, strerror(errno));
      close(fd);
      return 0;
    }
  zstream = apkz_open(fd, NULL, 0);
  if (!zstream)
    {
      pool_error(pool, -1, "%s: %s", fn, strerror(errno));
      close(fd);
      return 0;
    }
  if ((fp = solv_cookieopen(zstream, "r", apkz_read, 0, apkz_close)) == 0)
    {
      pool_error(pool, -1, "%s: %s", fn, strerror(errno));
      apkz_close(zstream);
      return 0;
    }

  /* skip signatures */
  while (getc(fp) != EOF)
    ;
  if (apkz_reset(zstream) != 1)
    {
      pool_error(pool, -1, "%s: unexpected EOF", fn);
      fclose(fp);
      return 0;
    }
  if ((flags & APK_ADD_WITH_HDRID) != 0)
    {
      q1chk = solv_chksum_create(REPOKEY_TYPE_SHA1);
      zstream->readcb_data = q1chk;
      zstream->readcb = (void *)solv_chksum_add;
    }
  clearerr(fp);
  tarhead_init(&th, fp);
  while (tarhead_next(&th) > 0)
    {
      if (th.type != 1 || strcmp(th.path, ".PKGINFO") != 0 || s)
	{
	  tarhead_skip(&th);
	  continue;
	}
      if (th.length > 10 * 1024 * 1024)
	{
	  pool_error(pool, -1, "%s: oversized .PKGINFO", fn);
	  break;
	}
      s = pool_id2solvable(pool, repo_add_solvable(repo));
      if (flags & APK_ADD_WITH_PKGID)
        pkgidchk = solv_chksum_create(REPOKEY_TYPE_MD5);
      while ((l = tarhead_gets(&th, &line, &line_alloc)) > 0)
	{
	  if (pkgidchk)
	    solv_chksum_add(pkgidchk, line, l);
	  l = strlen(line);
	  if (l && line[l - 1] == '\n')
	    line[--l] = 0;
	  if (l == 0 || line[0] == '#')
	    continue;
	  if (!strncmp(line, "pkgname = ", 10))
	    s->name = pool_str2id(pool, line + 10, 1);
	  else if (!strncmp(line, "pkgver = ", 9))
	    s->evr = pool_str2id(pool, line + 9, 1);
	  else if (!strncmp(line, "pkgdesc = ", 10))
	    {
	      repodata_set_str(data, s - pool->solvables, SOLVABLE_SUMMARY, line + 10);
	      repodata_set_str(data, s - pool->solvables, SOLVABLE_DESCRIPTION, line + 10);
	    }
	  else if (!strncmp(line, "url = ", 6))
	    repodata_set_str(data, s - pool->solvables, SOLVABLE_URL, line + 6);
	  else if (!strncmp(line, "builddate = ", 12))
	    repodata_set_num(data, s - pool->solvables, SOLVABLE_BUILDTIME, strtoull(line + 12, 0, 10));
	  else if (!strncmp(line, "packager = ", 11))
	    repodata_set_poolstr(data, s - pool->solvables, SOLVABLE_PACKAGER, line + 11);
	  else if (!strncmp(line, "size = ", 7))
	    repodata_set_num(data, s - pool->solvables, SOLVABLE_INSTALLSIZE, strtoull(line + 7, 0, 10));
	  else if (!strncmp(line, "arch = ", 7))
	    s->arch = pool_str2id(pool, line + 7, 1);
	  else if (!strncmp(line, "license = ", 10))
	    repodata_set_poolstr(data, s - pool->solvables, SOLVABLE_LICENSE, line + 10);
	  else if (!strncmp(line, "origin = ", 9))
	    {
	      if (s->name && !strcmp(line + 9,  pool_id2str(pool, s->name)))
	        repodata_set_void(data, s - pool->solvables, SOLVABLE_SOURCENAME);
	      else
		repodata_set_id(data, s - pool->solvables, SOLVABLE_SOURCENAME, pool_str2id(pool, line + 9, 1));
	      haveorigin = 1;
	    }
	  else if (!strncmp(line, "depend = ", 9))
	    add_deps(repo, s, SOLVABLE_REQUIRES, line + 9);
	  else if (!strncmp(line, "provides = ", 11))
	    add_deps(repo, s, SOLVABLE_PROVIDES, line + 11);
	  else if (!strncmp(line, "install_if = ", 13))
	    add_deps(repo, s, SOLVABLE_SUPPLEMENTS, line + 13);
	}
    }
  solv_free(line);
  tarhead_free(&th);
  fclose(fp);
  if (s && !s->name)
    {
      pool_error(pool, -1, "%s: package has no name", fn);
      s = solvable_free(s, 1);
    }
  if (s)
    {
      if (!s->arch)
	s->arch = ARCH_NOARCH;
      if (!s->evr)
	s->evr = ID_EMPTY;
      if (s->name)
	s->provides = repo_addid_dep(repo, s->provides, pool_rel2id(pool, s->name, s->evr, REL_EQ, 1), 0);
      if (s->name && !haveorigin)
	repodata_set_void(data, s - pool->solvables, SOLVABLE_SOURCENAME);
      if (pkgidchk)
	{
	  unsigned char pkgid[16];
	  solv_chksum_free(pkgidchk, pkgid);
	  repodata_set_bin_checksum(data, s - pool->solvables, SOLVABLE_PKGID, REPOKEY_TYPE_MD5, pkgid);
	  pkgidchk = 0;
	}
      if (q1chk)
	{
	  unsigned char hdrid[20];
	  solv_chksum_free(q1chk, hdrid);
	  repodata_set_bin_checksum(data, s - pool->solvables, SOLVABLE_HDRID, REPOKEY_TYPE_SHA1, hdrid);
	  q1chk = 0;
	}
      if (!(flags & REPO_NO_LOCATION))
	repodata_set_location(data, s - pool->solvables, 0, 0, fn);
    }
  if (q1chk)
    solv_chksum_free(q1chk, 0);
  if (pkgidchk)
    solv_chksum_free(pkgidchk, 0);
  if (!(flags & REPO_NO_INTERNALIZE))
    repodata_internalize(data);
  return s ? s - pool->solvables : 0;
}

static void
apk_add_hdrid(Repodata *data, Id p, char *idstr)
{
  size_t l = strlen(idstr);
  unsigned char chksum[33], *cp = chksum;

  if (idstr[0] != 'Q')
    return;
  if ((idstr[1] == '1' && (l == 30 || l == 46)) || (idstr[1] == '2' && l == 46))
    {
      int xpos = idstr[1] == '2' ? 43 : 27;
      int i, v;

      l -= 2;
      idstr += 2;
      for (i = v = 0; i < l; i++)
	{
	  int x = idstr[i];
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
	  else if (x == '=' && i == xpos) 
	    x = 0;
	  else
	    return;
	  v = v << 6 | x;
	  if ((i & 3) == 3)
	    {
	      *cp++ = v >> 16;
	      *cp++ = v >> 8;
	      if (i != xpos)
		  *cp++ = v;
	      v = 0;
	    }
	}
      repodata_set_bin_checksum(data, p, SOLVABLE_HDRID, l == 28 ? REPOKEY_TYPE_SHA1 : REPOKEY_TYPE_SHA256, chksum);
    }
}

static void
apk_process_index(Repo *repo, Repodata *data, struct tarhead *th, int flags)
{
  Pool *pool = repo->pool;
  Solvable *s = 0;
  char *line = 0;
  size_t l, line_alloc = 0;
  int haveorigin = 0;

  for (;;)
    {
      l = tarhead_gets(th, &line, &line_alloc);
      if (s && (l == 0 || (l == 1 && line[0] == '\n')))
	{
	  /* finish old solvable */
	  if (!s->name)
	    repo_free_solvable(repo, s - pool->solvables, 1);
	  else
	    {
	      if (!s->arch)
		s->arch = ARCH_NOARCH;
	      if (!s->evr)
		s->evr = ID_EMPTY;
	      if (s->name)
		s->provides = repo_addid_dep(repo, s->provides, pool_rel2id(pool, s->name, s->evr, REL_EQ, 1), 0);
	      if (s->name && !haveorigin)
		repodata_set_void(data, s - pool->solvables, SOLVABLE_SOURCENAME);
	    }
	  s = 0;
	}

      if (l == 0)
	break;

      l = strlen(line);
      if (l && line[l - 1] == '\n')
	line[--l] = 0;
      if (l < 2 || line[1] != ':')
	continue;
      if (!s)
	{
	  s = pool_id2solvable(pool, repo_add_solvable(repo));
	  haveorigin = 0;
	}
      if (line[0] == 'P')
	s->name = pool_str2id(pool, line + 2, 1);
      else if (line[0] == 'V')
	s->evr = pool_str2id(pool, line + 2, 1);
      else if (line[0] == 'T')
	{
	  repodata_set_str(data, s - pool->solvables, SOLVABLE_SUMMARY, line + 2);
	  repodata_set_str(data, s - pool->solvables, SOLVABLE_DESCRIPTION, line + 2);
	}
      else if (line[0] == 'U')
	repodata_set_str(data, s - pool->solvables, SOLVABLE_URL, line + 2);
      else if (line[0] == 't')
	repodata_set_num(data, s - pool->solvables, SOLVABLE_BUILDTIME, strtoull(line + 2, 0, 10));
      else if (line[0] == 'I')
	repodata_set_num(data, s - pool->solvables, SOLVABLE_INSTALLSIZE, strtoull(line + 2, 0, 10));
      else if (line[0] == 'S')
	repodata_set_num(data, s - pool->solvables, SOLVABLE_DOWNLOADSIZE, strtoull(line + 2, 0, 10));
      else if (line[0] == 'A')
	s->arch = pool_str2id(pool, line + 2, 1);
      else if (line[0] == 'L')
	repodata_set_poolstr(data, s - pool->solvables, SOLVABLE_LICENSE, line + 2);
      else if (line[0] == 'o')
	{
	  if (s->name && !strcmp(line + 2,  pool_id2str(pool, s->name)))
	    repodata_set_void(data, s - pool->solvables, SOLVABLE_SOURCENAME);
	  else
	    repodata_set_id(data, s - pool->solvables, SOLVABLE_SOURCENAME, pool_str2id(pool, line + 2, 1));
	  haveorigin = 1;
	}
      else if (line[0] == 'D')
	add_deps(repo, s, SOLVABLE_REQUIRES, line + 2);
      else if (line[0] == 'p')
	add_deps(repo, s, SOLVABLE_PROVIDES, line + 2);
      else if (line[0] == 'i')
	add_deps(repo, s, SOLVABLE_SUPPLEMENTS, line + 2);
      else if (line[0] == 'C' && (flags & APK_ADD_WITH_HDRID) != 0)
	apk_add_hdrid(data, s - pool->solvables, line + 2);
    }
  solv_free(line);
}

int
repo_add_apk_repo(Repo *repo, FILE *fp, int flags)
{
  char first[4];
  struct tarhead th;
  Repodata *data;
  int c;
  int close_fp = 0;

  data = repo_add_repodata(repo, flags);

  /* peek into first byte to find out if this is a compressed file */
  c = fgetc(fp);
  if (c == EOF)
    return (flags & APK_ADD_INSTALLED_DB) != 0 ? 0 : -1;	/* an empty file is allowed for the v2 db */
  ungetc(c, fp);

  if (c == 'A')
    {
     if (fread(first, 4, 1, fp) != 1)
	return -1;
     if (first[0] == 'A' && first[1] == 'D' && first[2] == 'B')
       return add_apkv3_idx(repo, data, fp, flags, first[3]);
     if ((flags & APK_ADD_INSTALLED_DB) == 0)
      return -1;	/* not a tar file */
    }

  if (c == 0x1f)
    {
      struct zstream *zstream;
      /* gzip compressed, setup decompression */
      zstream = apkz_open(-1, fp, 0);
      if (!zstream)
	return -1;
      zstream->doall = 1;
      if ((fp = solv_cookieopen(zstream, "r", apkz_read, 0, apkz_close)) == 0)
	{
	  apkz_close(zstream);
	  return -1;
        }
      close_fp = 1;
    }

  tarhead_init(&th, fp);
  if (c == 'A')
    {
      /* initialize input buffer with 4 bytes we already read */
      memcpy(th.blk, first, 4);
      th.end = 4;
    }
  
  if ((flags & APK_ADD_INSTALLED_DB) != 0)
    apk_process_index(repo, data, &th, flags);
  else
    {
      while (tarhead_next(&th) > 0)
	{
	  if (th.type != 1 || strcmp(th.path, "APKINDEX") != 0)
	    tarhead_skip(&th);
	  else
	    apk_process_index(repo, data, &th, flags);
	}
    }
  tarhead_free(&th);
  if (close_fp)
    fclose(fp);
  return 0;
}

