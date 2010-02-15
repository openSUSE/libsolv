/*
 * Copyright (c) 2009, Novell Inc.
 *
 * This program is licensed under the BSD license, read LICENSE.BSD
 * for further information
 */

/* solv, a little software installer demoing the sat solver library */

/* things available in the library but missing from solv:
 * - vendor policy loading
 * - soft locks file handling
 * - multi version handling
 */

#define _GNU_SOURCE

#include <stdio.h>
#include <stdlib.h>
#include <dirent.h>
#include <fnmatch.h>
#include <unistd.h>
#include <zlib.h>
#include <fcntl.h>
#include <assert.h>
#include <sys/utsname.h>
#include <sys/types.h>
#include <sys/wait.h>
#include <time.h>
#include <sys/time.h>

#include <sys/socket.h>
#include <netdb.h>
#include <poll.h>
#include <errno.h>

#include "pool.h"
#include "poolarch.h"
#include "repo.h"
#include "evr.h"
#include "policy.h"
#include "util.h"
#include "solver.h"
#include "solverdebug.h"
#include "chksum.h"
#include "repo_solv.h"

#include "repo_write.h"
#include "repo_rpmdb.h"
#include "repo_products.h"
#include "repo_rpmmd.h"
#include "repo_susetags.h"
#include "repo_repomdxml.h"
#include "repo_updateinfoxml.h"
#include "repo_deltainfoxml.h"
#include "repo_content.h"
#include "pool_fileconflicts.h"


#ifdef FEDORA
# define REPOINFO_PATH "/etc/yum.repos.d"
#else
# define REPOINFO_PATH "/etc/zypp/repos.d"
# define PRODUCTS_PATH "/etc/products.d"
# define SOFTLOCKS_PATH "/var/lib/zypp/SoftLocks"
#endif

#define SOLVCACHE_PATH "/var/cache/solv"

#define METADATA_EXPIRE (60 * 90)

struct repoinfo {
  Repo *repo;

  char *alias;
  char *name;
  int enabled;
  int autorefresh;
  char *baseurl;
  char *metalink;
  char *mirrorlist;
  char *path;
  int type;
  int pkgs_gpgcheck;
  int repo_gpgcheck;
  int priority;
  int keeppackages;
  int metadata_expire;

  unsigned char cookie[32];
  unsigned char extcookie[32];
};

#ifdef FEDORA
char *
yum_substitute(Pool *pool, char *line)
{
  char *p, *p2;
  static char *releaseevr;
  static char *basearch;

  if (!line)
    {
      sat_free(releaseevr);
      releaseevr = 0;
      sat_free(basearch);
      basearch = 0;
      return 0;
    }
  p = line;
  while ((p2 = strchr(p, '$')) != 0)
    {
      if (!strncmp(p2, "$releasever", 11))
	{
	  if (!releaseevr)
	    {
	      Queue q;
	
	      queue_init(&q);
	      rpm_installedrpmdbids(0, "Providename", "redhat-release", &q);
	      if (q.count)
		{
		  void *handle, *state = 0;
		  char *p;
		  handle = rpm_byrpmdbid(q.elements[0], 0, &state);
		  releaseevr = rpm_query(handle, SOLVABLE_EVR);
		  rpm_byrpmdbid(0, 0, &state);
		  if ((p = strchr(releaseevr, '-')) != 0)
		    *p = 0;
		}
	      queue_free(&q);
	      if (!releaseevr)
		releaseevr = strdup("?");
	    }
	  *p2 = 0;
	  p = pool_tmpjoin(pool, line, releaseevr, p2 + 11);
	  p2 = p + (p2 - line);
	  line = p;
	  p = p2 + strlen(releaseevr);
	  continue;
	}
      if (!strncmp(p2, "$basearch", 9))
	{
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
	  *p2 = 0;
	  p = pool_tmpjoin(pool, line, basearch, p2 + 9);
	  p2 = p + (p2 - line);
	  line = p;
	  p = p2 + strlen(basearch);
	  continue;
	}
      p = p2 + 1;
    }
  return line;
}
#endif

#define TYPE_UNKNOWN	0
#define TYPE_SUSETAGS	1
#define TYPE_RPMMD	2
#define TYPE_PLAINDIR	3

static int
read_repoinfos_sort(const void *ap, const void *bp)
{
  const struct repoinfo *a = ap;
  const struct repoinfo *b = bp;
  return strcmp(a->alias, b->alias);
}

struct repoinfo *
read_repoinfos(Pool *pool, const char *reposdir, int *nrepoinfosp)
{
  char buf[4096];
  char buf2[4096], *kp, *vp, *kpe;
  DIR *dir;
  FILE *fp;
  struct dirent *ent;
  int l, rdlen;
  struct repoinfo *repoinfos = 0, *cinfo;
  int nrepoinfos = 0;

  rdlen = strlen(reposdir);
  dir = opendir(reposdir);
  if (!dir)
    {
      *nrepoinfosp = 0;
      return 0;
    }
  while ((ent = readdir(dir)) != 0)
    {
      l = strlen(ent->d_name);
      if (l < 6 || rdlen + 2 + l >= sizeof(buf) || strcmp(ent->d_name + l - 5, ".repo") != 0)
	continue;
      snprintf(buf, sizeof(buf), "%s/%s", reposdir, ent->d_name);
      if ((fp = fopen(buf, "r")) == 0)
	{
	  perror(buf);
	  continue;
	}
      cinfo = 0;
      while(fgets(buf2, sizeof(buf2), fp))
	{
	  l = strlen(buf2);
	  if (l == 0)
	    continue;
	  while (l && (buf2[l - 1] == '\n' || buf2[l - 1] == ' ' || buf2[l - 1] == '\t'))
	    buf2[--l] = 0;
	  kp = buf2;
	  while (*kp == ' ' || *kp == '\t')
	    kp++;
	  if (!*kp || *kp == '#')
	    continue;
#ifdef FEDORA
	  if (strchr(kp, '$'))
	    kp = yum_substitute(pool, kp);
#endif
	  if (*kp == '[')
	    {
	      vp = strrchr(kp, ']');
	      if (!vp)
		continue;
	      *vp = 0;
	      repoinfos = sat_extend(repoinfos, nrepoinfos, 1, sizeof(*repoinfos), 15);
	      cinfo = repoinfos + nrepoinfos++;
	      memset(cinfo, 0, sizeof(*cinfo));
	      cinfo->alias = strdup(kp + 1);
	      cinfo->type = TYPE_RPMMD;
	      cinfo->autorefresh = 1;
	      cinfo->priority = 99;
#ifndef FEDORA
	      cinfo->repo_gpgcheck = 1;
#endif
	      cinfo->metadata_expire = METADATA_EXPIRE;
	      continue;
	    }
	  if (!cinfo)
	    continue;
          vp = strchr(kp, '=');
	  if (!vp)
	    continue;
	  for (kpe = vp - 1; kpe >= kp; kpe--)
	    if (*kpe != ' ' && *kpe != '\t')
	      break;
	  if (kpe == kp)
	    continue;
	  vp++;
	  while (*vp == ' ' || *vp == '\t')
	    vp++;
	  kpe[1] = 0;
	  if (!strcmp(kp, "name"))
	    cinfo->name = strdup(vp);
	  else if (!strcmp(kp, "enabled"))
	    cinfo->enabled = *vp == '0' ? 0 : 1;
	  else if (!strcmp(kp, "autorefresh"))
	    cinfo->autorefresh = *vp == '0' ? 0 : 1;
	  else if (!strcmp(kp, "gpgcheck"))
	    cinfo->pkgs_gpgcheck = *vp == '0' ? 0 : 1;
	  else if (!strcmp(kp, "repo_gpgcheck"))
	    cinfo->repo_gpgcheck = *vp == '0' ? 0 : 1;
	  else if (!strcmp(kp, "baseurl"))
	    cinfo->baseurl = strdup(vp);
	  else if (!strcmp(kp, "mirrorlist"))
	    {
	      if (strstr(vp, "metalink"))
	        cinfo->metalink = strdup(vp);
	      else
	        cinfo->mirrorlist = strdup(vp);
	    }
	  else if (!strcmp(kp, "path"))
	    {
	      if (vp && strcmp(vp, "/") != 0)
	        cinfo->path = strdup(vp);
	    }
	  else if (!strcmp(kp, "type"))
	    {
	      if (!strcmp(vp, "yast2"))
	        cinfo->type = TYPE_SUSETAGS;
	      else if (!strcmp(vp, "rpm-md"))
	        cinfo->type = TYPE_RPMMD;
	      else if (!strcmp(vp, "plaindir"))
	        cinfo->type = TYPE_PLAINDIR;
	      else
	        cinfo->type = TYPE_UNKNOWN;
	    }
	  else if (!strcmp(kp, "priority"))
	    cinfo->priority = atoi(vp);
	  else if (!strcmp(kp, "keeppackages"))
	    cinfo->keeppackages = *vp == '0' ? 0 : 1;
	}
      fclose(fp);
      cinfo = 0;
    }
  closedir(dir);
  qsort(repoinfos, nrepoinfos, sizeof(*repoinfos), read_repoinfos_sort);
  *nrepoinfosp = nrepoinfos;
  return repoinfos;
}

void
free_repoinfos(struct repoinfo *repoinfos, int nrepoinfos)
{
  int i;
  for (i = 0; i < nrepoinfos; i++)
    {
      struct repoinfo *cinfo = repoinfos + i;
      sat_free(cinfo->name);
      sat_free(cinfo->alias);
      sat_free(cinfo->path);
      sat_free(cinfo->metalink);
      sat_free(cinfo->mirrorlist);
      sat_free(cinfo->baseurl);
    }
  sat_free(repoinfos);
}

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

static int
verify_checksum(int fd, const char *file, const unsigned char *chksum, Id chksumtype)
{
  char buf[1024];
  unsigned char *sum;
  void *h;
  int l;

  h = sat_chksum_create(chksumtype);
  if (!h)
    {
      printf("%s: unknown checksum type\n", file);
      return 0;
    }
  while ((l = read(fd, buf, sizeof(buf))) > 0)
    sat_chksum_add(h, buf, l);
  lseek(fd, 0, SEEK_SET);
  l = 0;
  sum = sat_chksum_get(h, &l);
  if (memcmp(sum, chksum, l))
    {
      printf("%s: checksum mismatch\n", file);
      sat_chksum_free(h, 0);
      return 0;
    }
  sat_chksum_free(h, 0);
  return 1;
}

void
findfastest(char **urls, int nurls)
{
  int i, j, port;
  int *socks, qc;
  struct pollfd *fds;
  char *p, *p2, *q;
  char portstr[16];
  struct addrinfo hints, *result;;

  fds = sat_calloc(nurls, sizeof(*fds));
  socks = sat_calloc(nurls, sizeof(*socks));
  for (i = 0; i < nurls; i++)
    {
      socks[i] = -1;
      p = strchr(urls[i], '/');
      if (!p)
	continue;
      if (p[1] != '/')
	continue;
      p += 2;
      q = strchr(p, '/');
      qc = 0;
      if (q)
	{
	  qc = *q;
	  *q = 0;
	}
      if ((p2 = strchr(p, '@')) != 0)
	p = p2 + 1;
      port = 80;
      if (!strncmp("https:", urls[i], 6))
	port = 443;
      else if (!strncmp("ftp:", urls[i], 4))
	port = 21;
      if ((p2 = strrchr(p, ':')) != 0)
	{
	  port = atoi(p2 + 1);
	  if (q)
	    *q = qc;
	  q = p2;
	  qc = *q;
	  *q = 0;
	}
      sprintf(portstr, "%d", port);
      memset(&hints, 0, sizeof(struct addrinfo));
      hints.ai_family = AF_UNSPEC;
      hints.ai_socktype = SOCK_STREAM;
      hints.ai_flags = AI_NUMERICSERV;
      result = 0;
      if (!getaddrinfo(p, portstr, &hints, &result))
	{
	  socks[i] = socket(result->ai_family, result->ai_socktype, result->ai_protocol);
	  if (socks[i] >= 0)
	    {
	      fcntl(socks[i], F_SETFL, O_NONBLOCK);
	      if (connect(socks[i], result->ai_addr, result->ai_addrlen) == -1)
		{
		  if (errno != EINPROGRESS)
		    {
		      close(socks[i]);
		      socks[i] = -1;
		    }
		}
	    }
	  freeaddrinfo(result);
	}
      if (q)
	*q = qc;
    }
  for (;;)
    {
      for (i = j = 0; i < nurls; i++)
	{
	  if (socks[i] < 0)
	    continue;
	  fds[j].fd = socks[i];
	  fds[j].events = POLLOUT;
	  j++;
	}
      if (j < 2)
	{
	  i = j - 1;
	  break;
	}
      if (poll(fds, j, 10000) <= 0)
	{
	  i = -1;	/* something is wrong */
	  break;
	}
      for (i = 0; i < j; i++)
	if ((fds[i].revents & POLLOUT) != 0)
	  {
	    int soe = 0;
	    socklen_t soel = sizeof(int);
	    if (getsockopt(fds[i].fd, SOL_SOCKET, SO_ERROR, &soe, &soel) == -1 || soe != 0)
	      {
	        /* connect failed, kill socket */
	        for (j = 0; j < nurls; j++)
		  if (socks[j] == fds[i].fd)
		    {
		      close(socks[j]);
		      socks[j] = -1;
		    }
		i = j + 1;
		break;
	      }
	    break;	/* horray! */
	  }
      if (i == j + 1)
	continue;
      if (i == j)
        i = -1;		/* something is wrong, no bit was set */
      break;
    }
  /* now i contains the fastest fd index */
  if (i >= 0)
    {
      for (j = 0; j < nurls; j++)
	if (socks[j] == fds[i].fd)
	  break;
      if (j != 0)
	{
	  char *url0 = urls[0];
	  urls[0] = urls[j];
	  urls[j] = url0;
	}
    }
  for (i = j = 0; i < nurls; i++)
    if (socks[i] >= 0)
      close(socks[i]);
  free(socks);
  free(fds);
}

char *
findmetalinkurl(FILE *fp, unsigned char *chksump, Id *chksumtypep)
{
  char buf[4096], *bp, *ep;
  char **urls = 0;
  int nurls = 0;
  int i;

  if (chksumtypep)
    *chksumtypep = 0;
  while((bp = fgets(buf, sizeof(buf), fp)) != 0)
    {
      while (*bp == ' ' || *bp == '\t')
	bp++;
      if (chksumtypep && !*chksumtypep && !strncmp(bp, "<hash type=\"sha256\">", 20))
	{
	  int i;

	  bp += 20;
	  memset(chksump, 0, 32);
	  for (i = 0; i < 64; i++)
	    {
	      int c = *bp++;
	      if (c >= '0' && c <= '9')
		chksump[i / 2] = chksump[i / 2] * 16 + (c - '0');
	      else if (c >= 'a' && c <= 'f')
		chksump[i / 2] = chksump[i / 2] * 16 + (c - ('a' - 10));
	      else if (c >= 'A' && c <= 'F')
		chksump[i / 2] = chksump[i / 2] * 16 + (c - ('A' - 10));
	      else
		break;
	    }
	  if (i == 64)
	    *chksumtypep = REPOKEY_TYPE_SHA256;
	  continue;
	}
      if (strncmp(bp, "<url", 4))
	continue;
      bp = strchr(bp, '>');
      if (!bp)
	continue;
      bp++;
      ep = strstr(bp, "repodata/repomd.xml</url>");
      if (!ep)
	continue;
      *ep = 0;
      if (strncmp(bp, "http", 4))
	continue;
      urls = sat_extend(urls, nurls, 1, sizeof(*urls), 15);
      urls[nurls++] = strdup(bp);
    }
  if (nurls)
    {
      if (nurls > 1)
        findfastest(urls, nurls > 5 ? 5 : nurls);
      bp = urls[0];
      urls[0] = 0;
      for (i = 0; i < nurls; i++)
        sat_free(urls[i]);
      sat_free(urls);
      ep = strchr(bp, '/');
      if ((ep = strchr(ep + 2, '/')) != 0)
	{
	  *ep = 0;
	  printf("[using mirror %s]\n", bp);
	  *ep = '/';
	}
      return bp;
    }
  return 0;
}

char *
findmirrorlisturl(FILE *fp)
{
  char buf[4096], *bp, *ep;
  int i, l;
  char **urls = 0;
  int nurls = 0;

  while((bp = fgets(buf, sizeof(buf), fp)) != 0)
    {
      while (*bp == ' ' || *bp == '\t')
	bp++;
      if (!*bp || *bp == '#')
	continue;
      l = strlen(bp);
      while (l > 0 && (bp[l - 1] == ' ' || bp[l - 1] == '\t' || bp[l - 1] == '\n'))
	bp[--l] = 0;
      urls = sat_extend(urls, nurls, 1, sizeof(*urls), 15);
      urls[nurls++] = strdup(bp);
    }
  if (nurls)
    {
      if (nurls > 1)
        findfastest(urls, nurls > 5 ? 5 : nurls);
      bp = urls[0];
      urls[0] = 0;
      for (i = 0; i < nurls; i++)
        sat_free(urls[i]);
      sat_free(urls);
      ep = strchr(bp, '/');
      if ((ep = strchr(ep + 2, '/')) != 0)
	{
	  *ep = 0;
	  printf("[using mirror %s]\n", bp);
	  *ep = '/';
	}
      return bp;
    }
  return 0;
}

static ssize_t
cookie_gzread(void *cookie, char *buf, size_t nbytes)
{
  return gzread((gzFile *)cookie, buf, nbytes);
}

static int
cookie_gzclose(void *cookie)
{
  return gzclose((gzFile *)cookie);
}

FILE *
curlfopen(struct repoinfo *cinfo, const char *file, int uncompress, const unsigned char *chksum, Id chksumtype, int *badchecksump)
{
  pid_t pid;
  int fd, l;
  int status;
  char url[4096];
  const char *baseurl = cinfo->baseurl;

  if (!baseurl)
    {
      if (!cinfo->metalink && !cinfo->mirrorlist)
        return 0;
      if (file != cinfo->metalink && file != cinfo->mirrorlist)
	{
	  FILE *fp = curlfopen(cinfo, cinfo->metalink ? cinfo->metalink : cinfo->mirrorlist, 0, 0, 0, 0);
	  unsigned char mlchksum[32];
	  Id mlchksumtype = 0;
	  if (!fp)
	    return 0;
	  if (cinfo->metalink)
	    cinfo->baseurl = findmetalinkurl(fp, mlchksum, &mlchksumtype);
	  else
	    cinfo->baseurl = findmirrorlisturl(fp);
	  fclose(fp);
	  if (!cinfo->baseurl)
	    return 0;
#ifdef FEDORA
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
	  return curlfopen(cinfo, file, uncompress, chksum, chksumtype, badchecksump);
	}
      snprintf(url, sizeof(url), "%s", file);
    }
  else
    {
      l = strlen(baseurl);
      if (l && baseurl[l - 1] == '/')
	snprintf(url, sizeof(url), "%s%s", baseurl, file);
      else
	snprintf(url, sizeof(url), "%s/%s", baseurl, file);
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
      if (badchecksump)
	*badchecksump = 1;
      close(fd);
      return 0;
    }
  if (chksumtype && !verify_checksum(fd, file, chksum, chksumtype))
    {
      if (badchecksump)
	*badchecksump = 1;
      close(fd);
      return 0;
    }
  if (uncompress)
    {
      char tmpl[100];
      cookie_io_functions_t cio;
      gzFile *gzf;

      sprintf(tmpl, "/dev/fd/%d", fd);
      gzf = gzopen(tmpl, "r");
      close(fd);
      if (!gzf)
	{
	  fprintf(stderr, "could not open /dev/fd/%d, /proc not mounted?\n", fd);
	  exit(1);
	}
      memset(&cio, 0, sizeof(cio));
      cio.read = cookie_gzread;
      cio.close = cookie_gzclose;
      return fopencookie(gzf, "r", cio);
    }
  fcntl(fd, F_SETFD, FD_CLOEXEC);
  return fdopen(fd, "r");
}

static void
cleanupgpg(char *gpgdir)
{
  char cmd[256];
  snprintf(cmd, sizeof(cmd), "%s/pubring.gpg", gpgdir);
  unlink(cmd);
  snprintf(cmd, sizeof(cmd), "%s/pubring.gpg~", gpgdir);
  unlink(cmd);
  snprintf(cmd, sizeof(cmd), "%s/secring.gpg", gpgdir);
  unlink(cmd);
  snprintf(cmd, sizeof(cmd), "%s/trustdb.gpg", gpgdir);
  unlink(cmd);
  snprintf(cmd, sizeof(cmd), "%s/keys", gpgdir);
  unlink(cmd);
  rmdir(gpgdir);
}

int
checksig(Pool *sigpool, FILE *fp, FILE *sigfp)
{
  char *gpgdir;
  char *keysfile;
  const char *pubkey;
  char cmd[256];
  FILE *kfp;
  Solvable *s;
  Id p;
  off_t posfp, possigfp;
  int r, nkeys;

  gpgdir = mkdtemp(pool_tmpjoin(sigpool, "/var/tmp/solvgpg.XXXXXX", 0, 0));
  if (!gpgdir)
    return 0;
  keysfile = pool_tmpjoin(sigpool, gpgdir, "/keys", 0);
  if (!(kfp = fopen(keysfile, "w")) )
    {
      cleanupgpg(gpgdir);
      return 0;
    }
  nkeys = 0;
  for (p = 1, s = sigpool->solvables + p; p < sigpool->nsolvables; p++, s++)
    {
      if (!s->repo)
	continue;
      pubkey = solvable_lookup_str(s, SOLVABLE_DESCRIPTION);
      if (!pubkey || !*pubkey)
	continue;
      if (fwrite(pubkey, strlen(pubkey), 1, kfp) != 1)
	break;
      if (fputc('\n', kfp) == EOF)	/* Just in case... */
	break;
      nkeys++;
    }
  if (fclose(kfp) || !nkeys)
    {
      cleanupgpg(gpgdir);
      return 0;
    }
  snprintf(cmd, sizeof(cmd), "gpg2 -q --homedir %s --import %s", gpgdir, keysfile);
  if (system(cmd))
    {
      fprintf(stderr, "key import error\n");
      cleanupgpg(gpgdir);
      return 0;
    }
  unlink(keysfile);
  posfp = lseek(fileno(fp), 0, SEEK_CUR);
  lseek(fileno(fp), 0, SEEK_SET);
  possigfp = lseek(fileno(sigfp), 0, SEEK_CUR);
  lseek(fileno(sigfp), 0, SEEK_SET);
  snprintf(cmd, sizeof(cmd), "gpg -q --homedir %s --verify /dev/fd/%d /dev/fd/%d >/dev/null 2>&1", gpgdir, fileno(sigfp), fileno(fp));
  fcntl(fileno(fp), F_SETFD, 0);	/* clear CLOEXEC */
  fcntl(fileno(sigfp), F_SETFD, 0);	/* clear CLOEXEC */
  r = system(cmd);
  lseek(fileno(sigfp), possigfp, SEEK_SET);
  lseek(fileno(fp), posfp, SEEK_SET);
  fcntl(fileno(fp), F_SETFD, FD_CLOEXEC);
  fcntl(fileno(sigfp), F_SETFD, FD_CLOEXEC);
  cleanupgpg(gpgdir);
  return r == 0 ? 1 : 0;
}

#define CHKSUM_IDENT "1.1"

void
calc_checksum_fp(FILE *fp, Id chktype, unsigned char *out)
{
  char buf[4096];
  void *h = sat_chksum_create(chktype);
  int l;

  sat_chksum_add(h, CHKSUM_IDENT, strlen(CHKSUM_IDENT));
  while ((l = fread(buf, 1, sizeof(buf), fp)) > 0)
    sat_chksum_add(h, buf, l);
  rewind(fp);
  sat_chksum_free(h, out);
}

void
calc_checksum_stat(struct stat *stb, Id chktype, unsigned char *out)
{
  void *h = sat_chksum_create(chktype);
  sat_chksum_add(h, CHKSUM_IDENT, strlen(CHKSUM_IDENT));
  sat_chksum_add(h, &stb->st_dev, sizeof(stb->st_dev));
  sat_chksum_add(h, &stb->st_ino, sizeof(stb->st_ino));
  sat_chksum_add(h, &stb->st_size, sizeof(stb->st_size));
  sat_chksum_add(h, &stb->st_mtime, sizeof(stb->st_mtime));
  sat_chksum_free(h, out);
}

void
setarch(Pool *pool)
{
  struct utsname un;
  if (uname(&un))
    {
      perror("uname");
      exit(1);
    }
  pool_setarch(pool, un.machine);
}

char *calccachepath(Repo *repo, const char *repoext)
{
  char *q, *p = pool_tmpjoin(repo->pool, SOLVCACHE_PATH, "/", repo->name);
  if (repoext)
    {
      p = pool_tmpjoin(repo->pool, p, "_", repoext);
      p = pool_tmpjoin(repo->pool, p, ".solvx", 0);
    }
  else
    p = pool_tmpjoin(repo->pool, p, ".solv", 0);
  q = p + strlen(SOLVCACHE_PATH) + 1;
  if (*q == '.')
    *q = '_';
  for (; *q; q++)
    if (*q == '/')
      *q = '_';
  return p;
}

int
usecachedrepo(Repo *repo, const char *repoext, unsigned char *cookie, int mark)
{
  FILE *fp;
  unsigned char mycookie[32];
  unsigned char myextcookie[32];
  struct repoinfo *cinfo;
  int flags;

  cinfo = repo->appdata;
  if (!(fp = fopen(calccachepath(repo, repoext), "r")))
    return 0;
  if (fseek(fp, -sizeof(mycookie), SEEK_END) || fread(mycookie, sizeof(mycookie), 1, fp) != 1)
    {
      fclose(fp);
      return 0;
    }
  if (cookie && memcmp(cookie, mycookie, sizeof(mycookie)))
    {
      fclose(fp);
      return 0;
    }
  if (cinfo && !repoext)
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
    flags = REPO_USE_LOADING|REPO_EXTEND_SOLVABLES;
  if (repoext && strcmp(repoext, "DL") != 0)
    flags |= REPO_LOCALPOOL;	/* no local pool for DL so that we can compare IDs */

  if (repo_add_solv_flags(repo, fp, flags))
    {
      fclose(fp);
      return 0;
    }
  if (cinfo && !repoext)
    {
      memcpy(cinfo->cookie, mycookie, sizeof(mycookie));
      memcpy(cinfo->extcookie, myextcookie, sizeof(myextcookie));
    }
  if (mark)
    futimes(fileno(fp), 0);	/* try to set modification time */
  fclose(fp);
  return 1;
}

void
writecachedrepo(Repo *repo, Repodata *info, const char *repoext, unsigned char *cookie)
{
  FILE *fp;
  int i, fd;
  char *tmpl;
  struct repoinfo *cinfo;
  int onepiece;

  cinfo = repo->appdata;
  mkdir(SOLVCACHE_PATH, 0755);
  tmpl = sat_dupjoin(SOLVCACHE_PATH, "/", ".newsolv-XXXXXX");
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

  onepiece = 1;
  for (i = repo->start; i < repo->end; i++)
   if (repo->pool->solvables[i].repo != repo)
     break;
  if (i < repo->end)
    onepiece = 0;

  if (!info)
    repo_write(repo, fp, repo_write_stdkeyfilter, 0, 0);
  else if (repoext)
    repodata_write(info, fp, repo_write_stdkeyfilter, 0);
  else
    {
      int oldnrepodata = repo->nrepodata;
      repo->nrepodata = 1;	/* XXX: do this right */
      repo_write(repo, fp, repo_write_stdkeyfilter, 0, 0);
      repo->nrepodata = oldnrepodata;
      onepiece = 0;
    }

  if (!repoext && cinfo)
    {
      if (!cinfo->extcookie[0])
	{
	  /* create the ext cookie and append it */
	  /* we just need some unique ID */
	  struct stat stb;
	  if (!fstat(fileno(fp), &stb))
	    {
	      int i;

	      calc_checksum_stat(&stb, REPOKEY_TYPE_SHA256, cinfo->extcookie);
	      for (i = 0; i < 32; i++)
		cinfo->extcookie[i] ^= cookie[i];
	    }
	  if (cinfo->extcookie[0] == 0)
	    cinfo->extcookie[0] = 1;
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
  if (fwrite(cookie, 32, 1, fp) != 1)
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
  if (onepiece)
    {
      /* switch to just saved repo to activate paging and save memory */
      FILE *fp = fopen(tmpl, "r");
      if (fp)
	{
	  if (!repoext)
	    {
	      /* main repo */
	      repo_empty(repo, 1);
	      if (repo_add_solv_flags(repo, fp, SOLV_ADD_NO_STUBS))
		{
		  /* oops, no way to recover from here */
		  fprintf(stderr, "internal error\n");
		  exit(1);
		}
	    }
	  else
	    {
	      /* make sure repodata contains complete repo */
	      /* (this is how repodata_write saves it) */
	      repodata_extend_block(info, repo->start, repo->end - repo->start);
	      info->state = REPODATA_LOADING;
	      /* no need for LOCALPOOL as pool already contains ids */
	      repo_add_solv_flags(repo, fp, REPO_USE_LOADING|REPO_EXTEND_SOLVABLES);
	      info->state = REPODATA_AVAILABLE;	/* in case the load failed */
	    }
	  fclose(fp);
	}
    }
  if (!rename(tmpl, calccachepath(repo, repoext)))
    unlink(tmpl);
  free(tmpl);
}


static Pool *
read_sigs()
{
  Pool *sigpool = pool_create();
  Repo *repo = repo_create(sigpool, "rpmdbkeys");
  repo_add_rpmdb_pubkeys(repo, 0, 0);
  return sigpool;
}


/* repomd helpers */

static inline const char *
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

int
repomd_add_ext(Repo *repo, Repodata *data, const char *what)
{
  Pool *pool = repo->pool;
  Dataiterator di;
  Id chksumtype, handle;
  const unsigned char *chksum;
  const char *filename;

  dataiterator_init(&di, pool, repo, SOLVID_META, REPOSITORY_REPOMD_TYPE, what, SEARCH_STRING);
  dataiterator_prepend_keyname(&di, REPOSITORY_REPOMD);
  if (!dataiterator_step(&di))
    {
      dataiterator_free(&di);
      return 0;
    }
  if (!strcmp(what, "prestodelta"))
    what = "deltainfo";
  dataiterator_setpos_parent(&di);
  filename = pool_lookup_str(pool, SOLVID_POS, REPOSITORY_REPOMD_LOCATION);
  chksum = pool_lookup_bin_checksum(pool, SOLVID_POS, REPOSITORY_REPOMD_CHECKSUM, &chksumtype);
  if (!filename || !chksum)
    {
      dataiterator_free(&di);
      return 0;
    }
  handle = repodata_new_handle(data);
  repodata_set_poolstr(data, handle, REPOSITORY_REPOMD_TYPE, what);
  repodata_set_str(data, handle, REPOSITORY_REPOMD_LOCATION, filename);
  if (chksumtype)
    repodata_set_bin_checksum(data, handle, REPOSITORY_REPOMD_CHECKSUM, chksumtype, chksum);
  if (!strcmp(what, "deltainfo"))
    {
      repodata_add_idarray(data, handle, REPOSITORY_KEYS, REPOSITORY_DELTAINFO);
      repodata_add_idarray(data, handle, REPOSITORY_KEYS, REPOKEY_TYPE_FLEXARRAY);
    }
  if (!strcmp(what, "filelists"))
    {
      repodata_add_idarray(data, handle, REPOSITORY_KEYS, SOLVABLE_FILELIST);
      repodata_add_idarray(data, handle, REPOSITORY_KEYS, REPOKEY_TYPE_DIRSTRARRAY);
    }
  dataiterator_free(&di);
  repodata_add_flexarray(data, SOLVID_META, REPOSITORY_EXTERNAL, handle);
  return 1;
}


/* susetags helpers */

static inline const char *
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

static Id susetags_langtags[] = {
  SOLVABLE_SUMMARY, REPOKEY_TYPE_STR,
  SOLVABLE_DESCRIPTION, REPOKEY_TYPE_STR,
  SOLVABLE_EULA, REPOKEY_TYPE_STR,
  SOLVABLE_MESSAGEINS, REPOKEY_TYPE_STR,
  SOLVABLE_MESSAGEDEL, REPOKEY_TYPE_STR,
  SOLVABLE_CATEGORY, REPOKEY_TYPE_ID,
  0, 0
};

void
susetags_add_ext(Repo *repo, Repodata *data)
{
  Pool *pool = repo->pool;
  Dataiterator di;
  char ext[3];
  Id handle, filechksumtype;
  const unsigned char *filechksum;
  int i;

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
      if (!strcmp(ext, "DU"))
	{
	  repodata_add_idarray(data, handle, REPOSITORY_KEYS, SOLVABLE_DISKUSAGE);
	  repodata_add_idarray(data, handle, REPOSITORY_KEYS, REPOKEY_TYPE_DIRNUMNUMARRAY);
	}
      else if (!strcmp(ext, "FL"))
	{
	  repodata_add_idarray(data, handle, REPOSITORY_KEYS, SOLVABLE_FILELIST);
	  repodata_add_idarray(data, handle, REPOSITORY_KEYS, REPOKEY_TYPE_DIRSTRARRAY);
	}
      else
	{
	  for (i = 0; susetags_langtags[i]; i += 2)
	    {
	      repodata_add_idarray(data, handle, REPOSITORY_KEYS, pool_id2langid(pool, susetags_langtags[i], ext, 1));
	      repodata_add_idarray(data, handle, REPOSITORY_KEYS, susetags_langtags[i + 1]);
	    }
	}
      repodata_add_flexarray(data, SOLVID_META, REPOSITORY_EXTERNAL, handle);
    }
  dataiterator_free(&di);
}


static inline int
iscompressed(const char *name)
{
  int l = strlen(name);
  return l > 3 && !strcmp(name + l - 3, ".gz") ? 1 : 0;
}


/* load callback */

int
load_stub(Pool *pool, Repodata *data, void *dp)
{
  const char *filename, *descrdir, *repomdtype;
  const unsigned char *filechksum;
  Id filechksumtype;
  struct repoinfo *cinfo;
  FILE *fp;
  Id defvendor;
  char ext[3];

  cinfo = data->repo->appdata;

  filename = repodata_lookup_str(data, SOLVID_META, SUSETAGS_FILE_NAME);
  if (filename)
    {
      /* susetags load */
      ext[0] = filename[9];
      ext[1] = filename[10];
      ext[2] = 0;
#if 1
      printf("[%s:%s", data->repo->name, ext);
#endif
      if (usecachedrepo(data->repo, ext, cinfo->extcookie, 0))
	{
          printf(" cached]\n"); fflush(stdout);
	  return 1;
	}
#if 1
      printf(" fetching]\n"); fflush(stdout);
#endif
      defvendor = repo_lookup_id(data->repo, SOLVID_META, SUSETAGS_DEFAULTVENDOR);
      descrdir = repo_lookup_str(data->repo, SOLVID_META, SUSETAGS_DESCRDIR);
      if (!descrdir)
	descrdir = "suse/setup/descr";
      filechksumtype = 0;
      filechksum = repodata_lookup_bin_checksum(data, SOLVID_META, SUSETAGS_FILE_CHECKSUM, &filechksumtype);
      if ((fp = curlfopen(cinfo, pool_tmpjoin(pool, descrdir, "/", filename), iscompressed(filename), filechksum, filechksumtype, 0)) == 0)
	return 0;
      repo_add_susetags(data->repo, fp, defvendor, ext, REPO_USE_LOADING|REPO_EXTEND_SOLVABLES);
      fclose(fp);
      writecachedrepo(data->repo, data, ext, cinfo->extcookie);
      return 1;
    }

  repomdtype = repodata_lookup_str(data, SOLVID_META, REPOSITORY_REPOMD_TYPE);
  if (repomdtype)
    {
      if (!strcmp(repomdtype, "filelists"))
	strcpy(ext, "FL");
      else if (!strcmp(repomdtype, "deltainfo"))
	strcpy(ext, "DL");
      else
	return 0;
#if 1
      printf("[%s:%s", data->repo->name, ext);
#endif
      if (usecachedrepo(data->repo, ext, cinfo->extcookie, 0))
	{
	  printf(" cached]\n");fflush(stdout);
	  return 1;
	}
      printf(" fetching]\n"); fflush(stdout);
      filename = repodata_lookup_str(data, SOLVID_META, REPOSITORY_REPOMD_LOCATION);
      filechksumtype = 0;
      filechksum = repodata_lookup_bin_checksum(data, SOLVID_META, REPOSITORY_REPOMD_CHECKSUM, &filechksumtype);
      if ((fp = curlfopen(cinfo, filename, iscompressed(filename), filechksum, filechksumtype, 0)) == 0)
	return 0;
      if (!strcmp(ext, "FL"))
        repo_add_rpmmd(data->repo, fp, ext, REPO_USE_LOADING|REPO_EXTEND_SOLVABLES);
      else if (!strcmp(ext, "DL"))
        repo_add_deltainfoxml(data->repo, fp, REPO_USE_LOADING);
      fclose(fp);
      writecachedrepo(data->repo, data, ext, cinfo->extcookie);
      return 1;
    }

  return 0;
}

static unsigned char installedcookie[32];

void
read_repos(Pool *pool, struct repoinfo *repoinfos, int nrepoinfos)
{
  Repo *repo;
  struct repoinfo *cinfo;
  int i;
  FILE *fp;
  FILE *sigfp;
  const char *filename;
  const unsigned char *filechksum;
  Id filechksumtype;
  const char *descrdir;
  int defvendor;
  struct stat stb;
  Pool *sigpool = 0;
  Repodata *data;
  int badchecksum;
  int dorefresh;

  repo = repo_create(pool, "@System");
  printf("rpm database:");
  if (stat("/var/lib/rpm/Packages", &stb))
    memset(&stb, 0, sizeof(&stb));
  calc_checksum_stat(&stb, REPOKEY_TYPE_SHA256, installedcookie);
  if (usecachedrepo(repo, 0, installedcookie, 0))
    printf(" cached\n");
  else
    {
      FILE *ofp;
      printf(" reading\n");
      int done = 0;

#ifdef PRODUCTS_PATH
      repo_add_products(repo, PRODUCTS_PATH, 0, REPO_NO_INTERNALIZE);
#endif
      if ((ofp = fopen(calccachepath(repo, 0), "r")) != 0)
	{
	  Repo *ref = repo_create(pool, "@System.old");
	  if (!repo_add_solv(ref, ofp))
	    {
	      repo_add_rpmdb(repo, ref, 0, REPO_REUSE_REPODATA);
	      done = 1;
	    }
	  fclose(ofp);
	  repo_free(ref, 1);
	}
      if (!done)
        repo_add_rpmdb(repo, 0, 0, REPO_REUSE_REPODATA);
      writecachedrepo(repo, 0, 0, installedcookie);
    }
  pool_set_installed(pool, repo);

  for (i = 0; i < nrepoinfos; i++)
    {
      cinfo = repoinfos + i;
      if (!cinfo->enabled)
	continue;

      repo = repo_create(pool, cinfo->alias);
      cinfo->repo = repo;
      repo->appdata = cinfo;
      repo->priority = 99 - cinfo->priority;

      dorefresh = cinfo->autorefresh;
      if (dorefresh && cinfo->metadata_expire && stat(calccachepath(repo, 0), &stb) == 0)
	{
	  if (cinfo->metadata_expire == -1 || time(0) - stb.st_mtime < cinfo->metadata_expire)
	    dorefresh = 0;
	}
      if (!dorefresh && usecachedrepo(repo, 0, 0, 0))
	{
	  printf("repo '%s':", cinfo->alias);
	  printf(" cached\n");
	  continue;
	}
      badchecksum = 0;
      switch (cinfo->type)
	{
        case TYPE_RPMMD:
	  printf("rpmmd repo '%s':", cinfo->alias);
	  fflush(stdout);
	  if ((fp = curlfopen(cinfo, "repodata/repomd.xml", 0, 0, 0, 0)) == 0)
	    {
	      printf(" no repomd.xml file, skipped\n");
	      repo_free(repo, 1);
	      cinfo->repo = 0;
	      break;
	    }
	  calc_checksum_fp(fp, REPOKEY_TYPE_SHA256, cinfo->cookie);
	  if (usecachedrepo(repo, 0, cinfo->cookie, 1))
	    {
	      printf(" cached\n");
              fclose(fp);
	      break;
	    }
	  if (cinfo->repo_gpgcheck)
	    {
	      sigfp = curlfopen(cinfo, "repodata/repomd.xml.asc", 0, 0, 0, 0);
	      if (!sigfp)
		{
		  printf(" unsigned, skipped\n");
		  fclose(fp);
		  break;
		}
	      if (!sigpool)
		sigpool = read_sigs();
	      if (!checksig(sigpool, fp, sigfp))
		{
		  printf(" checksig failed, skipped\n");
		  fclose(sigfp);
		  fclose(fp);
		  break;
		}
	      fclose(sigfp);
	    }
	  repo_add_repomdxml(repo, fp, 0);
	  fclose(fp);
	  printf(" fetching\n");
	  filename = repomd_find(repo, "primary", &filechksum, &filechksumtype);
	  if (filename && (fp = curlfopen(cinfo, filename, iscompressed(filename), filechksum, filechksumtype, &badchecksum)) != 0)
	    {
	      repo_add_rpmmd(repo, fp, 0, 0);
	      fclose(fp);
	    }
	  if (badchecksum)
	    break;	/* hopeless */

	  filename = repomd_find(repo, "updateinfo", &filechksum, &filechksumtype);
	  if (filename && (fp = curlfopen(cinfo, filename, iscompressed(filename), filechksum, filechksumtype, &badchecksum)) != 0)
	    {
	      repo_add_updateinfoxml(repo, fp, 0);
	      fclose(fp);
	    }

	  data = repo_add_repodata(repo, 0);
	  if (!repomd_add_ext(repo, data, "deltainfo"))
	    repomd_add_ext(repo, data, "prestodelta");
	  repomd_add_ext(repo, data, "filelists");
	  repodata_internalize(data);
	  if (!badchecksum)
	    writecachedrepo(repo, 0, 0, cinfo->cookie);
	  repodata_create_stubs(repo_last_repodata(repo));
	  break;

        case TYPE_SUSETAGS:
	  printf("susetags repo '%s':", cinfo->alias);
	  fflush(stdout);
	  descrdir = 0;
	  defvendor = 0;
	  if ((fp = curlfopen(cinfo, "content", 0, 0, 0, 0)) == 0)
	    {
	      printf(" no content file, skipped\n");
	      repo_free(repo, 1);
	      cinfo->repo = 0;
	      break;
	    }
	  calc_checksum_fp(fp, REPOKEY_TYPE_SHA256, cinfo->cookie);
	  if (usecachedrepo(repo, 0, cinfo->cookie, 1))
	    {
	      printf(" cached\n");
	      fclose(fp);
	      break;
	    }
	  if (cinfo->repo_gpgcheck)
	    {
	      sigfp = curlfopen(cinfo, "content.asc", 0, 0, 0, 0);
	      if (!sigfp)
		{
		  printf(" unsigned, skipped\n");
		  fclose(fp);
		  break;
		}
	      if (sigfp)
		{
		  if (!sigpool)
		    sigpool = read_sigs();
		  if (!checksig(sigpool, fp, sigfp))
		    {
		      printf(" checksig failed, skipped\n");
		      fclose(sigfp);
		      fclose(fp);
		      break;
		    }
		  fclose(sigfp);
		}
	    }
	  repo_add_content(repo, fp, 0);
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
	      break;
	    }
	  printf(" fetching\n");
	  if ((fp = curlfopen(cinfo, pool_tmpjoin(pool, descrdir, "/", filename), iscompressed(filename), filechksum, filechksumtype, &badchecksum)) == 0)
	    break;	/* hopeless */
	  repo_add_susetags(repo, fp, defvendor, 0, REPO_NO_INTERNALIZE);
	  fclose(fp);
	  /* add default language */
	  filename = susetags_find(repo, "packages.en.gz", &filechksum, &filechksumtype);
          if (!filename)
	    filename = susetags_find(repo, "packages.en", &filechksum, &filechksumtype);
	  if (filename)
	    {
	      if ((fp = curlfopen(cinfo, pool_tmpjoin(pool, descrdir, "/", filename), iscompressed(filename), filechksum, filechksumtype, &badchecksum)) != 0)
		{
		  repo_add_susetags(repo, fp, defvendor, 0, REPO_NO_INTERNALIZE|REPO_REUSE_REPODATA|REPO_EXTEND_SOLVABLES);
		  fclose(fp);
		}
	    }
          repo_internalize(repo);
	  data = repo_add_repodata(repo, 0);
	  susetags_add_ext(repo, data);
	  repodata_internalize(data);
	  if (!badchecksum)
	    writecachedrepo(repo, 0, 0, cinfo->cookie);
	  repodata_create_stubs(repo_last_repodata(repo));
	  break;
	default:
	  printf("unsupported repo '%s': skipped\n", cinfo->alias);
	  repo_free(repo, 1);
	  cinfo->repo = 0;
	  break;
	}
    }
  if (sigpool)
    pool_free(sigpool);
}


int
str2archid(Pool *pool, char *arch)
{
  Id id;
  if (!*arch)
    return 0;
  id = str2id(pool, arch, 0);
  if (id == ARCH_SRC || id == ARCH_NOSRC || id == ARCH_NOARCH)
    return id;
  if (pool->id2arch && (id > pool->lastarch || !pool->id2arch[id]))
    return 0;
  return id;
}

int
depglob(Pool *pool, char *name, Queue *job)
{
  Id p, pp;
  Id id = str2id(pool, name, 0);
  int i, match = 0;

  if (id)
    {
      FOR_PROVIDES(p, pp, id)
	{
	  Solvable *s = pool->solvables + p;
	  match = 1;
	  if (s->name == id)
	    {
	      queue_push2(job, SOLVER_SOLVABLE_NAME, id);
	      return 1;
	    }
	}
      if (match)
	{
	  printf("[using capability match for '%s']\n", name);
	  queue_push2(job, SOLVER_SOLVABLE_PROVIDES, id);
	  return 1;
	}
    }

  if (strpbrk(name, "[*?") == 0)
    return 0;

  /* looks like a name glob. hard work. */
  for (p = 1; p < pool->nsolvables; p++)
    {
      Solvable *s = pool->solvables + p;
      if (!s->repo || !pool_installable(pool, s))
	continue;
      id = s->name;
      if (fnmatch(name, id2str(pool, id), 0) == 0)
	{
	  for (i = 0; i < job->count; i += 2)
	    if (job->elements[i] == SOLVER_SOLVABLE_NAME && job->elements[i + 1] == id)
	      break;
	  if (i == job->count)
	    queue_push2(job, SOLVER_SOLVABLE_NAME, id);
	  match = 1;
	}
    }
  if (match)
    return 1;
  /* looks like a dep glob. really hard work. */
  for (id = 1; id < pool->ss.nstrings; id++)
    {
      if (!pool->whatprovides[id])
	continue;
      if (fnmatch(name, id2str(pool, id), 0) == 0)
	{
	  if (!match)
	    printf("[using capability match for '%s']\n", name);
	  for (i = 0; i < job->count; i += 2)
	    if (job->elements[i] == SOLVER_SOLVABLE_PROVIDES && job->elements[i + 1] == id)
	      break;
	  if (i == job->count)
	    queue_push2(job, SOLVER_SOLVABLE_PROVIDES, id);
	  match = 1;
	}
    }
  if (match)
    return 1;
  return 0;
}

void
addrelation(Pool *pool, Queue *job, int flags, Id evr)
{
  int i;
  for (i = 0; i < job->count; i += 2)
    {
      if (job->elements[i] != SOLVER_SOLVABLE_NAME && job->elements[i] != SOLVER_SOLVABLE_PROVIDES)
	continue;
      job->elements[i + 1] = rel2id(pool, job->elements[i + 1], evr, flags, 1);
    }
}

int
limitevr(Pool *pool, char *evr, Queue *job, Id archid)
{
  Queue mq;
  Id p, pp, evrid;
  int matched = 0;
  int i, j;
  Solvable *s;
  const char *sevr;

  queue_init(&mq);
  for (i = 0; i < job->count; i += 2)
    {
      queue_empty(&mq);
      FOR_JOB_SELECT(p, pp, job->elements[i], job->elements[i + 1])
	{
	  s = pool_id2solvable(pool, p);
	  if (archid && s->arch != archid)
	    continue;
          sevr = id2str(pool, s->evr);
          if (!strchr(evr, ':') && strchr(sevr, ':'))
	    sevr = strchr(sevr, ':') + 1;
	  if (evrcmp_str(pool, sevr, evr, EVRCMP_MATCH) == 0)
	     queue_push(&mq, p);
	}
      if (mq.count)
	{
	  if (!matched && i)
	    {
	      queue_deleten(job, 0, i);
	      i = 0;
	    }
	  matched = 1;
	  /* if all solvables have the same evr */
	  s = pool_id2solvable(pool, mq.elements[0]);
	  evrid = s->evr;
	  for (j = 0; j < mq.count; j++)
	    {
	      s = pool_id2solvable(pool, mq.elements[j]);
	      if (s->evr != evrid)
		break;
	    }
	  if (j == mq.count && j > 1)
	    {
	      prune_to_best_arch(pool, &mq);
	      // prune_to_highest_prio(pool, &mq);
	      mq.count = 1;
	    }
	  if (mq.count > 1)
	    {
	      job->elements[i] = SOLVER_SOLVABLE_ONE_OF;
	      job->elements[i + 1] = pool_queuetowhatprovides(pool, &mq);
	    }
	  else
	    {
	      job->elements[i] = SOLVER_SOLVABLE;
	      job->elements[i + 1] = mq.elements[0];
	    }
	}
      else if (matched)
	{
	  queue_deleten(job, i, 2);
	  i -= 2;
	}
    }
  queue_free(&mq);
  if (matched)
    return 1;
  if (!archid)
    {
      char *r;
      if ((r = strrchr(evr, '.')) != 0 && r[1] && (archid = str2archid(pool, r + 1)) != 0)
	{
	  *r = 0;
	  if (limitevr(pool, evr, job, archid))
	    {
	      *r = '.';
	      return 1;
	    }
	  *r = '.';
	}
    }
  return 0;
}

int
limitrepo(Pool *pool, Id repofilter, Queue *job)
{
  Queue mq;
  Id p, pp;
  int matched = 0;
  int i;
  Solvable *s;

  queue_init(&mq);
  for (i = 0; i < job->count; i += 2)
    {
      queue_empty(&mq);
      FOR_JOB_SELECT(p, pp, job->elements[i], job->elements[i + 1])
	{
	  s = pool_id2solvable(pool, p);
	  if (s->repo && s->repo->repoid == repofilter)
	     queue_push(&mq, p);
	}
      if (mq.count)
	{
	  if (!matched && i)
	    {
	      queue_deleten(job, 0, i);
	      i = 0;
	    }
	  matched = 1;
	  if (mq.count > 1)
	    {
	      job->elements[i] = SOLVER_SOLVABLE_ONE_OF;
	      job->elements[i + 1] = pool_queuetowhatprovides(pool, &mq);
	    }
	  else
	    {
	      job->elements[i] = SOLVER_SOLVABLE;
	      job->elements[i + 1] = mq.elements[0];
	    }
	}
      else if (matched)
	{
	  queue_deleten(job, i, 2);
	  i -= 2;
	}
    }
  queue_free(&mq);
  return matched;
}

void
mkselect(Pool *pool, int mode, char *name, Queue *job)
{
  char *r, *r2;
  Id archid;

  if (*name == '/')
    {
      Dataiterator di;
      Queue q;
      int match = 0;

      queue_init(&q);
      dataiterator_init(&di, pool, mode == SOLVER_ERASE ? pool->installed : 0, 0, SOLVABLE_FILELIST, name, SEARCH_STRING|SEARCH_FILES|SEARCH_COMPLETE_FILELIST);
      while (dataiterator_step(&di))
	{
	  Solvable *s = pool->solvables + di.solvid;
	  if (!s->repo || !pool_installable(pool, s))
	    continue;
	  queue_push(&q, di.solvid);
	  dataiterator_skip_solvable(&di);
	}
      dataiterator_free(&di);
      if (q.count)
	{
	  printf("[using file list match for '%s']\n", name);
	  match = 1;
	  if (q.count > 1)
	    queue_push2(job, SOLVER_SOLVABLE_ONE_OF, pool_queuetowhatprovides(pool, &q));
	  else
	    queue_push2(job, SOLVER_SOLVABLE, q.elements[0]);
	}
      queue_free(&q);
      if (match)
	return;
    }
  if ((r = strpbrk(name, "<=>")) != 0)
    {
      /* relation case, support:
       * depglob rel
       * depglob.rpm rel
       */
      int rflags = 0;
      int nend = r - name;
      for (; *r; r++)
	{
	  if (*r == '<')
	    rflags |= REL_LT;
	  else if (*r == '=')
	    rflags |= REL_EQ;
	  else if (*r == '>')
	    rflags |= REL_GT;
	  else
	    break;
	}
      while (*r && *r == ' ' && *r == '\t')
	r++;
      while (nend && (name[nend - 1] == ' ' || name[nend -1 ] == '\t'))
	nend--;
      name[nend] = 0;
      if (!*name || !*r)
	{
	  fprintf(stderr, "bad relation\n");
	  exit(1);
	}
      if (depglob(pool, name, job))
	{
	  addrelation(pool, job, rflags, str2id(pool, r, 1));
	  return;
	}
      if ((r2 = strrchr(name, '.')) != 0 && r2[1] && (archid = str2archid(pool, r2 + 1)) != 0)
	{
	  *r2 = 0;
	  if (depglob(pool, name, job))
	    {
	      *r2 = '.';
	      addrelation(pool, job, REL_ARCH, archid);
	      addrelation(pool, job, rflags, str2id(pool, r, 1));
	      return;
	    }
	  *r2 = '.';
	}
    }
  else
    {
      /* no relation case, support:
       * depglob
       * depglob.arch
       * depglob-version-release
       * depglob-version-release.arch
       */
      if (depglob(pool, name, job))
	return;
      archid = 0;
      if ((r = strrchr(name, '.')) != 0 && r[1] && (archid = str2archid(pool, r + 1)) != 0)
	{
	  *r = 0;
	  if (depglob(pool, name, job))
	    {
	      *r = '.';
	      addrelation(pool, job, REL_ARCH, archid);
	      return;
	    }
	  *r = '.';
	}
      if ((r = strrchr(name, '-')) != 0)
	{
	  *r = 0;
	  if (depglob(pool, name, job))
	    {
	      /* have just the version */
	      *r = '-';
	      if (limitevr(pool, r + 1, job, 0))
	        return;
	    }
	  if ((r2 = strrchr(name, '-')) != 0)
	    {
	      *r = '-';
	      *r2 = 0;
	      if (depglob(pool, name, job))
		{
		  *r2 = '-';
		  if (limitevr(pool, r2 + 1, job, 0))
		    return;
		}
	      *r2 = '-';
	    }
	  *r = '-';
	}
    }
  fprintf(stderr, "nothing matches '%s'\n", name);
  exit(1);
}


int
yesno(const char *str)
{
  char inbuf[128], *ip;

  for (;;)
    {
      printf("%s", str);
      fflush(stdout);
      *inbuf = 0;
      if (!(ip = fgets(inbuf, sizeof(inbuf), stdin)))
	{
	  printf("Abort.\n");
	  exit(1);
	}
      while (*ip == ' ' || *ip == '\t')
	ip++;
      if (*ip == 'q')
	{
	  printf("Abort.\n");
	  exit(1);
	}
      if (*ip == 'y' || *ip == 'n')
	return *ip == 'y' ? 1 : 0;
    }
}

struct fcstate {
  FILE **newpkgsfps;
  Queue *checkq;
  int newpkgscnt;
  void *rpmdbstate;
};

static void *
fileconflict_cb(Pool *pool, Id p, void *cbdata)
{
  struct fcstate *fcstate = cbdata;
  Solvable *s;
  Id rpmdbid;
  int i;
  FILE *fp;

  if (!p)
    {
      rpm_byrpmdbid(0, 0, &fcstate->rpmdbstate);
      return 0;
    }
  s = pool_id2solvable(pool, p);
  if (pool->installed && s->repo == pool->installed)
    {
      if (!s->repo->rpmdbid)
	return 0;
      rpmdbid = s->repo->rpmdbid[p - s->repo->start];
      if (!rpmdbid)
	return 0;
       return rpm_byrpmdbid(rpmdbid, 0, &fcstate->rpmdbstate);
    }
  for (i = 0; i < fcstate->newpkgscnt; i++)
    if (fcstate->checkq->elements[i] == p)
      break;
  if (i == fcstate->newpkgscnt)
    return 0;
  fp = fcstate->newpkgsfps[i];
  if (!fp)
    return 0;
  rewind(fp);
  return rpm_byfp(fp, solvable2str(pool, s), &fcstate->rpmdbstate);
}

void
runrpm(const char *arg, const char *name, int dupfd3)
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
      if (dupfd3 != -1 && dupfd3 != 3)
	{
	  dup2(dupfd3, 3);
	  close(dupfd3);
	}
      if (dupfd3 != -1)
	fcntl(3, F_SETFD, 0);	/* clear CLOEXEC */
      if (strcmp(arg, "-e") == 0)
        execlp("rpm", "rpm", arg, "--nodeps", "--nodigest", "--nosignature", name, (char *)0);
      else
        execlp("rpm", "rpm", arg, "--force", "--nodeps", "--nodigest", "--nosignature", name, (char *)0);
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

static Id
nscallback(Pool *pool, void *data, Id name, Id evr)
{
  if (name == NAMESPACE_PRODUCTBUDDY)
    {    
      /* SUSE specific hack: each product has an associated rpm */
      Solvable *s = pool->solvables + evr; 
      Id p, pp, cap; 
      
      cap = str2id(pool, pool_tmpjoin(pool, "product(", id2str(pool, s->name) + 8, ")"), 0);
      if (!cap)
        return 0;
      cap = rel2id(pool, cap, s->evr, REL_EQ, 0);
      if (!cap)
        return 0;
      FOR_PROVIDES(p, pp, cap) 
        {
          Solvable *ps = pool->solvables + p; 
          if (ps->repo == s->repo && ps->arch == s->arch)
            break;
        }
      return p;
    }
  return 0;
}

#ifdef SOFTLOCKS_PATH

void
addsoftlocks(Pool *pool, Queue *job)
{
  FILE *fp;
  Id type, id, p, pp;
  char *bp, *ep, buf[4096];

  if ((fp = fopen(SOFTLOCKS_PATH, "r")) == 0)
    return;
  while((bp = fgets(buf, sizeof(buf), fp)) != 0)
    {
      while (*bp == ' ' || *bp == '\t')
	bp++;
      if (!*bp || *bp == '#')
	continue;
      for (ep = bp; *ep; ep++)
	if (*ep == ' ' || *ep == '\t' || *ep == '\n')
	  break;
      *ep = 0;
      type = SOLVER_SOLVABLE_NAME;
      if (!strncmp(bp, "provides:", 9) && bp[9])
	{
	  type = SOLVER_SOLVABLE_PROVIDES;
	  bp += 9;
	}
      id = str2id(pool, bp, 1);
      if (pool->installed)
	{
	  FOR_JOB_SELECT(p, pp, type, id)
	    if (pool->solvables[p].repo == pool->installed)
	      break;
	  if (p)
	    continue;	/* ignore, as it is already installed */
	}
      queue_push2(job, SOLVER_LOCK|SOLVER_WEAK|type, id);
    }
  fclose(fp);
}

#endif


void
rewrite_repos(Pool *pool, Id *addedfileprovides)
{
  Repo *repo;
  Repodata *data;
  Map providedids;
  Queue fileprovidesq;
  Id id;
  int i, j, n, nprovidedids;
  struct repoinfo *cinfo;

  map_init(&providedids, pool->ss.nstrings);
  queue_init(&fileprovidesq);
  for (nprovidedids = 0; (id = addedfileprovides[nprovidedids]) != 0; nprovidedids++)
    MAPSET(&providedids, id);
  FOR_REPOS(i, repo)
    {
      /* make sure this repo has just one main repodata */
      if (!repo->nrepodata)
	continue;
      cinfo = repo->appdata;
      data = repo->repodata + 0;
      if (data->store.pagefd == -1)
	continue;
      if (repodata_lookup_idarray(data, SOLVID_META, REPOSITORY_ADDEDFILEPROVIDES, &fileprovidesq))
	{
	  n = 0;
	  for (j = 0; j < fileprovidesq.count; j++)
	    if (MAPTST(&providedids, fileprovidesq.elements[j]))
	      n++;
	  if (n == nprovidedids)
	    continue;	/* nothing new added */
	}
      /* oh my! */
      for (j = 0; addedfileprovides[j]; j++)
	repodata_add_idarray(data, SOLVID_META, REPOSITORY_ADDEDFILEPROVIDES, addedfileprovides[j]);
      repodata_internalize(data);
      writecachedrepo(repo, data, 0, cinfo ? cinfo->cookie : installedcookie);
    }
  queue_free(&fileprovidesq);
  map_free(&providedids);
}

#define MODE_LIST        0
#define MODE_INSTALL     1
#define MODE_ERASE       2
#define MODE_UPDATE      3
#define MODE_DISTUPGRADE 4
#define MODE_VERIFY      5
#define MODE_PATCH       6
#define MODE_INFO        7
#define MODE_REPOLIST    8
#define MODE_SEARCH	 9

void
usage(int r)
{
  fprintf(stderr, "Usage: solv COMMAND <select>\n");
  fprintf(stderr, "\n");
  fprintf(stderr, "    dist-upgrade: replace installed packages with\n");
  fprintf(stderr, "                  versions from the repositories\n");
  fprintf(stderr, "    erase:        erase installed packages\n");
  fprintf(stderr, "    info:         display package information\n");
  fprintf(stderr, "    install:      install packages\n");
  fprintf(stderr, "    list:         list packages\n");
  fprintf(stderr, "    repos:        list enabled repositories\n");
  fprintf(stderr, "    search:       search name/summary/description\n");
  fprintf(stderr, "    update:       update installed packages\n");
  fprintf(stderr, "    verify:       check dependencies of installed packages\n");
  fprintf(stderr, "\n");
  exit(r);
}

int
main(int argc, char **argv)
{
  Pool *pool;
  Repo *commandlinerepo = 0;
  Id *commandlinepkgs = 0;
  Id p, pp;
  struct repoinfo *repoinfos;
  int nrepoinfos = 0;
  int mainmode = 0, mode = 0;
  int i, newpkgs;
  Queue job, checkq;
  Solver *solv = 0;
  Transaction *trans;
  char inbuf[128], *ip;
  int allpkgs = 0;
  FILE **newpkgsfps;
  struct fcstate fcstate;
  Id *addedfileprovides = 0;
  Id repofilter = 0;

  argc--;
  argv++;
  if (!argv[0])
    usage(1);
  if (!strcmp(argv[0], "install") || !strcmp(argv[0], "in"))
    {
      mainmode = MODE_INSTALL;
      mode = SOLVER_INSTALL;
    }
  else if (!strcmp(argv[0], "patch"))
    {
      mainmode = MODE_PATCH;
      mode = SOLVER_INSTALL;
    }
  else if (!strcmp(argv[0], "erase") || !strcmp(argv[0], "rm"))
    {
      mainmode = MODE_ERASE;
      mode = SOLVER_ERASE;
    }
  else if (!strcmp(argv[0], "list"))
    {
      mainmode = MODE_LIST;
      mode = 0;
    }
  else if (!strcmp(argv[0], "info"))
    {
      mainmode = MODE_INFO;
      mode = 0;
    }
  else if (!strcmp(argv[0], "search"))
    {
      mainmode = MODE_SEARCH;
      mode = 0;
    }
  else if (!strcmp(argv[0], "verify"))
    {
      mainmode = MODE_VERIFY;
      mode = SOLVER_VERIFY;
    }
  else if (!strcmp(argv[0], "update") || !strcmp(argv[0], "up"))
    {
      mainmode = MODE_UPDATE;
      mode = SOLVER_UPDATE;
    }
  else if (!strcmp(argv[0], "dist-upgrade") || !strcmp(argv[0], "dup"))
    {
      mainmode = MODE_DISTUPGRADE;
      mode = SOLVER_UPDATE;
    }
  else if (!strcmp(argv[0], "repos") || !strcmp(argv[0], "repolist") || !strcmp(argv[0], "lr"))
    {
      mainmode = MODE_REPOLIST;
      mode = 0;
    }
  else
    usage(1);

  pool = pool_create();
#ifdef FEDORA
  pool->obsoleteusescolors = 1;
#endif
  pool_setloadcallback(pool, load_stub, 0);
  pool->nscallback = nscallback;
  // pool_setdebuglevel(pool, 2);
  setarch(pool);
  repoinfos = read_repoinfos(pool, REPOINFO_PATH, &nrepoinfos);

  if (mainmode == MODE_REPOLIST)
    {
      int j = 1;
      for (i = 0; i < nrepoinfos; i++)
	{
	  struct repoinfo *cinfo = repoinfos + i;
	  if (!cinfo->enabled)
	    continue;
	  printf("%d: %-20s %s (prio %d)\n", j++, cinfo->alias, cinfo->name, cinfo->priority);
	}
      exit(0);
    }

  read_repos(pool, repoinfos, nrepoinfos);

  if (argc > 2 && !strcmp(argv[1], "-r"))
    {
      const char *rname = argv[2], *rp;
      for (rp = rname; *rp; rp++)
	if (*rp <= '0' || *rp >= '9')
	  break;
      if (!*rp)
	{
	  /* repo specified by number */
	  int rnum = atoi(rname);
	  for (i = 0; i < nrepoinfos; i++)
	    {
	      struct repoinfo *cinfo = repoinfos + i;
	      if (!cinfo->enabled)
		continue;
	      if (--rnum == 0)
	        repofilter = cinfo->repo->repoid;
	    }
	}
      else
	{
	  /* repo specified by alias */
	  Repo *repo;
	  FOR_REPOS(i, repo)
	    {
	      if (!strcasecmp(rname, repo->name))
		repofilter = repo->repoid;
	    }
	}
      if (!repofilter)
	{
	  fprintf(stderr, "%s: no such repo\n", rname);
	  exit(1);
	}
      argc -= 2;
      argv += 2;
    }
  if (mainmode == MODE_SEARCH)
    {
      Dataiterator di;
      Map m;
      if (argc != 2)
	usage(1);
      map_init(&m, pool->nsolvables);
      dataiterator_init(&di, pool, 0, 0, 0, argv[1], SEARCH_SUBSTRING|SEARCH_NOCASE);
      dataiterator_set_keyname(&di, SOLVABLE_NAME);
      dataiterator_set_search(&di, 0, 0);
      while (dataiterator_step(&di))
	MAPSET(&m, di.solvid);
      dataiterator_set_keyname(&di, SOLVABLE_SUMMARY);
      dataiterator_set_search(&di, 0, 0);
      while (dataiterator_step(&di))
	MAPSET(&m, di.solvid);
      dataiterator_set_keyname(&di, SOLVABLE_DESCRIPTION);
      dataiterator_set_search(&di, 0, 0);
      while (dataiterator_step(&di))
	MAPSET(&m, di.solvid);
      dataiterator_free(&di);

      for (p = 1; p < pool->nsolvables; p++)
	{
	  Solvable *s = pool_id2solvable(pool, p);
	  if (!MAPTST(&m, p))
	    continue;
	  printf("  - %s: %s\n", solvable2str(pool, s), solvable_lookup_str(s, SOLVABLE_SUMMARY));
	}
      map_free(&m);
      exit(0);
    }


  if (mainmode == MODE_LIST || mainmode == MODE_INSTALL)
    {
      for (i = 1; i < argc; i++)
	{
	  int l;
          l = strlen(argv[i]);
	  if (l <= 4 || strcmp(argv[i] + l - 4, ".rpm"))
	    continue;
	  if (access(argv[i], R_OK))
	    {
	      perror(argv[i]);
	      exit(1);
	    }
	  if (!commandlinepkgs)
	    commandlinepkgs = sat_calloc(argc, sizeof(Id));
	  if (!commandlinerepo)
	    commandlinerepo = repo_create(pool, "@commandline");
	  repo_add_rpms(commandlinerepo, (const char **)argv + i, 1, REPO_REUSE_REPODATA|REPO_NO_INTERNALIZE);
	  commandlinepkgs[i] = commandlinerepo->end - 1;
	}
      if (commandlinerepo)
	repo_internalize(commandlinerepo);
    }

  // FOR_REPOS(i, repo)
  //   printf("%s: %d solvables\n", repo->name, repo->nsolvables);
  addedfileprovides = 0;
  pool_addfileprovides_ids(pool, 0, &addedfileprovides);
  if (addedfileprovides && *addedfileprovides)
    rewrite_repos(pool, addedfileprovides);
  sat_free(addedfileprovides);
  pool_createwhatprovides(pool);

  queue_init(&job);
  for (i = 1; i < argc; i++)
    {
      Queue job2;
      int j;

      if (commandlinepkgs && commandlinepkgs[i])
	{
	  queue_push2(&job, SOLVER_SOLVABLE, commandlinepkgs[i]);
	  continue;
	}
      queue_init(&job2);
      mkselect(pool, mode, argv[i], &job2);
      if (repofilter && !limitrepo(pool, repofilter, &job2))
        {
	  fprintf(stderr, "nothing in repo matches '%s'\n", argv[i]);
	  exit(1);
        }
      for (j = 0; j < job2.count; j++)
	queue_push(&job, job2.elements[j]);
      queue_free(&job2);
    }

  if (!job.count && mainmode != MODE_UPDATE && mainmode != MODE_DISTUPGRADE && mainmode != MODE_VERIFY && mainmode != MODE_PATCH)
    {
      printf("no package matched\n");
      exit(1);
    }

  if (!job.count)
    allpkgs = 1;

  if (mainmode == MODE_LIST || mainmode == MODE_INFO)
    {
      /* list mode, no solver needed */
      for (i = 0; i < job.count; i += 2)
	{
	  FOR_JOB_SELECT(p, pp, job.elements[i], job.elements[i + 1])
	    {
	      Solvable *s = pool_id2solvable(pool, p);
	      if (mainmode == MODE_INFO)
		{
		  printf("Name:        %s\n", solvable2str(pool, s));
		  printf("Repo:        %s\n", s->repo->name);
		  printf("Summary:     %s\n", solvable_lookup_str(s, SOLVABLE_SUMMARY));
		  printf("Url:         %s\n", solvable_lookup_str(s, SOLVABLE_URL));
		  printf("License:     %s\n", solvable_lookup_str(s, SOLVABLE_LICENSE));
		  printf("Description:\n%s\n", solvable_lookup_str(s, SOLVABLE_DESCRIPTION));
		  printf("\n");
		}
	      else
		{
		  const char *sum = solvable_lookup_str_lang(s, SOLVABLE_SUMMARY, "de");
		  printf("  - %s [%s]\n", solvable2str(pool, s), s->repo->name);
		  if (sum)
		    printf("    %s\n", sum);
		}
	    }
	}
      queue_free(&job);
      pool_free(pool);
      free_repoinfos(repoinfos, nrepoinfos);
      sat_free(commandlinepkgs);
#ifdef FEDORA
      yum_substitute(pool, 0);
#endif
      exit(0);
    }

  if (mainmode == MODE_PATCH)
    {
      int pruneyou = 0;
      Map installedmap;
      Solvable *s;

      map_init(&installedmap, pool->nsolvables);
      if (pool->installed)
        FOR_REPO_SOLVABLES(pool->installed, p, s)
	  MAPSET(&installedmap, p);

      /* install all patches */
      for (p = 1; p < pool->nsolvables; p++)
	{
	  const char *type;
	  int r;
	  Id p2;

	  s = pool->solvables + p;
	  if (strncmp(id2str(pool, s->name), "patch:", 6) != 0)
	    continue;
	  FOR_PROVIDES(p2, pp, s->name)
	    {
	      Solvable *s2 = pool->solvables + p2;
	      if (s2->name != s->name)
		continue;
	      r = evrcmp(pool, s->evr, s2->evr, EVRCMP_COMPARE);
	      if (r < 0 || (r == 0 && p > p2))
		break;
	    }
	  if (p2)
	    continue;
	  type = solvable_lookup_str(s, SOLVABLE_PATCHCATEGORY);
	  if (type && !strcmp(type, "optional"))
	    continue;
	  r = solvable_trivial_installable_map(s, &installedmap, 0);
	  if (r == -1)
	    continue;
	  if (solvable_lookup_bool(s, UPDATE_RESTART) && r == 0)
	    {
	      if (!pruneyou++)
		queue_empty(&job);
	    }
	  else if (pruneyou)
	    continue;
	  queue_push2(&job, SOLVER_SOLVABLE, p);
	}
      map_free(&installedmap);
    }

  // add mode
  for (i = 0; i < job.count; i += 2)
    {
      if (mode == SOLVER_UPDATE)
	{
	  /* make update of not installed packages an install */
	  FOR_JOB_SELECT(p, pp, job.elements[i], job.elements[i + 1])
	    if (pool->installed && pool->solvables[p].repo == pool->installed)
	      break;
	  if (!p)
	    {
	      job.elements[i] |= SOLVER_INSTALL;
	      continue;
	    }
	}
      job.elements[i] |= mode;
    }

  if (mainmode == MODE_DISTUPGRADE && allpkgs && repofilter)
    queue_push2(&job, SOLVER_DISTUPGRADE|SOLVER_SOLVABLE_REPO, repofilter);

  // multiversion test
  // queue_push2(&job, SOLVER_NOOBSOLETES|SOLVER_SOLVABLE_NAME, str2id(pool, "kernel-pae", 1));
  // queue_push2(&job, SOLVER_NOOBSOLETES|SOLVER_SOLVABLE_NAME, str2id(pool, "kernel-pae-base", 1));
  // queue_push2(&job, SOLVER_NOOBSOLETES|SOLVER_SOLVABLE_NAME, str2id(pool, "kernel-pae-extra", 1));

#ifdef SOFTLOCKS_PATH
  addsoftlocks(pool, &job);
#endif

rerunsolver:
  for (;;)
    {
      Id problem, solution;
      int pcnt, scnt;

      solv = solver_create(pool);
      solv->ignorealreadyrecommended = 1;
      solv->updatesystem = allpkgs && !repofilter && (mainmode == MODE_UPDATE || mainmode == MODE_DISTUPGRADE);
      solv->dosplitprovides = solv->updatesystem;
      solv->fixsystem = allpkgs && !repofilter && mainmode == MODE_VERIFY;
      if (mainmode == MODE_DISTUPGRADE && allpkgs && !repofilter)
	{
	  solv->distupgrade = 1;
	  solv->allowdowngrade = 1;
	  solv->allowarchchange = 1;
	  solv->allowvendorchange = 1;
	}
      if (mainmode == MODE_ERASE)
	solv->allowuninstall = 1;	/* don't nag */

      solver_solve(solv, &job);
      if (!solv->problems.count)
	break;
      pcnt = solver_problem_count(solv);
      printf("Found %d problems:\n", pcnt);
      for (problem = 1; problem <= pcnt; problem++)
	{
	  int take = 0;
	  printf("Problem %d:\n", problem);
	  solver_printprobleminfo(solv, problem);
	  printf("\n");
	  scnt = solver_solution_count(solv, problem);
	  for (solution = 1; solution <= scnt; solution++)
	    {
	      printf("Solution %d:\n", solution);
	      solver_printsolution(solv, problem, solution);
	      printf("\n");
	    }
	  for (;;)
	    {
	      printf("Please choose a solution: ");
	      fflush(stdout);
	      *inbuf = 0;
	      if (!(ip = fgets(inbuf, sizeof(inbuf), stdin)))
		{
		  printf("Abort.\n");
		  exit(1);
		}
	      while (*ip == ' ' || *ip == '\t')
		ip++;
	      if (*ip >= '0' && *ip <= '9')
		{
		  take = atoi(ip);
		  if (take >= 1 && take <= scnt)
		    break;
		}
	      if (*ip == 's')
		{
		  take = 0;
		  break;
		}
	      if (*ip == 'q')
		{
		  printf("Abort.\n");
		  exit(1);
		}
	    }
	  if (!take)
	    continue;
	  solver_take_solution(solv, problem, take, &job);
	}
      solver_free(solv);
      solv = 0;
    }

  trans = &solv->trans;
  if (!trans->steps.count)
    {
      printf("Nothing to do.\n");
      exit(1);
    }
  printf("\n");
  printf("Transaction summary:\n\n");
  solver_printtransaction(solv);

#ifndef FEDORA
  if (1)
    {
      DUChanges duc[4];
      int i;

      duc[0].path = "/";
      duc[1].path = "/usr/share/man";
      duc[2].path = "/sbin";
      duc[3].path = "/etc";
      transaction_calc_duchanges(trans, duc, 4);
      for (i = 0; i < 4; i++)
        printf("duchanges %s: %d K  %d inodes\n", duc[i].path, duc[i].kbytes, duc[i].files);
    }
#endif
  printf("install size change: %d K\n", transaction_calc_installsizechange(trans));
  printf("\n");

  if (!yesno("OK to continue (y/n)? "))
    {
      printf("Abort.\n");
      exit(1);
    }

  queue_init(&checkq);
  newpkgs = transaction_installedresult(trans, &checkq);
  newpkgsfps = 0;

  if (newpkgs)
    {
      int downloadsize = 0;
      for (i = 0; i < newpkgs; i++)
	{
	  Solvable *s;

	  p = checkq.elements[i];
	  s = pool_id2solvable(pool, p);
	  downloadsize += solvable_lookup_num(s, SOLVABLE_DOWNLOADSIZE, 0);
	}
      printf("Downloading %d packages, %d K\n", newpkgs, downloadsize);
      newpkgsfps = sat_calloc(newpkgs, sizeof(*newpkgsfps));
      for (i = 0; i < newpkgs; i++)
	{
	  unsigned int medianr;
	  char *loc;
	  Solvable *s;
	  struct repoinfo *cinfo;
	  const unsigned char *chksum;
	  Id chksumtype;
	  Dataiterator di;

	  p = checkq.elements[i];
	  s = pool_id2solvable(pool, p);
	  if (s->repo == commandlinerepo)
	    {
	      loc = solvable_get_location(s, &medianr);
	      if (!(newpkgsfps[i] = fopen(loc, "r")))
		{
		  perror(loc);
		  exit(1);
		}
	      putchar('.');
	      continue;
	    }
	  cinfo = s->repo->appdata;
	  if (!cinfo)
	    {
	      printf("%s: no repository information\n", s->repo->name);
	      exit(1);
	    }
	  loc = solvable_get_location(s, &medianr);
	  if (!loc)
	     continue;

	  if (pool->installed && pool->installed->nsolvables)
	    {
	      /* try a delta first */
	      char *matchname = strdup(id2str(pool, s->name));
	      dataiterator_init(&di, pool, s->repo, SOLVID_META, DELTA_PACKAGE_NAME, matchname, SEARCH_STRING);
	      dataiterator_prepend_keyname(&di, REPOSITORY_DELTAINFO);
	      while (dataiterator_step(&di))
		{
		  Id baseevr, op;

		  dataiterator_setpos_parent(&di);
		  if (pool_lookup_id(pool, SOLVID_POS, DELTA_PACKAGE_EVR) != s->evr ||
		      pool_lookup_id(pool, SOLVID_POS, DELTA_PACKAGE_ARCH) != s->arch)
		    continue;
		  baseevr = pool_lookup_id(pool, SOLVID_POS, DELTA_BASE_EVR);
		  FOR_PROVIDES(op, pp, s->name)
		    {
		      Solvable *os = pool->solvables + op;
		      if (os->repo == pool->installed && os->name == s->name && os->arch == s->arch && os->evr == baseevr)
			break;
		    }
		  if (op && access("/usr/bin/applydeltarpm", X_OK) == 0)
		    {
		      /* base is installed, run sequence check */
		      const char *seqname;
		      const char *seqevr;
		      const char *seqnum;
		      const char *seq;
		      const char *dloc;
		      FILE *fp;
		      char cmd[128];
		      int newfd;

		      seqname = pool_lookup_str(pool, SOLVID_POS, DELTA_SEQ_NAME);
		      seqevr = pool_lookup_str(pool, SOLVID_POS, DELTA_SEQ_EVR);
		      seqnum = pool_lookup_str(pool, SOLVID_POS, DELTA_SEQ_NUM);
		      seq = pool_tmpjoin(pool, seqname, "-", seqevr);
		      seq = pool_tmpjoin(pool, seq, "-", seqnum);
#ifdef FEDORA
		      sprintf(cmd, "/usr/bin/applydeltarpm -a %s -c -s ", id2str(pool, s->arch));
#else
		      sprintf(cmd, "/usr/bin/applydeltarpm -c -s ");
#endif
		      if (system(pool_tmpjoin(pool, cmd, seq, 0)) != 0)
			continue;	/* didn't match */
		      /* looks good, download delta */
		      chksumtype = 0;
		      chksum = pool_lookup_bin_checksum(pool, SOLVID_POS, DELTA_CHECKSUM, &chksumtype);
		      if (!chksumtype)
			continue;	/* no way! */
		      dloc = pool_lookup_str(pool, SOLVID_POS, DELTA_LOCATION_DIR);
		      dloc = pool_tmpjoin(pool, dloc, "/", pool_lookup_str(pool, SOLVID_POS, DELTA_LOCATION_NAME));
		      dloc = pool_tmpjoin(pool, dloc, "-", pool_lookup_str(pool, SOLVID_POS, DELTA_LOCATION_EVR));
		      dloc = pool_tmpjoin(pool, dloc, ".", pool_lookup_str(pool, SOLVID_POS, DELTA_LOCATION_SUFFIX));
		      if ((fp = curlfopen(cinfo, dloc, 0, chksum, chksumtype, 0)) == 0)
			continue;
		      /* got it, now reconstruct */
		      newfd = opentmpfile();
#ifdef FEDORA
		      sprintf(cmd, "applydeltarpm -a %s /dev/fd/%d /dev/fd/%d", id2str(pool, s->arch), fileno(fp), newfd);
#else
		      sprintf(cmd, "applydeltarpm /dev/fd/%d /dev/fd/%d", fileno(fp), newfd);
#endif
		      fcntl(fileno(fp), F_SETFD, 0);
		      if (system(cmd))
			{
			  close(newfd);
			  fclose(fp);
			  continue;
			}
		      lseek(newfd, 0, SEEK_SET);
		      chksumtype = 0;
		      chksum = solvable_lookup_bin_checksum(s, SOLVABLE_CHECKSUM, &chksumtype);
		      if (chksumtype && !verify_checksum(newfd, loc, chksum, chksumtype))
			{
			  close(newfd);
			  fclose(fp);
			  continue;
			}
		      newpkgsfps[i] = fdopen(newfd, "r");
		      fclose(fp);
		      break;
		    }
		}
	      dataiterator_free(&di);
	      sat_free(matchname);
	    }
	  
	  if (newpkgsfps[i])
	    {
	      putchar('d');
	      fflush(stdout);
	      continue;		/* delta worked! */
	    }
	  if (cinfo->type == TYPE_SUSETAGS)
	    {
	      const char *datadir = repo_lookup_str(cinfo->repo, SOLVID_META, SUSETAGS_DATADIR);
	      loc = pool_tmpjoin(pool, datadir ? datadir : "suse", "/", loc);
	    }
	  chksumtype = 0;
	  chksum = solvable_lookup_bin_checksum(s, SOLVABLE_CHECKSUM, &chksumtype);
	  if ((newpkgsfps[i] = curlfopen(cinfo, loc, 0, chksum, chksumtype, 0)) == 0)
	    {
	      printf("\n%s: %s not found in repository\n", s->repo->name, loc);
	      exit(1);
	    }
	  putchar('.');
	  fflush(stdout);
	}
      putchar('\n');
    }

  if (newpkgs)
    {
      Queue conflicts;

      printf("Searching for file conflicts\n");
      queue_init(&conflicts);
      fcstate.rpmdbstate = 0;
      fcstate.newpkgscnt = newpkgs;
      fcstate.checkq = &checkq;
      fcstate.newpkgsfps = newpkgsfps;
      pool_findfileconflicts(pool, &checkq, newpkgs, &conflicts, &fileconflict_cb, &fcstate);
      if (conflicts.count)
	{
	  printf("\n");
	  for (i = 0; i < conflicts.count; i += 5)
	    printf("file %s of package %s conflicts with package %s\n", id2str(pool, conflicts.elements[i]), solvid2str(pool, conflicts.elements[i + 1]), solvid2str(pool, conflicts.elements[i + 3]));
	  printf("\n");
	  if (yesno("Re-run solver (y/n/q)? "))
	    {
	      for (i = 0; i < newpkgs; i++)
		if (newpkgsfps[i])
		  fclose(newpkgsfps[i]);
	      newpkgsfps = sat_free(newpkgsfps);
	      solver_free(solv);
	      pool_add_fileconflicts_deps(pool, &conflicts);
	      pool_createwhatprovides(pool);	/* Hmm... */
	      goto rerunsolver;
	    }
	}
      queue_free(&conflicts);
    }

  printf("Committing transaction:\n\n");
  transaction_order(trans, 0);
  for (i = 0; i < trans->steps.count; i++)
    {
      const char *evr, *evrp, *nvra;
      Solvable *s;
      int j;
      FILE *fp;

      p = trans->steps.elements[i];
      s = pool_id2solvable(pool, p);
      Id type = transaction_type(trans, p, SOLVER_TRANSACTION_RPM_ONLY);
      switch(type)
	{
	case SOLVER_TRANSACTION_ERASE:
	  printf("erase %s\n", solvid2str(pool, p));
	  if (!s->repo->rpmdbid || !s->repo->rpmdbid[p - s->repo->start])
	    continue;
	  /* strip epoch from evr */
	  evr = evrp = id2str(pool, s->evr);
	  while (*evrp >= '0' && *evrp <= '9')
	    evrp++;
	  if (evrp > evr && evrp[0] == ':' && evrp[1])
	    evr = evrp + 1;
	  nvra = pool_tmpjoin(pool, id2str(pool, s->name), "-", evr);
	  nvra = pool_tmpjoin(pool, nvra, ".", id2str(pool, s->arch));
	  runrpm("-e", nvra, -1);	/* to bad that --querybynumber doesn't work */
	  break;
	case SOLVER_TRANSACTION_INSTALL:
	case SOLVER_TRANSACTION_MULTIINSTALL:
	  printf("install %s\n", solvid2str(pool, p));
	  for (j = 0; j < newpkgs; j++)
	    if (checkq.elements[j] == p)
	      break;
	  fp = j < newpkgs ? newpkgsfps[j] : 0;
	  if (!fp)
	    continue;
	  rewind(fp);
	  lseek(fileno(fp), 0, SEEK_SET);
	  runrpm(type == SOLVER_TRANSACTION_MULTIINSTALL ? "-i" : "-U", "/dev/fd/3", fileno(fp));
	  fclose(fp);
	  newpkgsfps[j] = 0;
	  break;
	default:
	  break;
	}
    }

  for (i = 0; i < newpkgs; i++)
    if (newpkgsfps[i])
      fclose(newpkgsfps[i]);
  sat_free(newpkgsfps);
  queue_free(&checkq);
  solver_free(solv);
  queue_free(&job);
  pool_free(pool);
  free_repoinfos(repoinfos, nrepoinfos);
  sat_free(commandlinepkgs);
#ifdef FEDORA
  yum_substitute(pool, 0);
#endif
  exit(0);
}
