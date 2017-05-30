#ifdef ENABLE_RPMMD

#include <stdio.h>
#include <stdlib.h>
#include <unistd.h>

#include "pool.h"
#include "repo.h"
#include "chksum.h"
#include "repo_rpmmd.h"
#include "repo_deltainfoxml.h"
#include "repo_updateinfoxml.h"
#include "repo_repomdxml.h"
#ifdef ENABLE_APPDATA
#include "repo_appdata.h"
#endif

#include "repoinfo.h"
#include "repoinfo_cache.h"
#include "repoinfo_download.h"
#include "repoinfo_type_rpmmd.h"



static const char *
repomd_find(Repo *repo, const char *what, const unsigned char **chksump, Id *chksumtypep)
{
  Pool *pool = repo->pool;
  Dataiterator di;
  const char *filename;

  filename = 0;
  *chksump = 0;
  *chksumtypep = 0;
  dataiterator_init(&di, pool, repo, SOLVID_META, REPOSITORY_REPOMD_TYPE, what, SEARCH_STRING);
  dataiterator_prepend_keyname(&di, REPOSITORY_REPOMD);
  if (dataiterator_step(&di))
    {
      dataiterator_setpos_parent(&di);
      filename = pool_lookup_str(pool, SOLVID_POS, REPOSITORY_REPOMD_LOCATION);
      *chksump = pool_lookup_bin_checksum(pool, SOLVID_POS, REPOSITORY_REPOMD_CHECKSUM, chksumtypep);
    }
  dataiterator_free(&di);
  if (filename && !*chksumtypep)
    {
      printf("no %s file checksum!\n", what);
      filename = 0;
    }
  return filename;
}

static void
repomd_add_ext(Repo *repo, Repodata *data, const char *what, const char *ext)
{
  Id chksumtype, handle;
  const unsigned char *chksum;
  const char *filename;

  filename = repomd_find(repo, what, &chksum, &chksumtype);
  if (!filename && !strcmp(what, "deltainfo"))
    filename = repomd_find(repo, "prestodelta", &chksum, &chksumtype);
  if (!filename)
    return;
  handle = repodata_new_handle(data);
  repodata_set_poolstr(data, handle, REPOSITORY_REPOMD_TYPE, what);
  repodata_set_str(data, handle, REPOSITORY_REPOMD_LOCATION, filename);
  repodata_set_bin_checksum(data, handle, REPOSITORY_REPOMD_CHECKSUM, chksumtype, chksum);
  add_ext_keys(data, handle, ext);
  repodata_add_flexarray(data, SOLVID_META, REPOSITORY_EXTERNAL, handle);
}

int
repomd_load_ext(Repo *repo, Repodata *data)
{
  const char *filename, *repomdtype;
  char ext[3];
  FILE *fp;
  struct repoinfo *cinfo;
  const unsigned char *filechksum;
  Id filechksumtype;
  int r = 0;

  cinfo = repo->appdata;
  repomdtype = repodata_lookup_str(data, SOLVID_META, REPOSITORY_REPOMD_TYPE);
  if (!repomdtype)
    return 0;
  if (!strcmp(repomdtype, "filelists"))
    strcpy(ext, "FL");
  else if (!strcmp(repomdtype, "deltainfo"))
    strcpy(ext, "DL");
  else
    return 0;
  printf("[%s:%s", repo->name, ext);
  if (usecachedrepo(cinfo, ext, 0))
    {
      printf(" cached]\n"); fflush(stdout);
      return 1;
    }
  printf(" fetching]\n"); fflush(stdout);
  filename = repodata_lookup_str(data, SOLVID_META, REPOSITORY_REPOMD_LOCATION);
  filechksumtype = 0;
  filechksum = repodata_lookup_bin_checksum(data, SOLVID_META, REPOSITORY_REPOMD_CHECKSUM, &filechksumtype);
  if ((fp = curlfopen(cinfo, filename, 1, filechksum, filechksumtype, 0)) == 0)
    return 0;
  if (!strcmp(ext, "FL"))
    r = repo_add_rpmmd(repo, fp, ext, REPO_USE_LOADING|REPO_EXTEND_SOLVABLES|REPO_LOCALPOOL);
  else if (!strcmp(ext, "DL"))
    r = repo_add_deltainfoxml(repo, fp, REPO_USE_LOADING);
  fclose(fp);
  if (r)
    {
      printf("%s\n", pool_errstr(repo->pool));
      return 0;
    }
  if (cinfo->extcookieset)
    writecachedrepo(cinfo, ext, data);
  return 1;
}

int
repomd_load(struct repoinfo *cinfo, Pool **sigpoolp)
{
  Repo *repo = cinfo->repo;
  Pool *pool = repo->pool;
  Repodata *data;
  const char *filename;
  const unsigned char *filechksum;
  Id filechksumtype;
  FILE *fp;

  printf("rpmmd repo '%s':", cinfo->alias);
  fflush(stdout);
  if ((fp = curlfopen(cinfo, "repodata/repomd.xml", 0, 0, 0, 0)) == 0)
    {
      printf(" no repomd.xml file\n");
      cinfo->incomplete = 1;
      return 0;
    }
  calc_cookie_fp(fp, REPOKEY_TYPE_SHA256, cinfo->cookie);
  cinfo->cookieset = 1;
  if (usecachedrepo(cinfo, 0, 1))
    {
      printf(" cached\n");
      fclose(fp);
      return 1;
    }
  if (cinfo->repo_gpgcheck && !downloadchecksig(cinfo, fp, "repodata/repomd.xml.asc", sigpoolp))
    {
      fclose(fp);
      cinfo->incomplete = 1;
      return 0;
    }
  if (repo_add_repomdxml(repo, fp, 0))
    {
      printf("repomd.xml: %s\n", pool_errstr(pool));
      cinfo->incomplete = 1;
      fclose(fp);
      return 0;
    }
  fclose(fp);
  printf(" fetching\n");
  filename = repomd_find(repo, "primary", &filechksum, &filechksumtype);
  if (filename && (fp = curlfopen(cinfo, filename, 1, filechksum, filechksumtype, 1)) != 0)
    {
      if (repo_add_rpmmd(repo, fp, 0, 0))
	{
	  printf("primary: %s\n", pool_errstr(pool));
	  cinfo->incomplete = 1;
	}
      fclose(fp);
    }
  if (cinfo->incomplete)
    return 0;	/* hopeless */

  filename = repomd_find(repo, "updateinfo", &filechksum, &filechksumtype);
  if (filename && (fp = curlfopen(cinfo, filename, 1, filechksum, filechksumtype, 1)) != 0)
    {
      if (repo_add_updateinfoxml(repo, fp, 0))
	{
	  printf("updateinfo: %s\n", pool_errstr(pool));
	  cinfo->incomplete = 1;
	}
      fclose(fp);
    }

#ifdef ENABLE_APPDATA
  filename = repomd_find(repo, "appdata", &filechksum, &filechksumtype);
  if (filename && (fp = curlfopen(cinfo, filename, 1, filechksum, filechksumtype, 1)) != 0)
    {
      if (repo_add_appdata(repo, fp, 0))
	{
	  printf("appdata: %s\n", pool_errstr(pool));
	  cinfo->incomplete = 1;
	}
      fclose(fp);
    }
#endif
  data = repo_add_repodata(repo, 0);
  repodata_extend_block(data, repo->start, repo->end - repo->start);
  repomd_add_ext(repo, data, "deltainfo", "DL");
  repomd_add_ext(repo, data, "filelists", "FL");
  repodata_internalize(data);
  writecachedrepo(cinfo, 0, 0);
  repodata_create_stubs(repo_last_repodata(repo));
  return 1;
}

#endif
