/*
 * Copyright (c) 2012, Novell Inc.
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
#include <dirent.h>

#include "pool.h"
#include "repo.h"
#include "util.h"
#include "chksum.h"
#include "solv_xfopen.h"
#include "tarhead.h"
#include "repo_arch.h"

static Offset
adddep(Repo *repo, Offset olddeps, char *line)
{
  Pool *pool = repo->pool;
  char *p;
  Id id;

  while (*line == ' ' || *line == '\t')
    line++;
  p = line;
  while (*p && *p != ' ' && *p != '\t' && *p != '<' && *p != '=' && *p != '>')
    p++;
  id = pool_strn2id(pool, line, p - line, 1);
  while (*p == ' ' || *p == '\t')
    p++;
  if (*p == '<' || *p == '=' || *p == '>')
    {
      int flags = 0;
      for (;; p++)
	{
	  if (*p == '<')
	    flags |= REL_LT;
	  else if (*p == '=')
	    flags |= REL_EQ;
	  else if (*p == '>')
	    flags |= REL_GT;
	  else
	    break;
	}
      while (*p == ' ' || *p == '\t')
        p++;
      line = p;
      while (*p && *p != ' ' && *p != '\t')
	p++;
      id = pool_rel2id(pool, id, pool_strn2id(pool, line, p - line, 1), flags, 1);
    }
  return repo_addid_dep(repo, olddeps, id, 0);
}

Id
repo_add_arch_pkg(Repo *repo, const char *fn, int flags)
{
  Pool *pool = repo->pool;
  Repodata *data;
  FILE *fp;
  struct tarhead th;
  char *line = 0;
  size_t line_alloc = 0, l;
  Solvable *s;
  int fd;
  struct stat stb;
  Chksum *pkgidchk = 0;

  data = repo_add_repodata(repo, flags);
  if ((fd = open(flags & REPO_USE_ROOTDIR ? pool_prepend_rootdir_tmp(pool, fn) : fn, O_RDONLY, 0)) < 0)
    {
      pool_error(pool, -1, "%s: %s", fn, strerror(errno));
      return 0;
    }
  if (fstat(fd, &stb))
    {
      pool_error(pool, -1, "%s: fstat: %s", fn, strerror(errno));
      close(fd);
      return 0;
    }
  if (!(fp = solv_xfopen_fd(fn, fd, "r")))
    {
      pool_error(pool, -1, "%s: fdopen failed", fn);
      close(fd);
      return 0;
    }
  s = 0;
  tarhead_init(&th, fp);
  while (tarhead_next(&th) > 0)
    {
      if (th.type != 1 || strcmp(th.path, ".PKGINFO") != 0)
	{
          tarhead_skip(&th);
	  continue;
	}
      s = pool_id2solvable(pool, repo_add_solvable(repo));
      if (flags & ARCH_ADD_WITH_PKGID)
	pkgidchk = solv_chksum_create(REPOKEY_TYPE_MD5);
      while ((l = tarhead_gets(&th, &line, &line_alloc)) > 0)
	{
	  if (pkgidchk)
	    solv_chksum_add(pkgidchk, line, l);
	  l = strlen(line);	/* no NULs please */
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
	    repodata_add_poolstr_array(data, s - pool->solvables, SOLVABLE_LICENSE, line + 10);
	  else if (!strncmp(line, "replaces = ", 11))
	    s->obsoletes = adddep(repo, s->obsoletes, line + 11);
	  else if (!strncmp(line, "group = ", 8))
	    repodata_add_poolstr_array(data, s - pool->solvables, SOLVABLE_GROUP, line + 8);
	  else if (!strncmp(line, "depend = ", 9))
	    s->requires = adddep(repo, s->requires, line + 9);
	  else if (!strncmp(line, "optdepend = ", 12))
	    {
	      char *p = strchr(line, ':');
	      if (p)
		*p = 0;
	      s->suggests = adddep(repo, s->suggests, line + 12);
	    }
	  else if (!strncmp(line, "conflict = ", 11))
	    s->conflicts = adddep(repo, s->conflicts, line + 11);
	  else if (!strncmp(line, "provides = ", 11))
	    s->provides = adddep(repo, s->provides, line + 11);
	}
      break;
    }
  solv_free(line);
  tarhead_free(&th);
  fclose(fp);
  if (!s)
    {
      pool_error(pool, -1, "%s: not an arch package", fn);
      if (pkgidchk)
	solv_chksum_free(pkgidchk, 0);
      return 0;
    }
  if (s && !s->name)
    {
      pool_error(pool, -1, "%s: package has no name", fn);
      s = solvable_free(s, 1);
    }
  if (s)
    {
      if (!s->arch)
	s->arch = ARCH_ANY;
      if (!s->evr)
	s->evr = ID_EMPTY;
      s->provides = repo_addid_dep(repo, s->provides, pool_rel2id(pool, s->name, s->evr, REL_EQ, 1), 0);
      if (!(flags & REPO_NO_LOCATION))
	repodata_set_location(data, s - pool->solvables, 0, 0, fn);
      if (S_ISREG(stb.st_mode))
        repodata_set_num(data, s - pool->solvables, SOLVABLE_DOWNLOADSIZE, (unsigned long long)stb.st_size);
      if (pkgidchk)
	{
	  unsigned char pkgid[16];
	  solv_chksum_free(pkgidchk, pkgid);
	  repodata_set_bin_checksum(data, s - pool->solvables, SOLVABLE_PKGID, REPOKEY_TYPE_MD5, pkgid);
	  pkgidchk = 0;
	}
    }
  if (pkgidchk)
    solv_chksum_free(pkgidchk, 0);
  if (!(flags & REPO_NO_INTERNALIZE))
    repodata_internalize(data);
  return s ? s - pool->solvables : 0;
}

static Hashtable
joinhash_init(Repo *repo, Hashval *hmp)
{
  Hashval hm = mkmask(repo->nsolvables);
  Hashtable ht = solv_calloc(hm + 1, sizeof(*ht));
  Hashval h, hh;
  Solvable *s;
  int i;

  FOR_REPO_SOLVABLES(repo, i, s)
    {
      hh = HASHCHAIN_START;
      h = s->name & hm;
      while (ht[h])
        h = HASHCHAIN_NEXT(h, hh, hm);
      ht[h] = i;
    }
  *hmp = hm;
  return ht;
}

static Solvable *
joinhash_lookup(Repo *repo, Hashtable ht, Hashval hm, const char *fn)
{
  const char *p;
  Id name, evr;
  Hashval h, hh;

  if ((p = strrchr(fn, '/')) != 0)
    fn = p + 1;
  /* here we assume that the dirname is name-evr */
  if (!*fn)
    return 0;
  for (p = fn + strlen(fn) - 1; p > fn; p--)
    {
      while (p > fn && *p != '-')
	p--;
      if (p == fn)
	return 0;
      name = pool_strn2id(repo->pool, fn, p - fn, 0);
      if (!name)
	continue;
      evr = pool_str2id(repo->pool, p + 1, 0);
      if (!evr)
	continue;
      /* found valid name/evr combination, check hash */
      hh = HASHCHAIN_START;
      h = name & hm;
      while (ht[h])
	{
	  Solvable *s = repo->pool->solvables + ht[h];
	  if (s->name == name && s->evr == evr)
	    return s;
	  h = HASHCHAIN_NEXT(h, hh, hm);
	}
    }
  return 0;
}

static int getsentrynl(struct tarhead *th, char **linep, size_t *line_allocp)
{
  size_t l = tarhead_gets(th, linep, line_allocp);
  if (l)
    l = strlen(*linep);
  if (l && (*linep)[l - 1] == '\n')
    (*linep)[--l] = 0;
  return l ? 1 : 0;
}

static void
adddata(Repodata *data, Solvable *s, struct tarhead *th)
{
  Repo *repo = data->repo;
  Pool *pool = repo->pool;
  char *line = 0;
  size_t l, line_alloc = 0;
  int havesha256 = 0;

  while ((l = tarhead_gets(th, &line, &line_alloc)) > 0)
    {
      l = strlen(line);
      if (l && line[l - 1] == '\n')
        line[--l] = 0;
      if (l <= 2 || line[0] != '%' || line[l - 1] != '%')
	continue;
      if (!strcmp(line, "%FILENAME%"))
	{
	  if (getsentrynl(th, &line, &line_alloc))
	    repodata_set_location(data, s - pool->solvables, 0, 0, line);
	}
      else if (!strcmp(line, "%NAME%"))
	{
	  if (getsentrynl(th, &line, &line_alloc))
	    s->name = pool_str2id(pool, line, 1);
	}
      else if (!strcmp(line, "%VERSION%"))
	{
	  if (getsentrynl(th, &line, &line_alloc))
	    s->evr = pool_str2id(pool, line, 1);
	}
      else if (!strcmp(line, "%DESC%"))
	{
	  if (getsentrynl(th, &line, &line_alloc))
	    {
	      repodata_set_str(data, s - pool->solvables, SOLVABLE_SUMMARY, line);
	      repodata_set_str(data, s - pool->solvables, SOLVABLE_DESCRIPTION, line);
	    }
	}
      else if (!strcmp(line, "%GROUPS%"))
	{
	  if (getsentrynl(th, &line, &line_alloc))
	    repodata_add_poolstr_array(data, s - pool->solvables, SOLVABLE_GROUP, line);
	}
      else if (!strcmp(line, "%CSIZE%"))
	{
	  if (getsentrynl(th, &line, &line_alloc))
	    repodata_set_num(data, s - pool->solvables, SOLVABLE_DOWNLOADSIZE, strtoull(line, 0, 10));
	}
      else if (!strcmp(line, "%ISIZE%"))
	{
	  if (getsentrynl(th, &line, &line_alloc))
	    repodata_set_num(data, s - pool->solvables, SOLVABLE_INSTALLSIZE, strtoull(line, 0, 10));
	}
      else if (!strcmp(line, "%MD5SUM%"))
	{
	  if (getsentrynl(th, &line, &line_alloc) && !havesha256)
	    repodata_set_checksum(data, s - pool->solvables, SOLVABLE_CHECKSUM, REPOKEY_TYPE_MD5, line);
	}
      else if (!strcmp(line, "%SHA256SUM%"))
	{
	  if (getsentrynl(th, &line, &line_alloc))
	    {
	      repodata_set_checksum(data, s - pool->solvables, SOLVABLE_CHECKSUM, REPOKEY_TYPE_SHA256, line);
	      havesha256 = 1;
	    }
	}
      else if (!strcmp(line, "%URL%"))
	{
	  if (getsentrynl(th, &line, &line_alloc))
	    repodata_set_str(data, s - pool->solvables, SOLVABLE_URL, line);
	}
      else if (!strcmp(line, "%LICENSE%"))
	{
	  if (getsentrynl(th, &line, &line_alloc))
	    repodata_add_poolstr_array(data, s - pool->solvables, SOLVABLE_LICENSE, line);
	}
      else if (!strcmp(line, "%ARCH%"))
	{
	  if (getsentrynl(th, &line, &line_alloc))
	    s->arch = pool_str2id(pool, line, 1);
	}
      else if (!strcmp(line, "%BUILDDATE%"))
	{
	  if (getsentrynl(th, &line, &line_alloc))
	    repodata_set_num(data, s - pool->solvables, SOLVABLE_BUILDTIME, strtoull(line, 0, 10));
	}
      else if (!strcmp(line, "%PACKAGER%"))
	{
	  if (getsentrynl(th, &line, &line_alloc))
	    repodata_set_poolstr(data, s - pool->solvables, SOLVABLE_PACKAGER, line);
	}
      else if (!strcmp(line, "%REPLACES%"))
	{
	  while (getsentrynl(th, &line, &line_alloc) && *line)
	    s->obsoletes = adddep(repo, s->obsoletes, line);
	}
      else if (!strcmp(line, "%DEPENDS%"))
	{
	  while (getsentrynl(th, &line, &line_alloc) && *line)
	    s->requires = adddep(repo, s->requires, line);
	}
      else if (!strcmp(line, "%CONFLICTS%"))
	{
	  while (getsentrynl(th, &line, &line_alloc) && *line)
	    s->conflicts = adddep(repo, s->conflicts, line);
	}
      else if (!strcmp(line, "%PROVIDES%"))
	{
	  while (getsentrynl(th, &line, &line_alloc) && *line)
	    s->provides = adddep(repo, s->provides, line);
	}
      else if (!strcmp(line, "%OPTDEPENDS%"))
	{
	  while (getsentrynl(th, &line, &line_alloc) && *line)
	    {
	      char *p = strchr(line, ':');
	      if (p && p > line)
		*p = 0;
	      s->suggests = adddep(repo, s->suggests, line);
	    }
	}
      else if (!strcmp(line, "%FILES%"))
	{
	  while (getsentrynl(th, &line, &line_alloc) && *line)
	    {
	      char *p;
	      Id id;
	      l = strlen(line);
	      if (l > 1 && line[l - 1] == '/')
		line[--l] = 0;	/* remove trailing slashes */
	      if ((p = strrchr(line , '/')) != 0)
		{
		  *p++ = 0;
		  if (line[0] != '/')	/* anchor */
		    {
		      char tmp = *p;
		      memmove(line + 1, line, p - 1 - line);
		      *line = '/';
		      *p = 0;
		      id = repodata_str2dir(data, line, 1);
		      *p = tmp;
		    }
		  else
		    id = repodata_str2dir(data, line, 1);
		}
	      else
		{
		  p = line;
		  id = 0;
		}
	      if (!id)
		id = repodata_str2dir(data, "/", 1);
	      repodata_add_dirstr(data, s - pool->solvables, SOLVABLE_FILELIST, id, p);
	    }
	}
      while (*line && getsentrynl(th, &line, &line_alloc))
	;
    }
  solv_free(line);
}

static void
finishsolvable(Repo *repo, Solvable *s)
{
  Pool *pool = repo->pool;
  if (!s)
    return;
  if (!s->name)
    {
      solvable_free(s, 1);
      return;
    }
  if (!s->arch)
    s->arch = ARCH_ANY;
  if (!s->evr)
    s->evr = ID_EMPTY;
  s->provides = repo_addid_dep(repo, s->provides, pool_rel2id(pool, s->name, s->evr, REL_EQ, 1), 0);
}

int
repo_add_arch_repo(Repo *repo, FILE *fp, int flags)
{
  Pool *pool = repo->pool;
  Repodata *data;
  struct tarhead th;
  char *lastdn = 0;
  int lastdnlen = 0;
  Solvable *s = 0;
  Hashtable joinhash = 0;
  Hashval joinhashmask = 0;

  data = repo_add_repodata(repo, flags);

  if (flags & REPO_EXTEND_SOLVABLES)
    joinhash = joinhash_init(repo, &joinhashmask);

  tarhead_init(&th, fp);
  while (tarhead_next(&th) > 0)
    {
      char *bn;
      if (th.type != 1)
	{
          tarhead_skip(&th);
	  continue;
	}
      bn = strrchr(th.path, '/');
      if (!bn || (strcmp(bn + 1, "desc") != 0 && strcmp(bn + 1, "depends") != 0 && strcmp(bn + 1, "files") != 0))
	{
          tarhead_skip(&th);
	  continue;
	}
      if ((flags & REPO_EXTEND_SOLVABLES) != 0 && (!strcmp(bn + 1, "desc") || !strcmp(bn + 1, "depends")))
	{
          tarhead_skip(&th);
	  continue;	/* skip those when we're extending */
	}
      if (!lastdn || (bn - th.path) != lastdnlen || strncmp(lastdn, th.path, lastdnlen) != 0)
	{
	  finishsolvable(repo, s);
	  solv_free(lastdn);
	  lastdn = solv_strdup(th.path);
	  lastdnlen = bn - th.path;
	  lastdn[lastdnlen] = 0;
	  if (flags & REPO_EXTEND_SOLVABLES)
	    {
	      s = joinhash_lookup(repo, joinhash, joinhashmask, lastdn);
	      if (!s)
		{
		  tarhead_skip(&th);
		  continue;
		}
	    }
	  else
	    s = pool_id2solvable(pool, repo_add_solvable(repo));
	}
      adddata(data, s, &th);
    }
  finishsolvable(repo, s);
  solv_free(joinhash);
  solv_free(lastdn);
  if (!(flags & REPO_NO_INTERNALIZE))
    repodata_internalize(data);
  return 0;
}

int
repo_add_arch_local(Repo *repo, const char *dir, int flags)
{
  Pool *pool = repo->pool;
  Repodata *data;
  DIR *dp;
  struct dirent *de;
  char *entrydir, *file;
  FILE *fp;
  Solvable *s;

  data = repo_add_repodata(repo, flags);

  if (flags & REPO_USE_ROOTDIR)
    dir = pool_prepend_rootdir(pool, dir);
  dp = opendir(dir);
  if (dp)
    {
      while ((de = readdir(dp)) != 0)
	{
	  if (!de->d_name[0] || de->d_name[0] == '.')
	    continue;
	  entrydir = solv_dupjoin(dir, "/", de->d_name);
	  file = pool_tmpjoin(repo->pool, entrydir, "/desc", 0);
	  s = 0;
	  if ((fp = fopen(file, "r")) != 0)
	    {
	      struct tarhead th;
	      tarhead_init(&th, fp);
	      s = pool_id2solvable(pool, repo_add_solvable(repo));
	      adddata(data, s, &th);
	      tarhead_free(&th);
	      fclose(fp);
	      file = pool_tmpjoin(repo->pool, entrydir, "/files", 0);
	      if ((fp = fopen(file, "r")) != 0)
		{
		  tarhead_init(&th, fp);
		  adddata(data, s, &th);
		  tarhead_free(&th);
		  fclose(fp);
		}
	    }
	  solv_free(entrydir);
	}
      closedir(dp);
    }
  if (!(flags & REPO_NO_INTERNALIZE))
    repodata_internalize(data);
  if (flags & REPO_USE_ROOTDIR)
    solv_free((char *)dir);
  return 0;
}

