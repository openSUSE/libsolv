#include <stdio.h>
#include <stdlib.h>
#include <unistd.h>
#include <fcntl.h>
#include <sys/types.h>
#include <sys/wait.h>

#include "pool.h"
#include "repo.h"
#include "chksum.h"
#include "solv_xfopen.h"

#include "repoinfo.h"
#include "mirror.h"
#include "checksig.h"
#if defined(FEDORA) || defined(MAGEIA)
#include "repoinfo_config_yum.h"
#endif
#include "repoinfo_download.h"

static inline int
opentmpfile()
{
  char tmpl[100];
  int fd;

  strcpy(tmpl, "/var/tmp/solvXXXXXX");
  fd = mkstemp(tmpl);
  if (fd < 0) 
    {    
      perror("mkstemp");
      exit(1);
    }    
  unlink(tmpl);
  return fd;
}

int
verify_checksum(int fd, const char *file, const unsigned char *chksum, Id chksumtype)
{
  char buf[1024];
  const unsigned char *sum;
  Chksum *h;
  int l;

  h = solv_chksum_create(chksumtype);
  if (!h)
    {
      printf("%s: unknown checksum type\n", file);
      return 0;
    }
  while ((l = read(fd, buf, sizeof(buf))) > 0)
    solv_chksum_add(h, buf, l);
  lseek(fd, 0, SEEK_SET);
  l = 0;
  sum = solv_chksum_get(h, &l);
  if (memcmp(sum, chksum, l))
    {
      printf("%s: checksum mismatch\n", file);
      solv_chksum_free(h, 0);
      return 0;
    }
  solv_chksum_free(h, 0);
  return 1;
}

FILE *
curlfopen(struct repoinfo *cinfo, const char *file, int uncompress, const unsigned char *chksum, Id chksumtype, int markincomplete)
{
  FILE *fp;
  pid_t pid;
  int fd;
  int status;
  char url[4096];
  const char *baseurl = cinfo->baseurl;

  if (!baseurl)
    {
      if (!cinfo->metalink && !cinfo->mirrorlist)
        return 0;
      if (file != cinfo->metalink && file != cinfo->mirrorlist)
	{
	  unsigned char mlchksum[32];
	  Id mlchksumtype = 0;
	  fp = curlfopen(cinfo, cinfo->metalink ? cinfo->metalink : cinfo->mirrorlist, 0, 0, 0, 0);
	  if (!fp)
	    return 0;
	  if (cinfo->metalink)
	    cinfo->baseurl = findmetalinkurl(fp, mlchksum, &mlchksumtype);
	  else
	    cinfo->baseurl = findmirrorlisturl(fp);
	  fclose(fp);
	  if (!cinfo->baseurl)
	    return 0;
#if defined(FEDORA) || defined(MAGEIA)
	  if (strchr(cinfo->baseurl, '$'))
	    {
	      char *b = yum_substitute(cinfo->repo->pool, cinfo->baseurl);
	      free(cinfo->baseurl);
	      cinfo->baseurl = strdup(b);
	    }
#endif
	  if (!chksumtype && mlchksumtype && !strcmp(file, "repodata/repomd.xml"))
	    {
	      chksumtype = mlchksumtype;
	      chksum = mlchksum;
	    }
	  return curlfopen(cinfo, file, uncompress, chksum, chksumtype, markincomplete);
	}
      snprintf(url, sizeof(url), "%s", file);
    }
  else
    {
      const char *path = cinfo->path && strcmp(cinfo->path, "/") != 0 ? cinfo->path : "";
      int l = strlen(baseurl);
      int pl = strlen(path);
      const char *sep = l && baseurl[l - 1] == '/' ? "" : "/";
      const char *psep = pl && cinfo->path[pl - 1] == '/' ? "" : "/";
      snprintf(url, sizeof(url), "%s%s%s%s%s", baseurl, sep, path, psep, file);
    }
  fd = opentmpfile();
  // printf("url: %s\n", url);
  if ((pid = fork()) == (pid_t)-1)
    {
      perror("fork");
      exit(1);
    }
  if (pid == 0)
    {
      if (fd != 1)
	{
          dup2(fd, 1);
	  close(fd);
	}
      execlp("curl", "curl", "-f", "-s", "-L", url, (char *)0);
      perror("curl");
      _exit(0);
    }
  status = 0;
  while (waitpid(pid, &status, 0) != pid)
    ;
  if (lseek(fd, 0, SEEK_END) == 0 && (!status || !chksumtype))
    {
      /* empty file */
      close(fd);
      return 0;
    }
  lseek(fd, 0, SEEK_SET);
  if (status)
    {
      printf("%s: download error %d\n", file, status >> 8 ? status >> 8 : status);
      if (markincomplete)
	cinfo->incomplete = 1;
      close(fd);
      return 0;
    }
  if (chksumtype && !verify_checksum(fd, file, chksum, chksumtype))
    {
      if (markincomplete)
	cinfo->incomplete = 1;
      close(fd);
      return 0;
    }
  fcntl(fd, F_SETFD, FD_CLOEXEC);
  if (uncompress)
    {
      if (solv_xfopen_iscompressed(file) < 0)
	{
	  printf("%s: unsupported compression\n", file);
	  if (markincomplete)
	    cinfo->incomplete = 1;
	  close(fd);
	  return 0;
	}
      fp = solv_xfopen_fd(file, fd, "r");
    }
  else
    fp = fdopen(fd, "r");
  if (!fp)
    close(fd);
  return fp;
}

FILE *
downloadpackage(Solvable *s, const char *loc)
{
  const unsigned char *chksum;
  Id chksumtype;
  struct repoinfo *cinfo = s->repo->appdata;

#ifdef ENABLE_SUSEREPO
  if (cinfo->type == TYPE_SUSETAGS)
    {
      const char *datadir = repo_lookup_str(cinfo->repo, SOLVID_META, SUSETAGS_DATADIR);
      loc = pool_tmpjoin(s->repo->pool, datadir ? datadir : "suse", "/", loc);
    }
#endif
  chksumtype = 0;
  chksum = solvable_lookup_bin_checksum(s, SOLVABLE_CHECKSUM, &chksumtype);
  return curlfopen(cinfo, loc, 0, chksum, chksumtype, 0);
}

int
downloadchecksig(struct repoinfo *cinfo, FILE *fp, const char *sigurl, Pool **sigpool)
{
  FILE *sigfp;
  sigfp = curlfopen(cinfo, sigurl, 0, 0, 0, 0); 
  if (!sigfp)
    {    
      printf(" unsigned, skipped\n");
      return 0;
    }    
  if (!*sigpool)
    *sigpool = read_sigs();
  if (!checksig(*sigpool, fp, sigfp))
    {    
      printf(" checksig failed, skipped\n");
      fclose(sigfp);
      return 0;
    }    
  fclose(sigfp);
  return 1;
}

