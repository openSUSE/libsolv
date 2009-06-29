/*
 * Copyright (c) 2009, Novell Inc.
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
#include <zlib.h>

#include "pool.h"
#include "repo.h"
#include "util.h"
#include "chksum.h"
#include "repo_deb.h"

static unsigned char *
decompress(unsigned char *in, int inl, int *outlp)
{
  z_stream strm;
  int outl, ret;
  unsigned char *out;

  memset(&strm, 0, sizeof(strm));
  strm.next_in = in;
  strm.avail_in = inl;
  out = sat_malloc(4096);
  strm.next_out = out;
  strm.avail_out = 4096;
  outl = 0;
  ret = inflateInit2(&strm, -MAX_WBITS);
  if (ret != Z_OK)
    {
      free(out);
      return 0;
    }
  for (;;)
    {
      if (strm.avail_out == 0)
	{
	  outl += 4096;
	  out = sat_realloc(out, outl + 4096);
	  strm.next_out = out + outl;
	  strm.avail_out = 4096;
	}
      ret = inflate(&strm, Z_NO_FLUSH);
      if (ret == Z_STREAM_END)
	break;
      if (ret != Z_OK)
	{
	  free(out);
	  return 0;
	}
    }
  outl += 4096 - strm.avail_out;
  inflateEnd(&strm);
  *outlp = outl;
  return out;
}

static void
control2solvable(Solvable *s, char *control)
{
}

void
repo_add_debs(Repo *repo, const char **debs, int ndebs, int flags)
{
  Pool *pool = repo->pool;
  Repodata *data;
  unsigned char buf[4096], *bp;
  int i, l, l2, vlen, clen, ctarlen;
  unsigned char *ctgz;
  unsigned char pkgid[16];
  unsigned char *ctar;
  int gotpkgid;
  FILE *fp;
  Solvable *s;
  struct stat stb;

  if (!(flags & REPO_REUSE_REPODATA))
    data = repo_add_repodata(repo, 0);
  else
    data = repo_last_repodata(repo);
  for (i = 0; i < ndebs; i++)
    {
      if ((fp = fopen(debs[i], "r")) == 0)
        {
          perror(debs[i]);
          continue;
        }
      if (fstat(fileno(fp), &stb))
        {
          perror("stat");
          continue;
        }
      l = fread(buf, 1, sizeof(buf), fp);
      if (l < 8 + 60 || strncmp(buf, "!<arch>\ndebian-binary   ", 8 + 16) != 0)
	{
	  fprintf(stderr, "%s: not a deb package\n", debs[i]);
	  fclose(fp);
          continue;
	}
      vlen = atoi(buf + 8 + 48);
      if (vlen < 0 || vlen > l)
	{
	  fprintf(stderr, "%s: not a deb package\n", debs[i]);
	  fclose(fp);
          continue;
	}
      vlen += vlen & 1;
      if (l < 8 + 60 + vlen + 60)
	{
	  fprintf(stderr, "%s: unhandled deb package\n", debs[i]);
	  fclose(fp);
          continue;
	}
      if (strncmp(buf + 8 + 60 + vlen, "control.tar.gz  ", 16) != 0)
	{
	  fprintf(stderr, "%s: control.tar.gz is not second entry\n", debs[i]);
	  fclose(fp);
          continue;
	}
      clen = atoi(buf + 8 + 60 + vlen + 48);
      if (clen <= 0)
	{
	  fprintf(stderr, "%s: control.tar.gz has illegal size\n", debs[i]);
	  fclose(fp);
          continue;
	}
      ctgz = sat_calloc(1, clen + 4);
      bp = buf + 8 + 60 + vlen + 60;
      l -= 8 + 60 + vlen + 60;
      if (l > clen)
	l = clen;
      if (l)
	memcpy(ctgz, bp, l);
      if (l < clen)
	{
	  if (fread(ctgz + l, clen - l, 1, fp) != 1)
	    {
	      fprintf(stderr, "%s: unexpected EOF\n", debs[i]);
	      sat_free(ctgz);
	      fclose(fp);
	      continue;
	    }
	}
      fclose(fp);
      gotpkgid = 0;
      if (flags & DEBS_ADD_WITH_PKGID)
	{
	  void *handle = sat_chksum_create(REPOKEY_TYPE_MD5);
	  sat_chksum_add(handle, ctgz, clen);
	  sat_chksum_free(handle, pkgid);
	  gotpkgid = 1;
	}
      if (ctgz[0] != 0x1f || ctgz[1] != 0x8b)
	{
	  fprintf(stderr, "%s: control.tar.gz is not gzipped\n", debs[i]);
	  sat_free(ctgz);
          continue;
	}
      if (ctgz[2] != 8 || (ctgz[3] & 0xe0) != 0)
	{
	  fprintf(stderr, "%s: control.tar.gz is compressed in a strange way\n", debs[i]);
	  sat_free(ctgz);
          continue;
	}
      bp = ctgz + 4;
      bp += 6;	/* skip time, xflags and OS code */
      if (ctgz[3] & 0x04)
	{
	  /* skip extra field */
	  l = bp[0] | bp[1] << 8;
	  bp += l + 2;
	  if (bp >= ctgz + clen)
	    {
	      fprintf(stderr, "%s: corrupt gzip\n", debs[i]);
	      sat_free(ctgz);
	      continue;
	    }
	}
      if (ctgz[3] & 0x08)	/* orig filename */
	while (*bp)
	  bp++;
      if (ctgz[3] & 0x10)	/* file comment */
	while (*bp)
	  bp++;
      if (ctgz[3] & 0x02)	/* header crc */
        bp += 2;
      if (bp >= ctgz + clen)
	{
	  fprintf(stderr, "%s: corrupt control.tar.gz\n", debs[i]);
	  sat_free(ctgz);
	  continue;
	}
      ctar = decompress(bp, ctgz + clen - bp, &ctarlen);
      sat_free(ctgz);
      if (!ctar)
	{
	  fprintf(stderr, "%s: corrupt control.tar.gz\n", debs[i]);
	  continue;
	}
      bp = ctar;
      l = ctarlen;
      while (l > 512)
	{
	  int j;
	  l2 = 0;
	  for (j = 124; j < 124 + 12; j++)
	    if (bp[j] >= '0' && bp[j] <= '7')
	      l2 = l2 * 8 + (bp[j] - '0');
	  if (!strcmp(bp, "./control"))
	    break;
	  l2 = 512 + ((l2 + 511) & ~511);
	  l -= l2;
	  bp += l2;
	}
      if (l <= 512 || l - 512 - l2 <= 0 || l2 <= 0)
	{
	  fprintf(stderr, "%s: control.tar.gz contains no ./control file\n", debs[i]);
	  free(ctar);
	  continue;
	}
      memmove(ctar, bp + 512, l2);
      ctar = sat_realloc(ctar, l2 + 1);
      ctar[l2] = 0;
      s = pool_id2solvable(pool, repo_add_solvable(repo));
      control2solvable(s, (char *)ctar);
      repodata_set_location(data, s - pool->solvables, 0, 0, debs[i]);
      repodata_set_num(data, s - pool->solvables, SOLVABLE_DOWNLOADSIZE, (unsigned int)((stb.st_size + 1023) / 1024));
      if (gotpkgid)
	repodata_set_bin_checksum(data, s - pool->solvables, SOLVABLE_PKGID, REPOKEY_TYPE_MD5, pkgid);
      sat_free(ctar);
    }
  if (!(flags & REPO_NO_INTERNALIZE))
    repodata_internalize(data);
}

int
main(int argc, const char **argv)
{
  Pool *pool = pool_create();
  Repo *repo = repo_create(pool, "debs2solv");
  repo_add_debs(repo, argv + 1, argc - 1, DEBS_ADD_WITH_PKGID);
  repo_write(repo, stdout, 0, 0, 0);
  pool_free(pool);
}
