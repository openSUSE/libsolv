#include <stdio.h>
#include <stdlib.h>
#include <unistd.h>

#include "pool.h"
#include "repo.h"
#if defined(ENABLE_RPMDB) && (defined(SUSE) || defined(FEDORA) || defined(MANDRIVA) || defined(MAGEIA))
#include "repo_rpmdb.h"
#endif
#if defined(ENABLE_DEBIAN) && defined(DEBIAN)
#include "repo_deb.h"
#endif
#ifdef SUSE
#include "repo_autopattern.h"
#endif


#include "repoinfo.h"
#include "repoinfo_cache.h"

#if defined(SUSE) || defined(FEDORA) || defined(MAGEIA)
#include "repoinfo_config_yum.h"
#endif
#if defined(DEBIAN)
#include "repoinfo_config_debian.h"
#endif
#if defined(MANDRIVA)
#include "repoinfo_config_urpmi.h"
#endif

#if defined(ENABLE_RPMDB) && (defined(SUSE) || defined(FEDORA) || defined(MANDRIVA) || defined(MAGEIA))
#include "repoinfo_system_rpm.h"
#endif
#if defined(ENABLE_DEBIAN) && defined(DEBIAN)
#include "repoinfo_system_debian.h"
#endif

#ifdef ENABLE_RPMMD
#include "repoinfo_type_rpmmd.h"
#endif
#ifdef ENABLE_SUSEREPO
#include "repoinfo_type_susetags.h"
#endif
#ifdef ENABLE_DEBIAN
#include "repoinfo_type_debian.h"
#endif
#ifdef ENABLE_MDKREPO
#include "repoinfo_type_mdk.h"
#endif

static int
repoinfos_sort_cmp(const void *ap, const void *bp)
{
  const struct repoinfo *a = ap;
  const struct repoinfo *b = bp;
  return strcmp(a->alias, b->alias);
}

void
sort_repoinfos(struct repoinfo *repoinfos, int nrepoinfos)
{
  qsort(repoinfos, nrepoinfos, sizeof(*repoinfos), repoinfos_sort_cmp);
}

void
free_repoinfos(struct repoinfo *repoinfos, int nrepoinfos)
{
  int i, j;
  for (i = 0; i < nrepoinfos; i++)
    {
      struct repoinfo *cinfo = repoinfos + i;
      solv_free(cinfo->name);
      solv_free(cinfo->alias);
      solv_free(cinfo->path);
      solv_free(cinfo->metalink);
      solv_free(cinfo->mirrorlist);
      solv_free(cinfo->baseurl);
      for (j = 0; j < cinfo->ncomponents; j++)
        solv_free(cinfo->components[j]);
      solv_free(cinfo->components);
    }
  solv_free(repoinfos);
#if defined(SUSE) || defined(FEDORA) || defined(MAGEIA)
  yum_substitute((Pool *)0, 0);		/* free data */
#endif
}

struct repoinfo *
read_repoinfos(Pool *pool, int *nrepoinfosp)
{
  struct repoinfo *repoinfos = 0;
#if defined(SUSE) || defined(FEDORA) || defined(MAGEIA)
  repoinfos = read_repoinfos_yum(pool, nrepoinfosp);
#endif
#if defined(MANDRIVA)
  repoinfos = read_repoinfos_urpmi(pool, nrepoinfosp);
#endif
#if defined(DEBIAN)
  repoinfos = read_repoinfos_debian(pool, nrepoinfosp);
#endif
  return repoinfos;
}

int
read_installed_repo(struct repoinfo *cinfo, Pool *pool)
{
  int r = 1;
  cinfo->type = TYPE_INSTALLED;
  cinfo->repo = repo_create(pool, "@System");
  cinfo->repo->appdata = cinfo;
#if defined(ENABLE_RPMDB) && (defined(SUSE) || defined(FEDORA) || defined(MANDRIVA) || defined(MAGEIA))
  r = read_installed_rpm(cinfo);
#endif
#if defined(ENABLE_DEBIAN) && defined(DEBIAN)
  r = read_installed_debian(cinfo);
#endif
#ifdef SUSE
  repo_add_autopattern(cinfo->repo, 0);
#endif
  pool_set_installed(pool, cinfo->repo);
  return r;
}

int
is_cmdline_package(const char *filename)
{
  int l = strlen(filename);
#if defined(ENABLE_RPMDB) && (defined(SUSE) || defined(FEDORA) || defined(MANDRIVA) || defined(MAGEIA))
  if (l > 4 && !strcmp(filename + l - 4, ".rpm"))
    return 1;
#endif
#if defined(ENABLE_DEBIAN) && defined(DEBIAN)
  if (l > 4 && !strcmp(filename + l - 4, ".deb"))
    return 1;
#endif
  return 0;
}

Id
add_cmdline_package(Repo *repo, const char *filename)
{
#if defined(ENABLE_RPMDB) && (defined(SUSE) || defined(FEDORA) || defined(MANDRIVA) || defined(MAGEIA))
  return repo_add_rpm(repo, filename, REPO_REUSE_REPODATA|REPO_NO_INTERNALIZE);
#endif
#if defined(ENABLE_DEBIAN) && defined(DEBIAN)
  return repo_add_deb(repo, filename, REPO_REUSE_REPODATA|REPO_NO_INTERNALIZE);
#endif
  return 0;
}

void
commit_transactionelement(Pool *pool, Id type, Id p, FILE *fp)
{
#if defined(ENABLE_RPMDB) && (defined(SUSE) || defined(FEDORA) || defined(MANDRIVA) || defined(MAGEIA))
  commit_transactionelement_rpm(pool, type, p, fp);
#endif
#if defined(ENABLE_DEBIAN) && defined(DEBIAN)
  commit_transactionelement_debian(pool, type, p, fp);
#endif
}

void
add_ext_keys(Repodata *data, Id handle, const char *ext)
{
  static Id langtags[] = {
    SOLVABLE_SUMMARY,     REPOKEY_TYPE_STR,
    SOLVABLE_DESCRIPTION, REPOKEY_TYPE_STR,
    SOLVABLE_EULA,        REPOKEY_TYPE_STR,
    SOLVABLE_MESSAGEINS,  REPOKEY_TYPE_STR,
    SOLVABLE_MESSAGEDEL,  REPOKEY_TYPE_STR,
    SOLVABLE_CATEGORY,    REPOKEY_TYPE_ID,
    0, 0
  };
  if (!strcmp(ext, "DL"))
    {
      repodata_add_idarray(data, handle, REPOSITORY_KEYS, REPOSITORY_DELTAINFO);
      repodata_add_idarray(data, handle, REPOSITORY_KEYS, REPOKEY_TYPE_FLEXARRAY);
    }
  else if (!strcmp(ext, "FL"))
    {
      repodata_add_idarray(data, handle, REPOSITORY_KEYS, SOLVABLE_FILELIST);
      repodata_add_idarray(data, handle, REPOSITORY_KEYS, REPOKEY_TYPE_DIRSTRARRAY);
    }
  else if (!strcmp(ext, "DU"))
    {
      repodata_add_idarray(data, handle, REPOSITORY_KEYS, SOLVABLE_DISKUSAGE);
      repodata_add_idarray(data, handle, REPOSITORY_KEYS, REPOKEY_TYPE_DIRNUMNUMARRAY);
    }
  else
    {
      Pool *pool = data->repo->pool;
      int i;
      for (i = 0; langtags[i]; i += 2)
	{
	  repodata_add_idarray(data, handle, REPOSITORY_KEYS, pool_id2langid(pool, langtags[i], ext, 1));
	  repodata_add_idarray(data, handle, REPOSITORY_KEYS, langtags[i + 1]);
	}
    }
}

int
load_stub(Pool *pool, Repodata *data, void *dp)
{
  struct repoinfo *cinfo = data->repo->appdata;
  switch (cinfo->type)
    {
#ifdef ENABLE_SUSEREPO
    case TYPE_SUSETAGS:
      return susetags_load_ext(data->repo, data);
#endif
#ifdef ENABLE_RPMMD
    case TYPE_RPMMD:
      return repomd_load_ext(data->repo, data);
#endif
#ifdef ENABLE_MDKREPO
    case TYPE_MDK:
      return mdk_load_ext(data->repo, data);
#endif
    default:
      /* debian does not have any ext data yet */
      return 0;
    }
}

void
read_repos(Pool *pool, struct repoinfo *repoinfos, int nrepoinfos)
{
  Repo *repo;
  int i;
  Pool *sigpool = 0;

  for (i = 0; i < nrepoinfos; i++)
    {
      struct repoinfo *cinfo = repoinfos + i;
      if (!cinfo->enabled)
	continue;

      repo = repo_create(pool, cinfo->alias);
      cinfo->repo = repo;
      repo->appdata = cinfo;
      repo->priority = 99 - cinfo->priority;

      if ((!cinfo->autorefresh || cinfo->metadata_expire) && usecachedrepo(cinfo, 0, 0))
	{
#ifdef SUSE
	  repo_add_autopattern(cinfo->repo, 0);
#endif
	  printf("repo '%s':", cinfo->alias);
	  printf(" cached\n");
	  continue;
	}

      switch (cinfo->type)
	{
#ifdef ENABLE_RPMMD
        case TYPE_RPMMD:
	  repomd_load(cinfo, &sigpool);
	  break;
#endif
#ifdef ENABLE_SUSEREPO
        case TYPE_SUSETAGS:
	  susetags_load(cinfo, &sigpool);
	  break;
#endif
#ifdef ENABLE_DEBIAN
        case TYPE_DEBIAN:
	  debian_load(cinfo, &sigpool);
	  break;
#endif
#ifdef ENABLE_MDKREPO
        case TYPE_MDK:
	  mdk_load(cinfo, &sigpool);
	  break;
#endif
	default:
	  printf("unsupported repo '%s': skipped\n", cinfo->alias);
	  repo_free(repo, 1);
	  cinfo->repo = 0;
	  break;
	}
#ifdef SUSE
      if (cinfo->repo)
        repo_add_autopattern(cinfo->repo, 0);
#endif
    }
  if (sigpool)
    pool_free(sigpool);
}

