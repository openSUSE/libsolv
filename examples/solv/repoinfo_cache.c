#include <stdio.h>
#include <stdlib.h>
#include <unistd.h>
#include <sys/stat.h>
#include <time.h>

#include "pool.h"
#include "repo.h"
#include "chksum.h"
#include "repo_solv.h"
#include "repo_write.h"

#include "repoinfo.h"
#include "repoinfo_cache.h"

#define COOKIE_IDENT "1.1"

#define SOLVCACHE_PATH "/var/cache/solv"

static char *userhome;

void
set_userhome()
{
  userhome = getenv("HOME");
  if (userhome && userhome[0] != '/') 
    userhome = 0; 
}

void
calc_cookie_fp(FILE *fp, Id chktype, unsigned char *out)
{
  char buf[4096];
  Chksum *h = solv_chksum_create(chktype);
  int l;

  solv_chksum_add(h, COOKIE_IDENT, strlen(COOKIE_IDENT));
  while ((l = fread(buf, 1, sizeof(buf), fp)) > 0)
    solv_chksum_add(h, buf, l);
  rewind(fp);
  solv_chksum_free(h, out);
}

void
calc_cookie_stat(struct stat *stb, Id chktype, unsigned char *cookie, unsigned char *out)
{
  Chksum *h = solv_chksum_create(chktype);
  solv_chksum_add(h, COOKIE_IDENT, strlen(COOKIE_IDENT));
  if (cookie)
    solv_chksum_add(h, cookie, 32);
  solv_chksum_add(h, &stb->st_dev, sizeof(stb->st_dev));
  solv_chksum_add(h, &stb->st_ino, sizeof(stb->st_ino));
  solv_chksum_add(h, &stb->st_size, sizeof(stb->st_size));
  solv_chksum_add(h, &stb->st_mtime, sizeof(stb->st_mtime));
  solv_chksum_free(h, out);
}

char *
calc_cachepath(Repo *repo, const char *repoext, int forcesystemloc)
{
  char *q, *p;
  int l;
  if (!forcesystemloc && userhome && getuid())
    p = pool_tmpjoin(repo->pool, userhome, "/.solvcache/", 0);
  else
    p = pool_tmpjoin(repo->pool, SOLVCACHE_PATH, "/", 0);
  l = strlen(p);
  p = pool_tmpappend(repo->pool, p, repo->name, 0);
  if (repoext)
    {
      p = pool_tmpappend(repo->pool, p, "_", repoext);
      p = pool_tmpappend(repo->pool, p, ".solvx", 0);
    }
  else
    p = pool_tmpappend(repo->pool, p, ".solv", 0);
  q = p + l;
  if (*q == '.')
    *q = '_';
  for (; *q; q++)
    if (*q == '/')
      *q = '_';
  return p;
}

int
usecachedrepo(struct repoinfo *cinfo, const char *repoext, int mark)
{
  Repo *repo = cinfo->repo;
  FILE *fp;
  unsigned char *cookie = repoext ? cinfo->extcookie : (cinfo->cookieset ? cinfo->cookie : 0);
  unsigned char mycookie[32];
  unsigned char myextcookie[32];
  int flags;
  int forcesystemloc;

  if (repoext && !cinfo->extcookieset)
    return 0;	/* huh? */
  forcesystemloc = mark & 2 ? 0 : 1;
  if (mark < 2 && userhome && getuid())
    {
      /* first try home location */
      int res = usecachedrepo(cinfo, repoext, mark | 2);
      if (res)
	return res;
    }
  mark &= 1;
  if (!(fp = fopen(calc_cachepath(repo, repoext, forcesystemloc), "r")))
    return 0;
  if (!repoext && !cinfo->cookieset && cinfo->autorefresh && cinfo->metadata_expire != -1)
    {
      struct stat stb;		/* no cookie set yet, check cache expiry time */
      if (fstat(fileno(fp), &stb) || time(0) - stb.st_mtime >= cinfo->metadata_expire)
	{
	  fclose(fp);
	  return 0;
	}
    }
  if (fseek(fp, -sizeof(mycookie), SEEK_END) || fread(mycookie, sizeof(mycookie), 1, fp) != 1)
    {
      fclose(fp);
      return 0;
    }
  if (cookie && memcmp(cookie, mycookie, sizeof(mycookie)) != 0)
    {
      fclose(fp);
      return 0;
    }
  if (cinfo->type != TYPE_INSTALLED && !repoext)
    {
      if (fseek(fp, -sizeof(mycookie) * 2, SEEK_END) || fread(myextcookie, sizeof(myextcookie), 1, fp) != 1)
	{
	  fclose(fp);
	  return 0;
	}
    }
  rewind(fp);

  flags = 0;
  if (repoext)
    {
      flags = REPO_USE_LOADING|REPO_EXTEND_SOLVABLES;
      if (strcmp(repoext, "DL") != 0)
        flags |= REPO_LOCALPOOL;	/* no local pool for DL so that we can compare IDs */
    }
  if (repo_add_solv(repo, fp, flags))
    {
      fclose(fp);
      return 0;
    }
  if (cinfo->type != TYPE_INSTALLED && !repoext)
    {
      memcpy(cinfo->cookie, mycookie, sizeof(mycookie));
      cinfo->cookieset = 1;
      memcpy(cinfo->extcookie, myextcookie, sizeof(myextcookie));
      cinfo->extcookieset = 1;
    }
  if (mark)
    futimens(fileno(fp), 0);	/* try to set modification time */
  fclose(fp);
  return 1;
}

static void
switchtowritten(struct repoinfo *cinfo, const char *repoext, Repodata *repodata, char *tmpl)
{
  Repo *repo = cinfo->repo;
  FILE *fp;
  int i;

  if (!repoext && repodata)
    return;	/* rewrite case, don't bother for the added fileprovides */
  for (i = repo->start; i < repo->end; i++)
   if (repo->pool->solvables[i].repo != repo)
     break;
  if (i < repo->end)
    return;	/* not a simple block */
      /* switch to just saved repo to activate paging and save memory */
  fp = fopen(tmpl, "r");
  if (!fp)
    return;
  if (!repoext)
    {
      /* main repo */
      repo_empty(repo, 1);
      if (repo_add_solv(repo, fp, SOLV_ADD_NO_STUBS))
	{
	  /* oops, no way to recover from here */
	  fprintf(stderr, "internal error\n");
	  exit(1);
	}
    }
  else
    {
      int flags = REPO_USE_LOADING|REPO_EXTEND_SOLVABLES;
      /* make sure repodata contains complete repo */
      /* (this is how repodata_write saves it) */
      repodata_extend_block(repodata, repo->start, repo->end - repo->start);
      repodata->state = REPODATA_LOADING;
      if (strcmp(repoext, "DL") != 0)
	flags |= REPO_LOCALPOOL;
      repo_add_solv(repo, fp, flags);
      repodata->state = REPODATA_AVAILABLE;	/* in case the load failed */
    }
  fclose(fp);
}

void
writecachedrepo(struct repoinfo *cinfo, const char *repoext, Repodata *repodata)
{
  Repo *repo = cinfo->repo;
  FILE *fp;
  int fd;
  char *tmpl, *cachedir;

  if (cinfo->incomplete || (repoext && !cinfo->extcookieset) || (!repoext && !cinfo->cookieset))
    return;
  cachedir = userhome && getuid() ? pool_tmpjoin(repo->pool, userhome, "/.solvcache", 0) : SOLVCACHE_PATH;
  if (access(cachedir, W_OK | X_OK) != 0 && mkdir(cachedir, 0755) == 0)
    printf("[created %s]\n", cachedir);
  /* use dupjoin instead of tmpjoin because tmpl must survive repo_write */
  tmpl = solv_dupjoin(cachedir, "/", ".newsolv-XXXXXX");
  fd = mkstemp(tmpl);
  if (fd < 0)
    {
      free(tmpl);
      return;
    }
  fchmod(fd, 0444);
  if (!(fp = fdopen(fd, "w")))
    {
      close(fd);
      unlink(tmpl);
      free(tmpl);
      return;
    }

  if (!repodata)
    repo_write(repo, fp);
  else if (repoext)
    repodata_write(repodata, fp);
  else
    {
      int oldnrepodata = repo->nrepodata;
      repo->nrepodata = oldnrepodata > 2 ? 2 : oldnrepodata;	/* XXX: do this right */
      repo_write(repo, fp);
      repo->nrepodata = oldnrepodata;
    }

  if (!repoext && cinfo->type != TYPE_INSTALLED)
    {
      if (!cinfo->extcookieset)
	{
	  /* create the ext cookie and append it */
	  /* we just need some unique ID */
	  struct stat stb;
	  if (fstat(fileno(fp), &stb))
	    memset(&stb, 0, sizeof(stb));
	  calc_cookie_stat(&stb, REPOKEY_TYPE_SHA256, cinfo->cookie, cinfo->extcookie);
	  cinfo->extcookieset = 1;
	}
      if (fwrite(cinfo->extcookie, 32, 1, fp) != 1)
	{
	  fclose(fp);
	  unlink(tmpl);
	  free(tmpl);
	  return;
	}
    }
  /* append our cookie describing the metadata state */
  if (fwrite(repoext ? cinfo->extcookie : cinfo->cookie, 32, 1, fp) != 1)
    {
      fclose(fp);
      unlink(tmpl);
      free(tmpl);
      return;
    }
  if (fclose(fp))
    {
      unlink(tmpl);
      free(tmpl);
      return;
    }

  switchtowritten(cinfo, repoext, repodata, tmpl);

  if (!rename(tmpl, calc_cachepath(repo, repoext, 0)))
    unlink(tmpl);
  free(tmpl);
}

