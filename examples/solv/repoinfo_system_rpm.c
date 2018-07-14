#if defined(ENABLE_RPMDB) && (defined(SUSE) || defined(FEDORA) || defined(MANDRIVA) || defined(MAGEIA))

#include <stdio.h>
#include <stdlib.h>
#include <unistd.h>
#include <fcntl.h>
#include <sys/types.h>
#include <sys/wait.h>
#include <sys/stat.h>

#include "pool.h"
#include "repo.h"
#include "repo_rpmdb.h"
#if defined(ENABLE_SUSEREPO) && defined(SUSE)
#include "repo_products.h"
#endif
#if defined(ENABLE_APPDATA)
#include "repo_appdata.h"
#endif
#include "transaction.h"

#include "repoinfo.h"
#include "repoinfo_cache.h"
#include "repoinfo_system_rpm.h"

#ifdef SUSE
# define PRODUCTS_PATH "/etc/products.d"
#endif
#ifdef ENABLE_APPDATA
# define APPDATA_PATH "/usr/share/metainfo"
# define APPDATA_LEGACY_PATH "/usr/share/appdata"
#endif

static void
runrpm(const char *arg, const char *name, int dupfd3, const char *rootdir)
{
  pid_t pid;
  int status;

  if ((pid = fork()) == (pid_t)-1)
    {
      perror("fork");
      exit(1);
    }
  if (pid == 0)
    {
      if (!rootdir)
	rootdir = "/";
      if (dupfd3 != -1 && dupfd3 != 3)
	{
	  dup2(dupfd3, 3);
	  close(dupfd3);
	}
      if (dupfd3 != -1)
	fcntl(3, F_SETFD, 0);   /* clear CLOEXEC */
      if (strcmp(arg, "-e") == 0)
	execlp("rpm", "rpm", arg, "--nodeps", "--nodigest", "--nosignature", "--root", rootdir, name, (char *)0);
      else
	execlp("rpm", "rpm", arg, "--force", "--nodeps", "--nodigest", "--nosignature", "--root", rootdir, name, (char *)0);
      perror("rpm");
      _exit(0);
    }
  while (waitpid(pid, &status, 0) != pid)
    ;
  if (status)
    {
      printf("rpm failed\n");
      exit(1);
    }
}

int
read_installed_rpm(struct repoinfo *cinfo)
{
  Repo *repo = cinfo->repo;
  Pool *pool = repo->pool;
  FILE *ofp = 0;
  struct stat stb;

  memset(&stb, 0, sizeof(stb));
  printf("rpm database:");
  if (stat(pool_prepend_rootdir_tmp(pool, "/var/lib/rpm/Packages"), &stb))
    memset(&stb, 0, sizeof(stb));
  calc_cookie_stat(&stb, REPOKEY_TYPE_SHA256, 0, cinfo->cookie);
  cinfo->cookieset = 1;
  if (usecachedrepo(cinfo, 0, 0))
    {
      printf(" cached\n");
      return 1;
    }
  printf(" reading\n");
#if defined(ENABLE_SUSEREPO) && defined(PRODUCTS_PATH)
  if (repo_add_products(repo, PRODUCTS_PATH, REPO_REUSE_REPODATA | REPO_NO_INTERNALIZE | REPO_USE_ROOTDIR))
    {
      fprintf(stderr, "product reading failed: %s\n", pool_errstr(pool));
      return 0;
    }
#endif
#if defined(ENABLE_APPDATA) && defined(APPDATA_PATH)
  if (repo_add_appdata_dir(repo, APPDATA_PATH, REPO_REUSE_REPODATA | REPO_NO_INTERNALIZE | REPO_USE_ROOTDIR))
    {
      fprintf(stderr, "appdata reading failed: %s\n", pool_errstr(pool));
      return 0;
    }
#elif defined(ENABLE_APPDATA) && defined(APPDATA_LEGACY_PATH)
  if (repo_add_appdata_dir(repo, APPDATA_LEGACY_PATH, REPO_REUSE_REPODATA | REPO_NO_INTERNALIZE | REPO_USE_ROOTDIR))
    {
      fprintf(stderr, "appdata reading from legacy dir failed: %s\n", pool_errstr(pool));
      return 0;
    }
#endif
  ofp = fopen(calc_cachepath(repo, 0, 0), "r");
  if (repo_add_rpmdb_reffp(repo, ofp, REPO_REUSE_REPODATA | REPO_NO_INTERNALIZE | REPO_USE_ROOTDIR))
    {
      fprintf(stderr, "installed db: %s\n", pool_errstr(pool));
      return 0;
    }
  if (ofp)
    fclose(ofp);
  repo_internalize(repo);
  writecachedrepo(cinfo, 0, 0);
  return 1;
}

void
commit_transactionelement_rpm(Pool *pool, Id type, Id p, FILE *fp)
{
  Solvable *s = pool_id2solvable(pool, p);
  const char *rootdir = pool_get_rootdir(pool);
  const char *evr, *evrp, *nvra;

  switch(type)
    {
    case SOLVER_TRANSACTION_ERASE:
      if (!s->repo->rpmdbid || !s->repo->rpmdbid[p - s->repo->start])
	break;
      /* strip epoch from evr */
      evr = evrp = pool_id2str(pool, s->evr);
      while (*evrp >= '0' && *evrp <= '9')
	evrp++;
      if (evrp > evr && evrp[0] == ':' && evrp[1])
	evr = evrp + 1;
      nvra = pool_tmpjoin(pool, pool_id2str(pool, s->name), "-", evr);
      nvra = pool_tmpappend(pool, nvra, ".", pool_id2str(pool, s->arch));
      runrpm("-e", nvra, -1, rootdir);      /* too bad that --querybynumber doesn't work */
      break;
    case SOLVER_TRANSACTION_INSTALL:
    case SOLVER_TRANSACTION_MULTIINSTALL:
      rewind(fp);
      lseek(fileno(fp), 0, SEEK_SET);
      runrpm(type == SOLVER_TRANSACTION_MULTIINSTALL ? "-i" : "-U", "/dev/fd/3", fileno(fp), rootdir);
      break;
    default:
      break;
    }
}

#endif
