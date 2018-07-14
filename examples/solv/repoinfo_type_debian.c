#ifdef ENABLE_DEBIAN

#include <stdio.h>
#include <stdlib.h>
#include <unistd.h>
#include <sys/utsname.h>

#include "pool.h"
#include "repo.h"
#include "chksum.h"
#include "repo_deb.h"

#include "repoinfo.h"
#include "repoinfo_cache.h"
#include "repoinfo_download.h"
#include "repoinfo_type_debian.h"

static const char *
debian_find_component(struct repoinfo *cinfo, FILE *fp, char *comp, const unsigned char **chksump, Id *chksumtypep)
{
  char buf[4096];
  Id chksumtype;
  unsigned char *chksum;
  Id curchksumtype;
  int l, compl;
  char *ch, *fn, *bp;
  char *filename;
  static char *basearch;
  char *binarydir;
  int lbinarydir;

  if (!basearch)
    {
      struct utsname un;
      if (uname(&un))
	{
	  perror("uname");
	  exit(1);
	}
      basearch = strdup(un.machine);
      if (basearch[0] == 'i' && basearch[1] && !strcmp(basearch + 2, "86"))
	basearch[1] = '3';
    }
  binarydir = solv_dupjoin("binary-", basearch, "/");
  lbinarydir = strlen(binarydir);
  compl = strlen(comp);
  rewind(fp);
  curchksumtype = 0;
  filename = 0;
  chksum = solv_malloc(32);
  chksumtype = 0;
  while(fgets(buf, sizeof(buf), fp))
    {
      l = strlen(buf);
      if (l == 0)
	continue;
      while (l && (buf[l - 1] == '\n' || buf[l - 1] == ' ' || buf[l - 1] == '\t'))
	buf[--l] = 0;
      if (!strncasecmp(buf, "MD5Sum:", 7))
	{
	  curchksumtype = REPOKEY_TYPE_MD5;
	  continue;
	}
      if (!strncasecmp(buf, "SHA1:", 5))
	{
	  curchksumtype = REPOKEY_TYPE_SHA1;
	  continue;
	}
      if (!strncasecmp(buf, "SHA256:", 7))
	{
	  curchksumtype = REPOKEY_TYPE_SHA256;
	  continue;
	}
      if (!curchksumtype)
	continue;
      bp = buf;
      if (*bp++ != ' ')
	{
	  curchksumtype = 0;
	  continue;
	}
      ch = bp;
      while (*bp && *bp != ' ' && *bp != '\t')
	bp++;
      if (!*bp)
	continue;
      *bp++ = 0;
      while (*bp == ' ' || *bp == '\t')
	bp++;
      while (*bp && *bp != ' ' && *bp != '\t')
	bp++;
      if (!*bp)
	continue;
      while (*bp == ' ' || *bp == '\t')
	bp++;
      fn = bp;
      if (strncmp(fn, comp, compl) != 0 || fn[compl] != '/')
	continue;
      bp += compl + 1;
      if (strncmp(bp, binarydir, lbinarydir))
	continue;
      bp += lbinarydir;
      if (!strcmp(bp, "Packages") || !strcmp(bp, "Packages.gz"))
	{
	  unsigned char curchksum[32];
	  int curl;
	  if (filename && !strcmp(bp, "Packages"))
	    continue;
	  curl = solv_chksum_len(curchksumtype);
	  if (!curl || (chksumtype && solv_chksum_len(chksumtype) > curl))
	    continue;
          if (solv_hex2bin((const char **)&ch, curchksum, sizeof(curchksum)) != curl)
	    continue;
	  solv_free(filename);
	  filename = strdup(fn);
	  chksumtype = curchksumtype;
	  memcpy(chksum, curchksum, curl);
	}
    }
  free(binarydir);
  if (filename)
    {
      fn = solv_dupjoin("/", filename, 0);
      solv_free(filename);
      filename = solv_dupjoin("dists/", cinfo->name, fn);
      solv_free(fn);
    }
  if (!chksumtype)
    chksum = solv_free(chksum);
  *chksump = chksum;
  *chksumtypep = chksumtype;
  return filename;
}

int
debian_load(struct repoinfo *cinfo, Pool **sigpoolp)
{
  Repo *repo = cinfo->repo;
  Pool *pool = repo->pool;
  const char *filename;
  const unsigned char *filechksum;
  Id filechksumtype;
  FILE *fp, *fpr;
  int j;

  printf("debian repo '%s':", cinfo->alias);
  fflush(stdout);
  filename = solv_dupjoin("dists/", cinfo->name, "/Release");
  if ((fpr = curlfopen(cinfo, filename, 0, 0, 0, 0)) == 0)
    {
      printf(" no Release file\n");
      free((char *)filename);
      cinfo->incomplete = 1;
      return 0;
    }
  solv_free((char *)filename);
  if (cinfo->repo_gpgcheck)
    {
      filename = solv_dupjoin("dists/", cinfo->name, "/Release.gpg");
      if (!downloadchecksig(cinfo, fpr, filename, sigpoolp))
	{
	  fclose(fpr);
	  solv_free((char *)filename);
	  cinfo->incomplete = 1;
	  return 0;
	}
      solv_free((char *)filename);
    }
  calc_cookie_fp(fpr, REPOKEY_TYPE_SHA256, cinfo->cookie);
  cinfo->cookieset = 1;
  if (usecachedrepo(cinfo, 0, 1))
    {
      printf(" cached\n");
      fclose(fpr);
      return 1;
    }
  printf(" fetching\n");
  for (j = 0; j < cinfo->ncomponents; j++)
    {
      if (!(filename = debian_find_component(cinfo, fpr, cinfo->components[j], &filechksum, &filechksumtype)))
	{
	  printf("[component %s not found]\n", cinfo->components[j]);
	  continue;
	}
      if ((fp = curlfopen(cinfo, filename, 1, filechksum, filechksumtype, 1)) != 0)
	{
	  if (repo_add_debpackages(repo, fp, 0))
	    {
	      printf("component %s: %s\n", cinfo->components[j], pool_errstr(pool));
	      cinfo->incomplete = 1;
	    }
	  fclose(fp);
	}
      solv_free((char *)filechksum);
      solv_free((char *)filename);
    }
  fclose(fpr);
  writecachedrepo(cinfo, 0, 0);
  return 1;
}

#endif
