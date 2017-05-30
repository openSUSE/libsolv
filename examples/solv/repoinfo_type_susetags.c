#ifdef ENABLE_SUSEREPO

#include <stdio.h>
#include <stdlib.h>
#include <unistd.h>

#include "pool.h"
#include "repo.h"
#include "chksum.h"
#include "repo_content.h"
#include "repo_susetags.h"
#ifdef ENABLE_APPDATA
#include "repo_appdata.h"
#endif

#include "repoinfo.h"
#include "repoinfo_cache.h"
#include "repoinfo_download.h"
#include "repoinfo_type_susetags.h"

/* susetags helpers */

static const char *
susetags_find(Repo *repo, const char *what, const unsigned char **chksump, Id *chksumtypep)
{
  Pool *pool = repo->pool;
  Dataiterator di;
  const char *filename;

  filename = 0;
  *chksump = 0;
  *chksumtypep = 0;
  dataiterator_init(&di, pool, repo, SOLVID_META, SUSETAGS_FILE_NAME, what, SEARCH_STRING);
  dataiterator_prepend_keyname(&di, SUSETAGS_FILE);
  if (dataiterator_step(&di))
    {
      dataiterator_setpos_parent(&di);
      *chksump = pool_lookup_bin_checksum(pool, SOLVID_POS, SUSETAGS_FILE_CHECKSUM, chksumtypep);
      filename = what;
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
susetags_add_ext(Repo *repo, Repodata *data)
{
  Pool *pool = repo->pool;
  Dataiterator di;
  char ext[3];
  Id handle, filechksumtype;
  const unsigned char *filechksum;

  dataiterator_init(&di, pool, repo, SOLVID_META, SUSETAGS_FILE_NAME, 0, 0);
  dataiterator_prepend_keyname(&di, SUSETAGS_FILE);
  while (dataiterator_step(&di))
    {
      if (strncmp(di.kv.str, "packages.", 9) != 0)
	continue;
      if (!strcmp(di.kv.str + 9, "gz"))
	continue;
      if (!di.kv.str[9] || !di.kv.str[10] || (di.kv.str[11] && di.kv.str[11] != '.'))
	continue;
      ext[0] = di.kv.str[9];
      ext[1] = di.kv.str[10];
      ext[2] = 0;
      if (!strcmp(ext, "en"))
	continue;
      if (!susetags_find(repo, di.kv.str, &filechksum, &filechksumtype))
	continue;
      handle = repodata_new_handle(data);
      repodata_set_str(data, handle, SUSETAGS_FILE_NAME, di.kv.str);
      if (filechksumtype)
	repodata_set_bin_checksum(data, handle, SUSETAGS_FILE_CHECKSUM, filechksumtype, filechksum);
      add_ext_keys(data, handle, ext);
      repodata_add_flexarray(data, SOLVID_META, REPOSITORY_EXTERNAL, handle);
    }
  dataiterator_free(&di);
}

int
susetags_load_ext(Repo *repo, Repodata *data)
{
  const char *filename, *descrdir;
  Id defvendor;
  char ext[3];
  FILE *fp;
  struct repoinfo *cinfo;
  const unsigned char *filechksum;
  Id filechksumtype;
  int flags;

  cinfo = repo->appdata;
  filename = repodata_lookup_str(data, SOLVID_META, SUSETAGS_FILE_NAME);
  if (!filename)
    return 0;
  /* susetags load */
  ext[0] = filename[9];
  ext[1] = filename[10];
  ext[2] = 0;
  printf("[%s:%s", repo->name, ext);
  if (usecachedrepo(cinfo, ext, 0))
    {
      printf(" cached]\n"); fflush(stdout);
      return 1;
    }
  printf(" fetching]\n"); fflush(stdout);
  defvendor = repo_lookup_id(repo, SOLVID_META, SUSETAGS_DEFAULTVENDOR);
  descrdir = repo_lookup_str(repo, SOLVID_META, SUSETAGS_DESCRDIR);
  if (!descrdir)
    descrdir = "suse/setup/descr";
  filechksumtype = 0;
  filechksum = repodata_lookup_bin_checksum(data, SOLVID_META, SUSETAGS_FILE_CHECKSUM, &filechksumtype);
  if ((fp = curlfopen(cinfo, pool_tmpjoin(repo->pool, descrdir, "/", filename), 1, filechksum, filechksumtype, 0)) == 0)
    return 0;
  flags = REPO_USE_LOADING|REPO_EXTEND_SOLVABLES;
  if (strcmp(ext, "DL") != 0)
    flags |= REPO_LOCALPOOL;
  if (repo_add_susetags(repo, fp, defvendor, ext, flags))
    {
      fclose(fp);
      printf("%s\n", pool_errstr(repo->pool));
      return 0;
    }
  fclose(fp);
  writecachedrepo(cinfo, ext, data);
  return 1;
}

int
susetags_load(struct repoinfo *cinfo, Pool **sigpoolp)
{
  Repo *repo = cinfo->repo;
  Pool *pool = repo->pool;
  Repodata *data;
  const char *filename;
  const unsigned char *filechksum;
  Id filechksumtype;
  FILE *fp;
  const char *descrdir;
  int defvendor;

  printf("susetags repo '%s':", cinfo->alias);
  fflush(stdout);
  descrdir = 0;
  defvendor = 0;
  if ((fp = curlfopen(cinfo, "content", 0, 0, 0, 0)) == 0)
    {
      printf(" no content file\n");
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
  if (cinfo->repo_gpgcheck && !downloadchecksig(cinfo, fp, "content.asc", sigpoolp))
    {
      fclose(fp);
      cinfo->incomplete = 1;
      return 0;
    }
  if (repo_add_content(repo, fp, 0))
    {
      printf("content: %s\n", pool_errstr(pool));
      fclose(fp);
      cinfo->incomplete = 1;
      return 0;
    }
  fclose(fp);
  defvendor = repo_lookup_id(repo, SOLVID_META, SUSETAGS_DEFAULTVENDOR);
  descrdir = repo_lookup_str(repo, SOLVID_META, SUSETAGS_DESCRDIR);
  if (!descrdir)
    descrdir = "suse/setup/descr";
  filename = susetags_find(repo, "packages.gz", &filechksum, &filechksumtype);
  if (!filename)
    filename = susetags_find(repo, "packages", &filechksum, &filechksumtype);
  if (!filename)
    {
      printf(" no packages file entry, skipped\n");
      cinfo->incomplete = 1;
      return 0;
    }
  printf(" fetching\n");
  if ((fp = curlfopen(cinfo, pool_tmpjoin(pool, descrdir, "/", filename), 1, filechksum, filechksumtype, 1)) == 0)
    {
      cinfo->incomplete = 1;
      return 0;	/* hopeless */
    }
  if (repo_add_susetags(repo, fp, defvendor, 0, REPO_NO_INTERNALIZE|SUSETAGS_RECORD_SHARES))
    {
      printf("packages: %s\n", pool_errstr(pool));
      fclose(fp);
      cinfo->incomplete = 1;
      return 0;	/* hopeless */
    }
  fclose(fp);
  /* add default language */
  filename = susetags_find(repo, "packages.en.gz", &filechksum, &filechksumtype);
  if (!filename)
    filename = susetags_find(repo, "packages.en", &filechksum, &filechksumtype);
  if (filename)
    {
      if ((fp = curlfopen(cinfo, pool_tmpjoin(pool, descrdir, "/", filename), 1, filechksum, filechksumtype, 1)) != 0)
	{
	  if (repo_add_susetags(repo, fp, defvendor, 0, REPO_NO_INTERNALIZE|REPO_REUSE_REPODATA|REPO_EXTEND_SOLVABLES))
	    {
	      printf("packages.en: %s\n", pool_errstr(pool));
	      cinfo->incomplete = 1;
	    }
	  fclose(fp);
	}
    }
  filename = susetags_find(repo, "patterns", &filechksum, &filechksumtype);
  if (filename)
    {
      if ((fp = curlfopen(cinfo, pool_tmpjoin(pool, descrdir, "/", filename), 1, filechksum, filechksumtype, 1)) != 0)
	{
	  char pbuf[256];
	  while (fgets(pbuf, sizeof(pbuf), fp))
	    {
	      int l = strlen(pbuf);
	      FILE *fp2;
	      if (l && pbuf[l - 1] == '\n')
		pbuf[--l] = 0;
	      if (!*pbuf || *pbuf == '.' || strchr(pbuf, '/') != 0)
		continue;
	      filename = susetags_find(repo, pbuf, &filechksum, &filechksumtype);
	      if (filename && (fp2 = curlfopen(cinfo, pool_tmpjoin(pool, descrdir, "/", filename), 1, filechksum, filechksumtype, 1)) != 0)
		{
		  if (repo_add_susetags(repo, fp2, defvendor, 0, REPO_NO_INTERNALIZE))
		    {
		      printf("%s: %s\n", pbuf, pool_errstr(pool));
		      cinfo->incomplete = 1;
		    }
		  fclose(fp2);
		}
	    }
	  fclose(fp);
	}
    }
#ifdef ENABLE_APPDATA
  filename = susetags_find(repo, "appdata.xml.gz", &filechksum, &filechksumtype);
  if (!filename)
    filename = susetags_find(repo, "appdata.xml", &filechksum, &filechksumtype);
  if (filename && (fp = curlfopen(cinfo, pool_tmpjoin(pool, descrdir, "/", filename), 1, filechksum, filechksumtype, 1)) != 0)
    {
      if (repo_add_appdata(repo, fp, 0))
	{
	  printf("appdata: %s\n", pool_errstr(pool));
	  cinfo->incomplete = 1;
	}
      fclose(fp);
    }
#endif
  repo_internalize(repo);
  data = repo_add_repodata(repo, 0);
  repodata_extend_block(data, repo->start, repo->end - repo->start);
  susetags_add_ext(repo, data);
  repodata_internalize(data);
  writecachedrepo(cinfo, 0, 0);
  repodata_create_stubs(repo_last_repodata(repo));
  return 1;
}

#endif
