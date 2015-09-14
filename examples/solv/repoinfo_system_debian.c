#if defined(ENABLE_DEBIAN) && defined(DEBIAN)

#include <stdio.h>
#include <stdlib.h>
#include <unistd.h>
#include <fcntl.h>
#include <sys/types.h>
#include <sys/wait.h>
#include <sys/stat.h>

#include "pool.h"
#include "repo.h"
#include "repo_deb.h"
#include "transaction.h"

#include "repoinfo.h"
#include "repoinfo_cache.h"
#include "repoinfo_system_debian.h"

static void
rundpkg(const char *arg, const char *name, int dupfd3, const char *rootdir)
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
      if (strcmp(arg, "--install") == 0)
	execlp("dpkg", "dpkg", "--install", "--root", rootdir, "--force", "all", name, (char *)0);
      else
	execlp("dpkg", "dpkg", "--remove", "--root", rootdir, "--force", "all", name, (char *)0);
      perror("dpkg");
      _exit(0);
    }
  while (waitpid(pid, &status, 0) != pid)
    ;
  if (status)
    {
      printf("dpkg failed\n");
      exit(1);
    }
}

int
read_installed_debian(struct repoinfo *cinfo)
{
  struct stat stb;
  Repo *repo = cinfo->repo;
  Pool *pool = repo->pool;

  memset(&stb, 0, sizeof(stb));
  printf("dpgk database:");
  if (stat(pool_prepend_rootdir_tmp(pool, "/var/lib/dpkg/status"), &stb))
    memset(&stb, 0, sizeof(stb));
  calc_cookie_stat(&stb, REPOKEY_TYPE_SHA256, 0, cinfo->cookie);
  cinfo->cookieset = 1;
  if (usecachedrepo(cinfo, 0, 0))
    {
      printf(" cached\n");
      return 1;
    }
  if (repo_add_debdb(repo, REPO_REUSE_REPODATA | REPO_NO_INTERNALIZE | REPO_USE_ROOTDIR))
    {
      fprintf(stderr, "installed db: %s\n", pool_errstr(pool));
      return 0;
    }
  repo_internalize(repo);
  writecachedrepo(cinfo, 0, 0);
  return 1;
}

void
commit_transactionelement_debian(Pool *pool, Id type, Id p, FILE *fp)
{
  Solvable *s = pool_id2solvable(pool, p);
  const char *rootdir = pool_get_rootdir(pool);

  switch(type)
    {   
    case SOLVER_TRANSACTION_ERASE:
      rundpkg("--remove", pool_id2str(pool, s->name), 0, rootdir);
      break;
    case SOLVER_TRANSACTION_INSTALL:
    case SOLVER_TRANSACTION_MULTIINSTALL:
      rewind(fp);
      lseek(fileno(fp), 0, SEEK_SET);
      rundpkg("--install", "/dev/fd/3", fileno(fp), rootdir);
      break;
    default:
      break;
    }   
}

#endif
