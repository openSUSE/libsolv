#ifdef ENABLE_MDKREPO

#include <stdio.h>
#include <stdlib.h>
#include <unistd.h>

#include "pool.h"
#include "repo.h"
#include "chksum.h"
#include "repo_mdk.h"
#include "solv_xfopen.h"

#include "repoinfo.h"
#include "repoinfo_cache.h"
#include "repoinfo_download.h"
#include "repoinfo_type_mdk.h"

static int
mdk_find(const char *md5sums, const char *what, unsigned char *chksum)
{
  const char *sp, *ep;
  int wl = strlen(what);
  for (sp = md5sums; (ep = strchr(sp, '\n')) != 0; sp = ep + 1)
    {
      int l = ep - sp;
      if (l <= 34)
	continue;
      if (sp[32] != ' ' || sp[33] != ' ')
	continue;
      if (wl != l - 34 || strncmp(what, sp + 34, wl) != 0)
	continue;
      if (solv_hex2bin(&sp, chksum, 16) != 16)
	continue;
      return 1;
    }
  return 0;
}

static char *
slurp(FILE *fp)
{
  int l, ll;
  char *buf = 0;
  int bufl = 0;

  for (l = 0; ; l += ll)
    {
      if (bufl - l < 4096)
        {
          bufl += 4096;
          buf = solv_realloc(buf, bufl);
        }
      ll = fread(buf + l, 1, bufl - l, fp);
      if (ll < 0)
        {
          buf = solv_free(buf);
          l = 0;
          break;
        }
      if (ll == 0)
        {
          buf[l] = 0;
          break;
        }
    }
  return buf;
}

int
mdk_load_ext(Repo *repo, Repodata *data)
{
  struct repoinfo *cinfo = repo->appdata;
  const char *type, *ext, *filename;
  const unsigned char *filechksum;
  Id filechksumtype;
  int r = 0;
  FILE *fp;

  type = repodata_lookup_str(data, SOLVID_META, REPOSITORY_REPOMD_TYPE);
  if (strcmp(type, "filelists") != 0)
    return 0;
  ext = "FL";
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
  r = repo_add_mdk_info(repo, fp, REPO_USE_LOADING|REPO_EXTEND_SOLVABLES|REPO_LOCALPOOL);
  fclose(fp);
  if (r)
    {
      printf("%s\n", pool_errstr(repo->pool));
      return 0;
    }
  writecachedrepo(cinfo, ext, data);
  return 1;
}

static void
mdk_add_ext(Repo *repo, Repodata *data, const char *what, const char *ext, const char *filename, Id chksumtype, const unsigned char *chksum)
{
  Id handle = repodata_new_handle(data);
  /* we mis-use the repomd ids here... need something generic in the future */
  repodata_set_poolstr(data, handle, REPOSITORY_REPOMD_TYPE, what);
  repodata_set_str(data, handle, REPOSITORY_REPOMD_LOCATION, filename);
  repodata_set_bin_checksum(data, handle, REPOSITORY_REPOMD_CHECKSUM, chksumtype, chksum);
  add_ext_keys(data, handle, ext);
  repodata_add_flexarray(data, SOLVID_META, REPOSITORY_EXTERNAL, handle);
}

int
mdk_load(struct repoinfo *cinfo, Pool **sigpoolp)
{
  Repo *repo = cinfo->repo;
  Pool *pool = repo->pool;
  Repodata *data;
  const char *compression;
  FILE *fp, *cfp;
  char *md5sums;
  unsigned char probe[5];
  unsigned char md5[16];

  printf("mdk repo '%s':", cinfo->alias);
  fflush(stdout);
  if ((fp = curlfopen(cinfo, "media_info/MD5SUM", 0, 0, 0, 0)) == 0)
    {
      printf(" no media_info/MD5SUM file\n");
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
  md5sums = slurp(fp);
  fclose(fp);
  printf(" fetching\n");
  if (!mdk_find(md5sums, "synthesis.hdlist.cz", md5))
    {
      solv_free(md5sums);
      cinfo->incomplete = 1;
      return 0;	/* hopeless */
    }
  if ((fp = curlfopen(cinfo, "media_info/synthesis.hdlist.cz", 0, md5, REPOKEY_TYPE_MD5, 1)) == 0)
    {
      solv_free(md5sums);
      cinfo->incomplete = 1;
      return 0;	/* hopeless */
    }
  /* probe compression */
  if (fread(probe, 5, 1, fp) != 1)
    {
      fclose(fp);
      solv_free(md5sums);
      cinfo->incomplete = 1;
      return 0;	/* hopeless */
    }
  if (probe[0] == 0xfd && memcmp(probe + 1, "7zXZ", 4) == 0)
    compression = "synthesis.hdlist.xz";
  else
    compression = "synthesis.hdlist.gz";
  lseek(fileno(fp), 0, SEEK_SET);
  cfp = solv_xfopen_fd(compression, dup(fileno(fp)), "r");
  fclose(fp);
  fp = cfp;
  if (!fp)
    {
      solv_free(md5sums);
      cinfo->incomplete = 1;
      return 0;	/* hopeless */
    }
  if (repo_add_mdk(repo, fp, REPO_NO_INTERNALIZE))
    {
      printf("synthesis.hdlist.cz: %s\n", pool_errstr(pool));
      fclose(fp);
      solv_free(md5sums);
      cinfo->incomplete = 1;
      return 0;	/* hopeless */
    }
  fclose(fp);
  /* add info, could do this on demand, but always having the summary is nice */
  if (mdk_find(md5sums, "info.xml.lzma", md5))
    {
      if ((fp = curlfopen(cinfo, "media_info/info.xml.lzma", 1, md5, REPOKEY_TYPE_MD5, 1)) != 0)
	{
	  if (repo_add_mdk_info(repo, fp, REPO_NO_INTERNALIZE|REPO_REUSE_REPODATA|REPO_EXTEND_SOLVABLES))
	    {
	      printf("info.xml.lzma: %s\n", pool_errstr(pool));
	      cinfo->incomplete = 1;
	    }
	  fclose(fp);
	}
    }
  repo_internalize(repo);
  data = repo_add_repodata(repo, 0);
  /* setup on-demand loading of filelist data */
  if (mdk_find(md5sums, "files.xml.lzma", md5))
    {
      repodata_extend_block(data, repo->start, repo->end - repo->start);
      mdk_add_ext(repo, data, "filelists", "FL", "media_info/files.xml.lzma", REPOKEY_TYPE_MD5, md5);
    }
  solv_free(md5sums);
  repodata_internalize(data);
  writecachedrepo(cinfo, 0, 0);
  repodata_create_stubs(repo_last_repodata(repo));
  return 1;
}

#endif
