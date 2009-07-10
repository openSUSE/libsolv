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

static unsigned int
makedeps(Repo *repo, char *deps, unsigned int olddeps, Id marker)
{
  Pool *pool = repo->pool;
  char *p, *n, *ne, *e, *ee;
  Id id, name, evr;
  int flags;

  while ((p = strchr(deps, ',')) != 0)
    {
      *p++ = 0;
      olddeps = makedeps(repo, deps, olddeps, marker);
      deps = p;
    }
  id = 0;
  p = deps;
  for (;;)
    {
      while (*p == ' ' || *p == '\t' || *p == '\n')
	p++;
      if (!*p || *p == '(')
	break;
      n = p;
      while (*p && *p != ' ' && *p != '\t' && *p != '\n' && *p != '(' && *p != '|')
	p++;
      ne = p;
      while (*p == ' ' || *p == '\t' || *p == '\n')
	p++;
      evr = 0;
      flags = 0;
      e = ee = 0;
      if (*p == '(')
	{
	  p++;
	  while (*p == ' ' || *p == '\t' || *p == '\n')
	    p++;
	  if (*p == '>')
	    flags |= REL_GT;
	  else if (*p == '=')
	    flags |= REL_EQ;
	  else if (*p == '<')
	    flags |= REL_LT;
	  if (flags)
	    {
	      p++;
	      if (*p == '>')
		flags |= REL_GT;
	      else if (*p == '=')
		flags |= REL_EQ;
	      else if (*p == '<')
		flags |= REL_LT;
	      else
		p--;
	      p++;
	    }
	  while (*p == ' ' || *p == '\t' || *p == '\n')
	    p++;
	  e = p;
	  while (*p && *p != ' ' && *p != '\t' && *p != '\n' && *p != ')')
	    p++;
	  ee = p;
	  while (*p && *p != ')')
	    p++;
	  if (*p)
	    p++;
	  while (*p == ' ' || *p == '\t' || *p == '\n')
	    p++;
	}
      name = strn2id(pool, n, ne - n, 1);
      if (e)
	{
	  evr = strn2id(pool, e, ee - e, 1);
	  name = rel2id(pool, name, evr, flags, 1);
	}
      if (!id)
	id = name;
      else
	id = rel2id(pool, id, name, REL_OR, 1);
      if (*p != '|')
	break;
      p++;
    }
  if (!id)
    return olddeps;
  return repo_addid_dep(repo, olddeps, id, marker);
}


/* put data from control file into the solvable */
/* warning: does inplace changes */
static void
control2solvable(Solvable *s, Repodata *data, char *control)
{
  Repo *repo = s->repo;
  Pool *pool = repo->pool;
  char *p, *q, *end, *tag;
  int x, l;

  p = control;
  while (*p)
    {
      p = strchr(p, '\n');
      if (!p)
	break;
      if (p[1] == ' ' || p[1] == '\t')
	{
	  char *q;
	  /* continuation line */
	  q = p - 1;
	  while (q >= control && *q == ' ' && *q == '\t')
	    q--;
	  l = q + 1 - control;
	  if (l)
	    memmove(p + 1 - l, control, l);
	  control = p + 1 - l;
	  p[1] = '\n';
	  p += 2;
	  continue;
	}
      end = p - 1;
      if (*p)
        *p++ = 0;
      /* strip trailing space */
      while (end >= control && *end == ' ' && *end == '\t')
	*end-- = 0;
      tag = control;
      control = p;
      q = strchr(tag, ':');
      if (!q || q - tag < 4)
	continue;
      *q++ = 0;
      while (*q == ' ' || *q == '\t')
	q++;
      x = '@' + (tag[0] & 0x1f);
      x = (x << 8) + '@' + (tag[1] & 0x1f);
      switch(x)
	{
	case 'A' << 8 | 'R':
	  if (!strcasecmp(tag, "architecture"))
	    s->arch = str2id(pool, q, 1);
	  break;
	case 'B' << 8 | 'R':
	  if (!strcasecmp(tag, "breaks"))
	    s->conflicts = makedeps(repo, q, s->conflicts, 0);
	  break;
	case 'C' << 8 | 'O':
	  if (!strcasecmp(tag, "conflicts"))
	    s->conflicts = makedeps(repo, q, s->conflicts, 0);
	  break;
	case 'D' << 8 | 'E':
	  if (!strcasecmp(tag, "depends"))
	    s->requires = makedeps(repo, q, s->requires, -SOLVABLE_PREREQMARKER);
	  else if (!strcasecmp(tag, "description"))
	    {
	      char *ld = strchr(q, '\n');
	      if (ld)
		{
		  *ld++ = 0;
	          repodata_set_str(data, s - pool->solvables, SOLVABLE_DESCRIPTION, ld);
		}
	      else
	        repodata_set_str(data, s - pool->solvables, SOLVABLE_DESCRIPTION, q);
	      repodata_set_str(data, s - pool->solvables, SOLVABLE_SUMMARY, q);
	    }
	  break;
	case 'E' << 8 | 'N':
	  if (!strcasecmp(tag, "enhances"))
	    s->enhances = makedeps(repo, q, s->enhances, 0);
	  break;
	case 'H' << 8 | 'O':
	  if (!strcasecmp(tag, "homepage"))
	    repodata_set_str(data, s - pool->solvables, SOLVABLE_URL, q);
	  break;
	case 'I' << 8 | 'N':
	  if (!strcasecmp(tag, "installed-size"))
	    repodata_set_num(data, s - pool->solvables, SOLVABLE_INSTALLSIZE, atoi(q));
	  break;
	case 'P' << 8 | 'A':
	  if (!strcasecmp(tag, "package"))
	    s->name = str2id(pool, q, 1);
	  break;
	case 'P' << 8 | 'R':
	  if (!strcasecmp(tag, "pre-depends"))
	    s->requires = makedeps(repo, q, s->requires, SOLVABLE_PREREQMARKER);
	  else if (!strcasecmp(tag, "provides"))
	    s->provides = makedeps(repo, q, s->provides, 0);
	  break;
	case 'R' << 8 | 'E':
	  if (!strcasecmp(tag, "replaces"))
	    s->obsoletes = makedeps(repo, q, s->conflicts, 0);
	  else if (!strcasecmp(tag, "recommends"))
	    s->recommends = makedeps(repo, q, s->recommends, 0);
	  break;
	case 'S' << 8 | 'U':
	  if (!strcasecmp(tag, "suggests"))
	    s->suggests = makedeps(repo, q, s->suggests, 0);
	  break;
	case 'V' << 8 | 'E':
	  if (!strcasecmp(tag, "version"))
	    s->evr = str2id(pool, q, 1);
	  break;
	}
    }
  if (!s->arch)
    s->arch = ARCH_ALL;
  if (!s->evr)
    s->evr = ID_EMPTY;
  if (s->name)
    s->provides = repo_addid_dep(repo, s->provides, rel2id(pool, s->name, s->evr, REL_EQ, 1), 0);
}

void
repo_add_debpackages(Repo *repo, FILE *fp, int flags)
{
  Pool *pool = repo->pool;
  Repodata *data;
  char *buf, *p;
  int bufl, l, ll;
  Solvable *s;

  data = repo_add_repodata(repo, flags);
  buf = sat_malloc(4096);
  bufl = 4096;
  l = 0;
  buf[l] = 0;
  p = buf;
  for (;;)
    {
      if (!(p = strchr(p, '\n')))
	{
	  int l3;
	  if (l + 1024 >= bufl)
	    {
	      buf = sat_realloc(buf, bufl + 4096);
	      bufl += 4096;
	      p = buf + l;
	      continue;
	    }
	  p = buf + l;
	  ll = fread(p, 1, bufl - l - 1, fp);
	  if (ll <= 0)
	    break;
	  p[ll] = 0;
	  while ((l3 = strlen(p)) < ll)
	    p[l3] = '\n';
	  l += ll;
	  continue;
	}
      p++;
      if (*p != '\n')
	continue;
      *p = 0;
      ll = p - buf + 1;
      s = pool_id2solvable(pool, repo_add_solvable(repo));
      control2solvable(s, data, buf);
      if (!s->name)
	repo_free_solvable_block(repo, s - pool->solvables, 1, 1);
      if (l > ll)
        memmove(buf, p + 1, l - ll);
      l -= ll;
      p = buf;
      buf[l] = 0;
    }
  if (l)
    {
      s = pool_id2solvable(pool, repo_add_solvable(repo));
      control2solvable(s, data, buf);
      if (!s->name)
	repo_free_solvable_block(repo, s - pool->solvables, 1, 1);
    }
  sat_free(buf);
  if (!(flags & REPO_NO_INTERNALIZE))
    repodata_internalize(data);
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

  data = repo_add_repodata(repo, flags);
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
      if (l < 8 + 60 || strncmp((char *)buf, "!<arch>\ndebian-binary   ", 8 + 16) != 0)
	{
	  fprintf(stderr, "%s: not a deb package\n", debs[i]);
	  fclose(fp);
          continue;
	}
      vlen = atoi((char *)buf + 8 + 48);
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
      if (strncmp((char *)buf + 8 + 60 + vlen, "control.tar.gz  ", 16) != 0)
	{
	  fprintf(stderr, "%s: control.tar.gz is not second entry\n", debs[i]);
	  fclose(fp);
          continue;
	}
      clen = atoi((char *)buf + 8 + 60 + vlen + 48);
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
	  if (!strcmp((char *)bp, "./control"))
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
      control2solvable(s, data, (char *)ctar);
      repodata_set_location(data, s - pool->solvables, 0, 0, debs[i]);
      if (S_ISREG(stb.st_mode))
        repodata_set_num(data, s - pool->solvables, SOLVABLE_DOWNLOADSIZE, (unsigned int)((stb.st_size + 1023) / 1024));
      if (gotpkgid)
	repodata_set_bin_checksum(data, s - pool->solvables, SOLVABLE_PKGID, REPOKEY_TYPE_MD5, pkgid);
      sat_free(ctar);
    }
  if (!(flags & REPO_NO_INTERNALIZE))
    repodata_internalize(data);
}
