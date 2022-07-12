#if defined(ENABLE_RPMDB) || defined(ENABLE_RPMPKG)

#include <stdio.h>
#include <stdlib.h>
#include <unistd.h>
#include <dirent.h>
#include <sys/stat.h>

#include "pool.h"
#include "repo.h"

#ifdef SUSE
#include "repo_autopattern.h"
#endif
#include "repoinfo.h"
#include "repoinfo_cache.h"
#include "repoinfo_download.h"
#include "repoinfo_type_rpmmd.h"
#include "ext/repo_rpmdb.h"

static inline int endswith(const char* str, const char* suf)
{
  if (strlen(str) < strlen(suf))
    return 0;
  return strcmp(str + strlen(str) - strlen(suf), suf) == 0;
}

int
plaindir_load(struct repoinfo *cinfo, Pool **sigpoolp)
{
  Repo *repo = cinfo->repo;
  Repodata *data;
  DIR *dp;
  struct dirent *de;
  struct stat stb;

  printf("plaindir repo '%s':", cinfo->alias);
  fflush(stdout);
  if (stat(cinfo->path, &stb))
    {
      perror(cinfo->path);
      return -1;
    }
  calc_cookie_stat(&stb, REPOKEY_TYPE_SHA256, NULL, cinfo->cookie);
  cinfo->cookieset = 1;
  if (usecachedrepo(cinfo, 0, 1))
    {
      printf(" cached\n");
      return 1;
    }
  printf(" reading\n");
  if ((dp = opendir(cinfo->path)) == 0)
    {
      perror(cinfo->path);
      return -1;
    }
  while ((de = readdir(dp)) != 0)
    {
      if (de->d_name[0] == 0 || de->d_name[0] == '.')
        continue;
      if (!endswith(de->d_name, ".rpm") || endswith(de->d_name, ".delta.rpm") || endswith(de->d_name, ".patch.rpm"))
        continue;
      char* fn = solv_dupjoin(cinfo->path, "/", de->d_name);
      repo_add_rpm(repo, fn, 0);
      solv_free(fn);
    }
  closedir(dp);

#ifdef SUSE
  repo_add_autopattern(repo, 0);
#endif
  data = repo_add_repodata(repo, 0);
  repodata_internalize(data);
  writecachedrepo(cinfo, 0, 0);
  repodata_create_stubs(repo_last_repodata(repo));
  return 1;
}

#endif
