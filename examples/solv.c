/*
 * Copyright (c) 2009, Novell Inc.
 *
 * This program is licensed under the BSD license, read LICENSE.BSD
 * for further information
 */

/* solv, a little software installer demoing the sat solver library */

/* things it does:
 * - understands globs for package names / dependencies
 * - understands .arch suffix
 * - installation of commandline packages
 * - repository data caching
 * - on demand loading of secondary repository data
 * - gpg and checksum verification
 * - file conflicts
 * - deltarpm support
 * - fastestmirror implementation
 *
 * things available in the library but missing from solv:
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
#include <sys/dir.h>
#include <sys/stat.h>

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
#ifdef ENABLE_RPMDB
#include "repo_rpmdb.h"
#include "pool_fileconflicts.h"
#endif
#ifdef ENABLE_DEBIAN
#include "repo_deb.h"
#endif
#ifdef ENABLE_RPMMD
#include "repo_rpmmd.h"
#include "repo_repomdxml.h"
#include "repo_updateinfoxml.h"
#include "repo_deltainfoxml.h"
#endif
#ifdef ENABLE_SUSEREPO
#include "repo_products.h"
#include "repo_susetags.h"
#include "repo_content.h"
#endif
#include "solv_xfopen.h"

#ifdef FEDORA
# define REPOINFO_PATH "/etc/yum.repos.d"
#endif
#ifdef SUSE
# define REPOINFO_PATH "/etc/zypp/repos.d"
# define PRODUCTS_PATH "/etc/products.d"
# define SOFTLOCKS_PATH "/var/lib/zypp/SoftLocks"
#endif

#define SOLVCACHE_PATH "/var/cache/solv"

#define METADATA_EXPIRE (60 * 15)

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
  char **components;
  int ncomponents;

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
      solv_free(releaseevr);
      releaseevr = 0;
      solv_free(basearch);
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
#define TYPE_DEBIAN     4

#ifndef NOSYSTEM
static int
read_repoinfos_sort(const void *ap, const void *bp)
{
  const struct repoinfo *a = ap;
  const struct repoinfo *b = bp;
  return strcmp(a->alias, b->alias);
}
#endif

#if defined(SUSE) || defined(FEDORA)

struct repoinfo *
read_repoinfos(Pool *pool, int *nrepoinfosp)
{
  const char *reposdir = REPOINFO_PATH;
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
      if (ent->d_name[0] == '.')
	continue;
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
	      repoinfos = solv_extend(repoinfos, nrepoinfos, 1, sizeof(*repoinfos), 15);
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

#endif

#ifdef DEBIAN

struct repoinfo *
read_repoinfos(Pool *pool, int *nrepoinfosp)
{
  FILE *fp;
  char buf[4096];
  char buf2[4096];
  int l;
  char *kp, *url, *distro;
  struct repoinfo *repoinfos = 0, *cinfo;
  int nrepoinfos = 0;
  DIR *dir = 0;
  struct dirent *ent;

  fp = fopen("/etc/apt/sources.list", "r");
  while (1)
    {
      if (!fp)
	{
	  if (!dir)
	    {
	      dir = opendir("/etc/apt/sources.list.d");
	      if (!dir)
		break;
	    }
	  if ((ent = readdir(dir)) == 0)
	    {
	      closedir(dir);
	      break;
	    }
	  if (ent->d_name[0] == '.')
	    continue;
	  l = strlen(ent->d_name);
	  if (l < 5 || strcmp(ent->d_name + l - 5, ".list") != 0)
	    continue;
	  snprintf(buf, sizeof(buf), "%s/%s", "/etc/apt/sources.list.d", ent->d_name);
	  if (!(fp = fopen(buf, "r")))
	    continue;
	}
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
	  if (strncmp(kp, "deb", 3) != 0)
	    continue;
	  kp += 3;
	  if (*kp != ' ' && *kp != '\t')
	    continue;
	  while (*kp == ' ' || *kp == '\t')
	    kp++;
	  if (!*kp)
	    continue;
	  url = kp;
	  while (*kp && *kp != ' ' && *kp != '\t')
	    kp++;
	  if (*kp)
	    *kp++ = 0;
	  while (*kp == ' ' || *kp == '\t')
	    kp++;
	  if (!*kp)
	    continue;
	  distro = kp;
	  while (*kp && *kp != ' ' && *kp != '\t')
	    kp++;
	  if (*kp)
	    *kp++ = 0;
	  while (*kp == ' ' || *kp == '\t')
	    kp++;
	  if (!*kp)
	    continue;
	  repoinfos = solv_extend(repoinfos, nrepoinfos, 1, sizeof(*repoinfos), 15);
	  cinfo = repoinfos + nrepoinfos++;
	  memset(cinfo, 0, sizeof(*cinfo));
	  cinfo->baseurl = strdup(url);
	  cinfo->alias = solv_dupjoin(url, "/", distro);
	  cinfo->name = strdup(distro);
	  cinfo->type = TYPE_DEBIAN;
	  cinfo->enabled = 1;
	  cinfo->autorefresh = 1;
	  cinfo->repo_gpgcheck = 1;
	  cinfo->metadata_expire = METADATA_EXPIRE;
	  while (*kp)
	    {
	      char *compo;
	      while (*kp == ' ' || *kp == '\t')
		kp++;
	      if (!*kp)
		break;
	      compo = kp;
	      while (*kp && *kp != ' ' && *kp != '\t')
		kp++;
	      if (*kp)
		*kp++ = 0;
	      cinfo->components = solv_extend(cinfo->components, cinfo->ncomponents, 1, sizeof(*cinfo->components), 15);
	      cinfo->components[cinfo->ncomponents++] = strdup(compo);
	    }
	}
      fclose(fp);
      fp = 0;
    }
  qsort(repoinfos, nrepoinfos, sizeof(*repoinfos), read_repoinfos_sort);
  *nrepoinfosp = nrepoinfos;
  return repoinfos;
}

#endif

#ifdef NOSYSTEM
struct repoinfo *
read_repoinfos(Pool *pool, int *nrepoinfosp)
{
  *nrepoinfosp = 0;
  return 0;
}
#endif


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
  const unsigned char *sum;
  void *h;
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

void
findfastest(char **urls, int nurls)
{
  int i, j, port;
  int *socks, qc;
  struct pollfd *fds;
  char *p, *p2, *q;
  char portstr[16];
  struct addrinfo hints, *result;;

  fds = solv_calloc(nurls, sizeof(*fds));
  socks = solv_calloc(nurls, sizeof(*socks));
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
	  bp += 20;
	  if (solv_hex2bin((const char **)&bp, chksump, 32) == 32)
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
      urls = solv_extend(urls, nurls, 1, sizeof(*urls), 15);
      urls[nurls++] = strdup(bp);
    }
  if (nurls)
    {
      if (nurls > 1)
        findfastest(urls, nurls > 5 ? 5 : nurls);
      bp = urls[0];
      urls[0] = 0;
      for (i = 0; i < nurls; i++)
        solv_free(urls[i]);
      solv_free(urls);
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
      urls = solv_extend(urls, nurls, 1, sizeof(*urls), 15);
      urls[nurls++] = strdup(bp);
    }
  if (nurls)
    {
      if (nurls > 1)
        findfastest(urls, nurls > 5 ? 5 : nurls);
      bp = urls[0];
      urls[0] = 0;
      for (i = 0; i < nurls; i++)
        solv_free(urls[i]);
      solv_free(urls);
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

static inline int
iscompressed(const char *name)
{
  int l = strlen(name);
  return l > 3 && !strcmp(name + l - 3, ".gz") ? 1 : 0;
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
    return solv_xfopen_fd(".gz", fd, "r");
  fcntl(fd, F_SETFD, FD_CLOEXEC);
  return fdopen(fd, "r");
}

#ifndef DEBIAN

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

#else

int
checksig(Pool *sigpool, FILE *fp, FILE *sigfp)
{
  char cmd[256];
  int r;

  snprintf(cmd, sizeof(cmd), "gpgv -q --keyring /etc/apt/trusted.gpg /dev/fd/%d /dev/fd/%d >/dev/null 2>&1", fileno(sigfp), fileno(fp));
  fcntl(fileno(fp), F_SETFD, 0);	/* clear CLOEXEC */
  fcntl(fileno(sigfp), F_SETFD, 0);	/* clear CLOEXEC */
  r = system(cmd);
  fcntl(fileno(fp), F_SETFD, FD_CLOEXEC);
  fcntl(fileno(sigfp), F_SETFD, FD_CLOEXEC);
  return r == 0 ? 1 : 0;
}

#endif

#define CHKSUM_IDENT "1.1"

void
calc_checksum_fp(FILE *fp, Id chktype, unsigned char *out)
{
  char buf[4096];
  void *h = solv_chksum_create(chktype);
  int l;

  solv_chksum_add(h, CHKSUM_IDENT, strlen(CHKSUM_IDENT));
  while ((l = fread(buf, 1, sizeof(buf), fp)) > 0)
    solv_chksum_add(h, buf, l);
  rewind(fp);
  solv_chksum_free(h, out);
}

void
calc_checksum_stat(struct stat *stb, Id chktype, unsigned char *out)
{
  void *h = solv_chksum_create(chktype);
  solv_chksum_add(h, CHKSUM_IDENT, strlen(CHKSUM_IDENT));
  solv_chksum_add(h, &stb->st_dev, sizeof(stb->st_dev));
  solv_chksum_add(h, &stb->st_ino, sizeof(stb->st_ino));
  solv_chksum_add(h, &stb->st_size, sizeof(stb->st_size));
  solv_chksum_add(h, &stb->st_mtime, sizeof(stb->st_mtime));
  solv_chksum_free(h, out);
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
      p = pool_tmpappend(repo->pool, p, "_", repoext);
      p = pool_tmpappend(repo->pool, p, ".solvx", 0);
    }
  else
    p = pool_tmpappend(repo->pool, p, ".solv", 0);
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
    {
      flags = REPO_USE_LOADING|REPO_EXTEND_SOLVABLES;
      if (strcmp(repoext, "DL") != 0)
        flags |= REPO_LOCALPOOL;	/* no local pool for DL so that we can compare IDs */
    }

  if (repo_add_solv(repo, fp, flags))
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
  /* use dupjoin instead of tmpjoin because tmpl must survive repo_write */
  tmpl = solv_dupjoin(SOLVCACHE_PATH, "/", ".newsolv-XXXXXX");
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
    repo_write(repo, fp);
  else if (repoext)
    repodata_write(info, fp);
  else
    {
      int oldnrepodata = repo->nrepodata;
      repo->nrepodata = oldnrepodata > 2 ? 2 : oldnrepodata;	/* XXX: do this right */
      repo_write(repo, fp);
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
	      if (repo_add_solv(repo, fp, SOLV_ADD_NO_STUBS))
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
	      repo_add_solv(repo, fp, REPO_USE_LOADING|REPO_EXTEND_SOLVABLES);
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
#if defined(ENABLE_RPMDB_PUBKEY)
  Repo *repo = repo_create(sigpool, "rpmdbkeys");
  repo_add_rpmdb_pubkeys(repo, 0, 0);
#endif
  return sigpool;
}


#ifdef ENABLE_RPMMD
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
  Id chksumtype, handle;
  const unsigned char *chksum;
  const char *filename;

  filename = repomd_find(repo, what, &chksum, &chksumtype);
  if (!filename)
    return 0;
  if (!strcmp(what, "prestodelta"))
    what = "deltainfo";
  handle = repodata_new_handle(data);
  repodata_set_poolstr(data, handle, REPOSITORY_REPOMD_TYPE, what);
  repodata_set_str(data, handle, REPOSITORY_REPOMD_LOCATION, filename);
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
  repodata_add_flexarray(data, SOLVID_META, REPOSITORY_EXTERNAL, handle);
  return 1;
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
#if 1
  printf("[%s:%s", repo->name, ext);
#endif
  if (usecachedrepo(repo, ext, cinfo->extcookie, 0))
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
    r = repo_add_rpmmd(repo, fp, ext, REPO_USE_LOADING|REPO_EXTEND_SOLVABLES);
  else if (!strcmp(ext, "DL"))
    r = repo_add_deltainfoxml(repo, fp, REPO_USE_LOADING);
  fclose(fp);
  if (r)
    {
      printf("%s\n", pool_errstr(repo->pool));
      return 0;
    }
  writecachedrepo(repo, data, ext, cinfo->extcookie);
  return 1;
}

#endif


#ifdef ENABLE_SUSEREPO
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

  cinfo = repo->appdata;
  filename = repodata_lookup_str(data, SOLVID_META, SUSETAGS_FILE_NAME);
  if (!filename)
    return 0;
  /* susetags load */
  ext[0] = filename[9];
  ext[1] = filename[10];
  ext[2] = 0;
#if 1
  printf("[%s:%s", repo->name, ext);
#endif
  if (usecachedrepo(repo, ext, cinfo->extcookie, 0))
    {
      printf(" cached]\n"); fflush(stdout);
      return 1;
    }
#if 1
  printf(" fetching]\n"); fflush(stdout);
#endif
  defvendor = repo_lookup_id(repo, SOLVID_META, SUSETAGS_DEFAULTVENDOR);
  descrdir = repo_lookup_str(repo, SOLVID_META, SUSETAGS_DESCRDIR);
  if (!descrdir)
    descrdir = "suse/setup/descr";
  filechksumtype = 0;
  filechksum = repodata_lookup_bin_checksum(data, SOLVID_META, SUSETAGS_FILE_CHECKSUM, &filechksumtype);
  if ((fp = curlfopen(cinfo, pool_tmpjoin(repo->pool, descrdir, "/", filename), iscompressed(filename), filechksum, filechksumtype, 0)) == 0)
    return 0;
  if (repo_add_susetags(repo, fp, defvendor, ext, REPO_USE_LOADING|REPO_EXTEND_SOLVABLES))
    {
      fclose(fp);
      printf("%s\n", pool_errstr(repo->pool));
      return 0;
    }
  fclose(fp);
  writecachedrepo(repo, data, ext, cinfo->extcookie);
  return 1;
}
#endif



/* load callback */

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
    default:
      return 0;
    }
}

static unsigned char installedcookie[32];

#ifdef ENABLE_DEBIAN

const char *
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
#endif

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
#ifdef ENABLE_SUSEREPO
  const char *descrdir;
  int defvendor;
#endif
  struct stat stb;
  Pool *sigpool = 0;
#if defined(ENABLE_SUSEREPO) || defined(ENABLE_RPMMD)
  Repodata *data;
#endif
  int badchecksum;
  int dorefresh;
#if defined(ENABLE_DEBIAN)
  FILE *fpr;
  int j;
#endif

  repo = repo_create(pool, "@System");
#if defined(ENABLE_RPMDB) && (defined(SUSE) || defined(FEDORA))
  printf("rpm database:");
  if (stat("/var/lib/rpm/Packages", &stb))
    memset(&stb, 0, sizeof(&stb));
#endif
#if defined(ENABLE_DEBIAN) && defined(DEBIAN)
  printf("dpgk database:");
  if (stat("/var/lib/dpkg/status", &stb))
    memset(&stb, 0, sizeof(&stb));
#endif
#ifdef NOSYSTEM
  printf("no installed database:");
  memset(&stb, 0, sizeof(&stb));
#endif
  calc_checksum_stat(&stb, REPOKEY_TYPE_SHA256, installedcookie);
  if (usecachedrepo(repo, 0, installedcookie, 0))
    printf(" cached\n");
  else
    {
#if defined(ENABLE_RPMDB) && (defined(SUSE) || defined(FEDORA))
      FILE *ofp;
      int done = 0;
#endif
      printf(" reading\n");

#if defined(ENABLE_RPMDB) && (defined(SUSE) || defined(FEDORA))
# if defined(ENABLE_SUSEREPO) && defined(PRODUCTS_PATH)
      if (repo_add_products(repo, PRODUCTS_PATH, 0, REPO_NO_INTERNALIZE))
	{
	  fprintf(stderr, "product reading failed: %s\n", pool_errstr(pool));
	  exit(1);
	}
# endif
      if ((ofp = fopen(calccachepath(repo, 0), "r")) != 0)
	{
	  Repo *ref = repo_create(pool, "@System.old");
	  if (!repo_add_solv(ref, ofp, 0))
	    {
	      if (repo_add_rpmdb(repo, ref, 0, REPO_REUSE_REPODATA))
		{
		  fprintf(stderr, "installed db: %s\n", pool_errstr(pool));
		  exit(1);
		}
	      done = 1;
	    }
	  fclose(ofp);
	  repo_free(ref, 1);
	}
      if (!done)
	{
	  if (repo_add_rpmdb(repo, 0, 0, REPO_REUSE_REPODATA))
	    {
	      fprintf(stderr, "installed db: %s\n", pool_errstr(pool));
	      exit(1);
	    }
	}
#endif
#if defined(ENABLE_DEBIAN) && defined(DEBIAN)
      if (repo_add_debdb(repo, 0, REPO_REUSE_REPODATA))
	{
	  fprintf(stderr, "installed db: %s\n", pool_errstr(pool));
	  exit(1);
	}
#endif
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
#ifdef ENABLE_RPMMD
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
	  if (repo_add_repomdxml(repo, fp, 0))
	    {
	      printf("repomd.xml: %s\n", pool_errstr(pool));
	      fclose(fp);
	      break;	/* hopeless */
	    }
	  fclose(fp);
	  printf(" fetching\n");
	  filename = repomd_find(repo, "primary", &filechksum, &filechksumtype);
	  if (filename && (fp = curlfopen(cinfo, filename, iscompressed(filename), filechksum, filechksumtype, &badchecksum)) != 0)
	    {
	      if (repo_add_rpmmd(repo, fp, 0, 0))
		{
	          printf("primary: %s\n", pool_errstr(pool));
		  badchecksum = 1;
		}
	      fclose(fp);
	    }
	  if (badchecksum)
	    break;	/* hopeless */

	  filename = repomd_find(repo, "updateinfo", &filechksum, &filechksumtype);
	  if (filename && (fp = curlfopen(cinfo, filename, iscompressed(filename), filechksum, filechksumtype, &badchecksum)) != 0)
	    {
	      if (repo_add_updateinfoxml(repo, fp, 0))
		{
	          printf("updateinfo: %s\n", pool_errstr(pool));
		  badchecksum = 1;
		}
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
#endif

#ifdef ENABLE_SUSEREPO
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
	  if (repo_add_content(repo, fp, 0))
	    {
	      printf("content: %s\n", pool_errstr(pool));
	      fclose(fp);
	      break;	/* hopeless */
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
	      break;
	    }
	  printf(" fetching\n");
	  if ((fp = curlfopen(cinfo, pool_tmpjoin(pool, descrdir, "/", filename), iscompressed(filename), filechksum, filechksumtype, &badchecksum)) == 0)
	    break;	/* hopeless */
	  if (repo_add_susetags(repo, fp, defvendor, 0, REPO_NO_INTERNALIZE|SUSETAGS_RECORD_SHARES))
	    {
	      printf("packages: %s\n", pool_errstr(pool));
	      fclose(fp);
	      break;	/* hopeless */
	    }
	  fclose(fp);
	  /* add default language */
	  filename = susetags_find(repo, "packages.en.gz", &filechksum, &filechksumtype);
          if (!filename)
	    filename = susetags_find(repo, "packages.en", &filechksum, &filechksumtype);
	  if (filename)
	    {
	      if ((fp = curlfopen(cinfo, pool_tmpjoin(pool, descrdir, "/", filename), iscompressed(filename), filechksum, filechksumtype, &badchecksum)) != 0)
		{
		  if (repo_add_susetags(repo, fp, defvendor, 0, REPO_NO_INTERNALIZE|REPO_REUSE_REPODATA|REPO_EXTEND_SOLVABLES))
		    {
		      printf("packages.en: %s\n", pool_errstr(pool));
		      badchecksum = 1;
		    }
		  fclose(fp);
		}
	    }
	  filename = susetags_find(repo, "patterns", &filechksum, &filechksumtype);
	  if (filename)
	    {
	      if ((fp = curlfopen(cinfo, pool_tmpjoin(pool, descrdir, "/", filename), iscompressed(filename), filechksum, filechksumtype, &badchecksum)) != 0)
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
		      if (filename && (fp2 = curlfopen(cinfo, pool_tmpjoin(pool, descrdir, "/", filename), iscompressed(filename), filechksum, filechksumtype, &badchecksum)) != 0)
			{
			  if (repo_add_susetags(repo, fp2, defvendor, 0, REPO_NO_INTERNALIZE))
			    {
			      printf("%s: %s\n", pbuf, pool_errstr(pool));
			      badchecksum = 1;
			    }
			  fclose(fp2);
			}
		    }
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
#endif

#if defined(ENABLE_DEBIAN)
        case TYPE_DEBIAN:
	  printf("debian repo '%s':", cinfo->alias);
	  fflush(stdout);
	  filename = solv_dupjoin("dists/", cinfo->name, "/Release");
	  if ((fpr = curlfopen(cinfo, filename, 0, 0, 0, 0)) == 0)
	    {
	      printf(" no Release file, skipped\n");
	      repo_free(repo, 1);
	      cinfo->repo = 0;
	      free((char *)filename);
	      break;
	    }
	  solv_free((char *)filename);
	  if (cinfo->repo_gpgcheck)
	    {
	      filename = solv_dupjoin("dists/", cinfo->name, "/Release.gpg");
	      sigfp = curlfopen(cinfo, filename, 0, 0, 0, 0);
	      solv_free((char *)filename);
	      if (!sigfp)
		{
		  printf(" unsigned, skipped\n");
		  fclose(fpr);
		  break;
		}
	      if (!sigpool)
		sigpool = read_sigs();
	      if (!checksig(sigpool, fpr, sigfp))
		{
		  printf(" checksig failed, skipped\n");
		  fclose(sigfp);
		  fclose(fpr);
		  break;
		}
	      fclose(sigfp);
	    }
	  calc_checksum_fp(fpr, REPOKEY_TYPE_SHA256, cinfo->cookie);
	  if (usecachedrepo(repo, 0, cinfo->cookie, 1))
	    {
	      printf(" cached\n");
              fclose(fpr);
	      break;
	    }
	  printf(" fetching\n");
          for (j = 0; j < cinfo->ncomponents; j++)
	    {
	      if (!(filename = debian_find_component(cinfo, fpr, cinfo->components[j], &filechksum, &filechksumtype)))
		{
		  printf("[component %s not found]\n", cinfo->components[j]);
		  continue;
		}
	      if ((fp = curlfopen(cinfo, filename, iscompressed(filename), filechksum, filechksumtype, &badchecksum)) != 0)
		{
	          if (repo_add_debpackages(repo, fp, 0))
		    {
		      printf("component %s: %s\n", cinfo->components[j], pool_errstr(pool));
		      badchecksum = 1;
		    }
		  fclose(fp);
		}
	      solv_free((char *)filechksum);
	      solv_free((char *)filename);
	    }
	  fclose(fpr);
	  if (!badchecksum)
	    writecachedrepo(repo, 0, 0, cinfo->cookie);
	  break;
#endif

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
  id = pool_str2id(pool, arch, 0);
  if (id == ARCH_SRC || id == ARCH_NOSRC || id == ARCH_NOARCH)
    return id;
  if (pool->id2arch && (id > pool->lastarch || !pool->id2arch[id]))
    return 0;
  return id;
}


#define DEPGLOB_NAME     1
#define DEPGLOB_DEP      2
#define DEPGLOB_NAMEDEP  3

int
depglob(Pool *pool, char *name, Queue *job, int what)
{
  Id p, pp;
  Id id = pool_str2id(pool, name, 0);
  int i, match = 0;

  if (id)
    {
      FOR_PROVIDES(p, pp, id)
	{
	  Solvable *s = pool->solvables + p;
	  match = 1;
	  if (s->name == id && (what & DEPGLOB_NAME) != 0)
	    {
	      queue_push2(job, SOLVER_SOLVABLE_NAME, id);
	      return 1;
	    }
	}
      if (match)
	{
	  if (what == DEPGLOB_NAMEDEP)
	    printf("[using capability match for '%s']\n", name);
	  queue_push2(job, SOLVER_SOLVABLE_PROVIDES, id);
	  return 1;
	}
    }

  if (strpbrk(name, "[*?") == 0)
    return 0;

  if ((what & DEPGLOB_NAME) != 0)
    {
      /* looks like a name glob. hard work. */
      for (p = 1; p < pool->nsolvables; p++)
	{
	  Solvable *s = pool->solvables + p;
	  if (!s->repo || !pool_installable(pool, s))
	    continue;
	  id = s->name;
	  if (fnmatch(name, pool_id2str(pool, id), 0) == 0)
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
    }
  if ((what & DEPGLOB_DEP))
    {
      /* looks like a dep glob. really hard work. */
      for (id = 1; id < pool->ss.nstrings; id++)
	{
	  if (!pool->whatprovides[id])
	    continue;
	  if (fnmatch(name, pool_id2str(pool, id), 0) == 0)
	    {
	      if (!match && what == DEPGLOB_NAMEDEP)
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
    }
  return 0;
}

int
limitrelation(Pool *pool, Queue *job, int flags, Id evr)
{
  int i, j;
  Id p, pp;
  for (i = j = 0; i < job->count; i += 2)
    {
      Id select = job->elements[i] & SOLVER_SELECTMASK;
      if (select != SOLVER_SOLVABLE_NAME && select != SOLVER_SOLVABLE_PROVIDES)
	{
	  fprintf(stderr, "limitrelation only works on name/provides jobs\n");
	  exit(1);
	}
      job->elements[i + 1] = pool_rel2id(pool, job->elements[i + 1], evr, flags, 1);
      if (flags == REL_ARCH)
	job->elements[i] |= SOLVER_SETARCH;
      if (flags == REL_EQ && select == SOLVER_SOLVABLE_NAME && job->elements[i])
	{
#ifndef DEBIAN
	  const char *evrstr = pool_id2str(pool, evr);
	  if (!strchr(evrstr, '-'))
	    job->elements[i] |= SOLVER_SETEV;
	  else
#endif
	    job->elements[i] |= SOLVER_SETEVR;
	}
      /* make sure we still have matches */
      FOR_JOB_SELECT(p, pp, job->elements[i], job->elements[i + 1])
	break;
      if (p)
	{
	  job->elements[j] = job->elements[i];
	  job->elements[j + 1] = job->elements[i + 1];
	  j += 2;
	}
    }
  queue_truncate(job, j);
  return j / 2;
}

int
limitrelation_arch(Pool *pool, Queue *job, int flags, char *evr)
{
  char *r;
  Id archid;
  if ((r = strrchr(evr, '.')) != 0 && r[1] && (archid = str2archid(pool, r + 1)) != 0)
    {
      *r = 0;
      limitrelation(pool, job, REL_ARCH, archid);
      limitrelation(pool, job, flags, pool_str2id(pool, evr, 1));
      *r = '.';
    }
  else
    limitrelation(pool, job, flags, pool_str2id(pool, evr, 1));
  return job->count / 2;
}

int
limitrepo(Pool *pool, Id repofilter, Queue *job)
{
  Queue mq;
  Id p, pp;
  int i, j;
  Solvable *s;

  queue_init(&mq);
  for (i = j = 0; i < job->count; i += 2)
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
	  /* here we assume that repo == vendor, so we also set SOLVER_SETVENDOR */
	  Id flags = (job->elements[i] & SOLVER_SETMASK) | SOLVER_SETVENDOR | SOLVER_SETREPO;
	  if ((job->elements[i] & SOLVER_SELECTMASK) == SOLVER_SOLVABLE_NAME)
	    flags |= SOLVER_SETNAME;
	  if (mq.count == 1)
	    {
	      job->elements[j] = SOLVER_SOLVABLE | SOLVER_NOAUTOSET | flags;
	      job->elements[j + 1] = mq.elements[0];
	    }
	  else
	    {
	      job->elements[j] = SOLVER_SOLVABLE_ONE_OF | flags;
	      job->elements[j + 1] = pool_queuetowhatprovides(pool, &mq);
	    }
	  j += 2;
	}
    }
  queue_truncate(job, j);
  queue_free(&mq);
  return j / 2;
}

int
mkselect(Pool *pool, int mode, char *name, Queue *job)
{
  char *r, *r2;
  Id archid;

  if (*name == '/')
    {
      Dataiterator di;
      int type = strpbrk(name, "[*?") == 0 ? SEARCH_STRING : SEARCH_GLOB;
      Queue q;
      queue_init(&q);
      dataiterator_init(&di, pool, mode == SOLVER_ERASE ? pool->installed : 0, 0, SOLVABLE_FILELIST, name, type|SEARCH_FILES|SEARCH_COMPLETE_FILELIST);
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
	  if (q.count > 1)
	    queue_push2(job, SOLVER_SOLVABLE_ONE_OF, pool_queuetowhatprovides(pool, &q));
	  else
	    queue_push2(job, SOLVER_SOLVABLE | SOLVER_NOAUTOSET, q.elements[0]);
	  queue_free(&q);
	  return job->count / 2;
	}
    }
  if ((r = strpbrk(name, "<=>")) != 0)
    {
      /* relation case, support:
       * depglob rel
       * depglob.arch rel
       */
      int rflags = 0;
      int nend = r - name;
      char oldnend;
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
      if (!*name || !*r)
	{
	  fprintf(stderr, "bad relation\n");
	  exit(1);
	}
      oldnend = name[nend];
      name[nend] = 0;
      if (depglob(pool, name, job, DEPGLOB_NAMEDEP))
	{
	  name[nend] = oldnend;
	  limitrelation(pool, job, rflags, pool_str2id(pool, r, 1));
	  return job->count / 2;
	}
      if ((r2 = strrchr(name, '.')) != 0 && r2[1] && (archid = str2archid(pool, r2 + 1)) != 0)
	{
	  *r2 = 0;
	  if (depglob(pool, name, job, DEPGLOB_NAMEDEP))
	    {
	      name[nend] = oldnend;
	      *r2 = '.';
	      limitrelation(pool, job, REL_ARCH, archid);
	      limitrelation(pool, job, rflags, pool_str2id(pool, r, 1));
	      return job->count / 2;
	    }
	  *r2 = '.';
	}
      name[nend] = oldnend;
    }
  else
    {
      /* no relation case, support:
       * depglob
       * depglob.arch
       * nameglob-version
       * nameglob-version.arch
       * nameglob-version-release
       * nameglob-version-release.arch
       */
      if (depglob(pool, name, job, DEPGLOB_NAMEDEP))
	return job->count / 2;
      if ((r = strrchr(name, '.')) != 0 && r[1] && (archid = str2archid(pool, r + 1)) != 0)
	{
	  *r = 0;
	  if (depglob(pool, name, job, DEPGLOB_NAMEDEP))
	    {
	      *r = '.';
	      limitrelation(pool, job, REL_ARCH, archid);
	      return job->count / 2;
	    }
	  *r = '.';
	}
      if ((r = strrchr(name, '-')) != 0)
	{
	  *r = 0;
	  if (depglob(pool, name, job, DEPGLOB_NAME))
	    {
	      /* have just the version */
	      limitrelation_arch(pool, job, REL_EQ, r + 1);
	      *r = '-';
	      return job->count / 2;
	    }
	  if ((r2 = strrchr(name, '-')) != 0)
	    {
	      *r = '-';
	      *r2 = 0;
	      r = r2;
	      if (depglob(pool, name, job, DEPGLOB_NAME))
		{
		  /* have version-release */
		  limitrelation_arch(pool, job, REL_EQ, r + 1);
		  *r = '-';
		  return job->count / 2;
		}
	    }
	  *r = '-';
	}
    }
  return 0;
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

#if defined(ENABLE_RPMDB) && (defined(SUSE) || defined(FEDORA))

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
  return rpm_byfp(fp, pool_solvable2str(pool, s), &fcstate->rpmdbstate);
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

#endif

#if defined(ENABLE_DEBIAN) && defined(DEBIAN)

void
rundpkg(const char *arg, const char *name, int dupfd3)
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
      if (strcmp(arg, "--install") == 0)
        execlp("dpkg", "dpkg", "--install", "--force", "all", name, (char *)0);
      else
        execlp("dpkg", "dpkg", "--remove", "--force", "all", name, (char *)0);
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

#endif

static Id
nscallback(Pool *pool, void *data, Id name, Id evr)
{
  if (name == NAMESPACE_PRODUCTBUDDY)
    {
      /* SUSE specific hack: each product has an associated rpm */
      Solvable *s = pool->solvables + evr;
      Id p, pp, cap;
      Id bestp = 0;

      cap = pool_str2id(pool, pool_tmpjoin(pool, "product(", pool_id2str(pool, s->name) + 8, ")"), 0);
      if (!cap)
        return 0;
      cap = pool_rel2id(pool, cap, s->evr, REL_EQ, 0);
      if (!cap)
        return 0;
      FOR_PROVIDES(p, pp, cap)
        {
          Solvable *ps = pool->solvables + p;
          if (ps->repo == s->repo && ps->arch == s->arch)
            if (!bestp || pool_evrcmp(pool, pool->solvables[bestp].evr, ps->evr, EVRCMP_COMPARE) < 0)
	      bestp = p;
        }
      return bestp;
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
      id = pool_str2id(pool, bp, 1);
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
rewrite_repos(Pool *pool, Queue *addedfileprovides, Queue *addedfileprovides_inst)
{
  Repo *repo;
  Repodata *data;
  Map providedids;
  Queue fileprovidesq;
  int i, j, n;
  struct repoinfo *cinfo;

  map_init(&providedids, pool->ss.nstrings);
  queue_init(&fileprovidesq);
  for (i = 0; i < addedfileprovides->count; i++)
    MAPSET(&providedids, addedfileprovides->elements[i]);
  FOR_REPOS(i, repo)
    {
      /* make sure all repodatas but the first are extensions */
      if (repo->nrepodata < 2)
	continue;
      data = repo_id2repodata(repo, 1);
      if (data->loadcallback)
        continue;
      for (j = 2; j < repo->nrepodata; j++)
	{
	  Repodata *edata = repo_id2repodata(repo, j);
	  if (!edata->loadcallback)
	    break;
	}
      if (j < repo->nrepodata)
	continue;	/* found a non-externsion repodata, can't rewrite  */
      if (repodata_lookup_idarray(data, SOLVID_META, REPOSITORY_ADDEDFILEPROVIDES, &fileprovidesq))
	{
	  if (repo == pool->installed && addedfileprovides_inst)
	    {
	      for (j = 0; j < addedfileprovides->count; j++)
		MAPCLR(&providedids, addedfileprovides->elements[j]);
	      for (j = 0; j < addedfileprovides_inst->count; j++)
		MAPSET(&providedids, addedfileprovides_inst->elements[j]);
	    }
	  n = 0;
	  for (j = 0; j < fileprovidesq.count; j++)
	    if (MAPTST(&providedids, fileprovidesq.elements[j]))
	      n++;
	  if (repo == pool->installed && addedfileprovides_inst)
	    {
	      for (j = 0; j < addedfileprovides_inst->count; j++)
		MAPCLR(&providedids, addedfileprovides_inst->elements[j]);
	      for (j = 0; j < addedfileprovides->count; j++)
		MAPSET(&providedids, addedfileprovides->elements[j]);
	      if (n == addedfileprovides_inst->count)
		continue;	/* nothing new added */
	    }
	  else if (n == addedfileprovides->count)
	    continue;	/* nothing new added */
	}
      repodata_set_idarray(data, SOLVID_META, REPOSITORY_ADDEDFILEPROVIDES, repo == pool->installed && addedfileprovides_inst ? addedfileprovides_inst : addedfileprovides);
      repodata_internalize(data);
      cinfo = repo->appdata;
      writecachedrepo(repo, data, 0, cinfo ? cinfo->cookie : installedcookie);
    }
  queue_free(&fileprovidesq);
  map_free(&providedids);
}

static void
select_patches(Pool *pool, Queue *job)
{
  Id p, pp;
  int pruneyou = 0;
  Map installedmap, noobsmap;
  Solvable *s;

  map_init(&noobsmap, 0);
  map_init(&installedmap, pool->nsolvables);
  solver_calculate_noobsmap(pool, job, &noobsmap);
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
      if (strncmp(pool_id2str(pool, s->name), "patch:", 6) != 0)
	continue;
      FOR_PROVIDES(p2, pp, s->name)
	{
	  Solvable *s2 = pool->solvables + p2;
	  if (s2->name != s->name)
	    continue;
	  r = pool_evrcmp(pool, s->evr, s2->evr, EVRCMP_COMPARE);
	  if (r < 0 || (r == 0 && p > p2))
	    break;
	}
      if (p2)
	continue;
      type = solvable_lookup_str(s, SOLVABLE_PATCHCATEGORY);
      if (type && !strcmp(type, "optional"))
	continue;
      r = solvable_trivial_installable_map(s, &installedmap, 0, &noobsmap);
      if (r == -1)
	continue;
      if (solvable_lookup_bool(s, UPDATE_RESTART) && r == 0)
	{
	  if (!pruneyou++)
	    queue_empty(job);
	}
      else if (pruneyou)
	continue;
      queue_push2(job, SOLVER_SOLVABLE, p);
    }
  map_free(&installedmap);
  map_free(&noobsmap);
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
  Queue addedfileprovides;
  Queue addedfileprovides_inst;
  Id repofilter = 0;
  int cleandeps = 0;

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

  if (argc > 1 && !strcmp(argv[1], "--clean"))
    {
      cleandeps = 1;
      argc--;
      argv++;
    }

  pool = pool_create();

#if 0
  {
    const char *langs[] = {"de_DE", "de", "en"};
    pool_set_languages(pool, langs, sizeof(langs)/sizeof(*langs));
  }
#endif

  pool_setloadcallback(pool, load_stub, 0);
  pool->nscallback = nscallback;
  // pool_setdebuglevel(pool, 2);
  setarch(pool);
  repoinfos = read_repoinfos(pool, &nrepoinfos);

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
	  printf("  - %s: %s\n", pool_solvable2str(pool, s), solvable_lookup_str(s, SOLVABLE_SUMMARY));
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
#if defined(ENABLE_RPMDB) && (defined(SUSE) || defined(FEDORA))
	  if (l <= 4 || strcmp(argv[i] + l - 4, ".rpm"))
	    continue;
#endif
#if defined(ENABLE_DEBIAN) && defined(DEBIAN)
	  if (l <= 4 || strcmp(argv[i] + l - 4, ".deb"))
	    continue;
#endif
	  if (access(argv[i], R_OK))
	    {
	      perror(argv[i]);
	      exit(1);
	    }
	  if (!commandlinepkgs)
	    commandlinepkgs = solv_calloc(argc, sizeof(Id));
	  if (!commandlinerepo)
	    commandlinerepo = repo_create(pool, "@commandline");
	  p = 0;
#if defined(ENABLE_RPMDB) && (defined(SUSE) || defined(FEDORA))
	  p = repo_add_rpm(commandlinerepo, (const char *)argv[i], REPO_REUSE_REPODATA|REPO_NO_INTERNALIZE);
#endif
#if defined(ENABLE_DEBIAN) && defined(DEBIAN)
	  p = repo_add_deb(commandlinerepo, (const char *)argv[i], REPO_REUSE_REPODATA|REPO_NO_INTERNALIZE);
#endif
	  if (!p)
	    {
	      fprintf(stderr, "could not add '%s'\n", argv[i]);
	      exit(1);
	    }
	  commandlinepkgs[i] = p;
	}
      if (commandlinerepo)
	repo_internalize(commandlinerepo);
    }

  // FOR_REPOS(i, repo)
  //   printf("%s: %d solvables\n", repo->name, repo->nsolvables);
  queue_init(&addedfileprovides);
  queue_init(&addedfileprovides_inst);
  pool_addfileprovides_queue(pool, &addedfileprovides, &addedfileprovides_inst);
  if (addedfileprovides.count || addedfileprovides_inst.count)
    rewrite_repos(pool, &addedfileprovides, &addedfileprovides_inst);
  queue_free(&addedfileprovides);
  queue_free(&addedfileprovides_inst);
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
      if (!mkselect(pool, mode, argv[i], &job2))
	{
	  fprintf(stderr, "nothing matches '%s'\n", argv[i]);
	  exit(1);
	}
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
	  Id how = job.elements[i] & SOLVER_SELECTMASK;
	  FOR_JOB_SELECT(p, pp, how, job.elements[i + 1])
	    {
	      Solvable *s = pool_id2solvable(pool, p);
	      if (mainmode == MODE_INFO)
		{
		  const char *str;
		  printf("Name:        %s\n", pool_solvable2str(pool, s));
		  printf("Repo:        %s\n", s->repo->name);
		  printf("Summary:     %s\n", solvable_lookup_str(s, SOLVABLE_SUMMARY));
		  str = solvable_lookup_str(s, SOLVABLE_URL);
		  if (str)
		    printf("Url:         %s\n", str);
		  str = solvable_lookup_str(s, SOLVABLE_LICENSE);
		  if (str)
		    printf("License:     %s\n", str);
		  printf("Description:\n%s\n", solvable_lookup_str(s, SOLVABLE_DESCRIPTION));
		  printf("\n");
		}
	      else
		{
#if 1
		  const char *sum = solvable_lookup_str_lang(s, SOLVABLE_SUMMARY, "de", 1);
#else
		  const char *sum = solvable_lookup_str_poollang(s, SOLVABLE_SUMMARY);
#endif
		  printf("  - %s [%s]\n", pool_solvable2str(pool, s), s->repo->name);
		  if (sum)
		    printf("    %s\n", sum);
		}
	    }
	}
      queue_free(&job);
      pool_free(pool);
      free_repoinfos(repoinfos, nrepoinfos);
      solv_free(commandlinepkgs);
#ifdef FEDORA
      yum_substitute(pool, 0);
#endif
      exit(0);
    }

  if (mainmode == MODE_PATCH)
    select_patches(pool, &job);

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
	      if (cleandeps)
		job.elements[i] |= SOLVER_CLEANDEPS;
	      continue;
	    }
	}
      job.elements[i] |= mode;
      if (cleandeps)
        job.elements[i] |= SOLVER_CLEANDEPS;
    }

  if (mainmode == MODE_DISTUPGRADE && allpkgs)
    {
      if (repofilter)
        queue_push2(&job, SOLVER_DISTUPGRADE|SOLVER_SOLVABLE_REPO, repofilter);
      else
        queue_push2(&job, SOLVER_DISTUPGRADE|SOLVER_SOLVABLE_ALL, 0);
    }
  if (mainmode == MODE_UPDATE && allpkgs)
    {
      if (repofilter)
        queue_push2(&job, SOLVER_UPDATE|SOLVER_SOLVABLE_REPO, repofilter);
      else
        queue_push2(&job, SOLVER_UPDATE|SOLVER_SOLVABLE_ALL, 0);
    }
  if (mainmode == MODE_VERIFY && allpkgs)
    {
      if (repofilter)
        queue_push2(&job, SOLVER_VERIFY|SOLVER_SOLVABLE_REPO, repofilter);
      else
        queue_push2(&job, SOLVER_VERIFY|SOLVER_SOLVABLE_ALL, 0);
    }

  // multiversion test
  // queue_push2(&job, SOLVER_NOOBSOLETES|SOLVER_SOLVABLE_NAME, pool_str2id(pool, "kernel-pae", 1));
  // queue_push2(&job, SOLVER_NOOBSOLETES|SOLVER_SOLVABLE_NAME, pool_str2id(pool, "kernel-pae-base", 1));
  // queue_push2(&job, SOLVER_NOOBSOLETES|SOLVER_SOLVABLE_NAME, pool_str2id(pool, "kernel-pae-extra", 1));

#ifdef SOFTLOCKS_PATH
  addsoftlocks(pool, &job);
#endif

#if defined(ENABLE_RPMDB) && (defined(SUSE) || defined(FEDORA))
rerunsolver:
#endif
  for (;;)
    {
      Id problem, solution;
      int pcnt, scnt;

      solv = solver_create(pool);
      solver_set_flag(solv, SOLVER_FLAG_SPLITPROVIDES, 1);
      if (mainmode == MODE_ERASE)
        solver_set_flag(solv, SOLVER_FLAG_ALLOW_UNINSTALL, 1);	/* don't nag */

      if (!solver_solve(solv, &job))
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

  trans = solver_create_transaction(solv);
  if (!trans->steps.count)
    {
      printf("Nothing to do.\n");
      transaction_free(trans);
      solver_free(solv);
      queue_free(&job);
      pool_free(pool);
      free_repoinfos(repoinfos, nrepoinfos);
      solv_free(commandlinepkgs);
#ifdef FEDORA
      yum_substitute(pool, 0);
#endif
      exit(1);
    }
  printf("\n");
  printf("Transaction summary:\n\n");
  transaction_print(trans);

#if defined(SUSE)
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
      transaction_free(trans);
      solver_free(solv);
      queue_free(&job);
      pool_free(pool);
      free_repoinfos(repoinfos, nrepoinfos);
      solv_free(commandlinepkgs);
#ifdef FEDORA
      yum_substitute(pool, 0);
#endif
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
	  downloadsize += solvable_lookup_sizek(s, SOLVABLE_DOWNLOADSIZE, 0);
	}
      printf("Downloading %d packages, %d K\n", newpkgs, downloadsize);
      newpkgsfps = solv_calloc(newpkgs, sizeof(*newpkgsfps));
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
	      char *matchname = strdup(pool_id2str(pool, s->name));
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
		      const char *archstr;
		      FILE *fp;
		      char cmd[128];
		      int newfd;

		      archstr = pool_id2str(pool, s->arch);
		      if (strlen(archstr) > 10 || strchr(archstr, '\'') != 0)
			continue;

		      seqname = pool_lookup_str(pool, SOLVID_POS, DELTA_SEQ_NAME);
		      seqevr = pool_lookup_str(pool, SOLVID_POS, DELTA_SEQ_EVR);
		      seqnum = pool_lookup_str(pool, SOLVID_POS, DELTA_SEQ_NUM);
		      seq = pool_tmpjoin(pool, seqname, "-", seqevr);
		      seq = pool_tmpappend(pool, seq, "-", seqnum);
		      if (strchr(seq, '\'') != 0)
			continue;
#ifdef FEDORA
		      sprintf(cmd, "/usr/bin/applydeltarpm -a '%s' -c -s '", archstr);
#else
		      sprintf(cmd, "/usr/bin/applydeltarpm -c -s '");
#endif
		      if (system(pool_tmpjoin(pool, cmd, seq, "'")) != 0)
			continue;	/* didn't match */
		      /* looks good, download delta */
		      chksumtype = 0;
		      chksum = pool_lookup_bin_checksum(pool, SOLVID_POS, DELTA_CHECKSUM, &chksumtype);
		      if (!chksumtype)
			continue;	/* no way! */
		      dloc = pool_lookup_str(pool, SOLVID_POS, DELTA_LOCATION_DIR);
		      dloc = pool_tmpappend(pool, dloc, "/", pool_lookup_str(pool, SOLVID_POS, DELTA_LOCATION_NAME));
		      dloc = pool_tmpappend(pool, dloc, "-", pool_lookup_str(pool, SOLVID_POS, DELTA_LOCATION_EVR));
		      dloc = pool_tmpappend(pool, dloc, ".", pool_lookup_str(pool, SOLVID_POS, DELTA_LOCATION_SUFFIX));
		      if ((fp = curlfopen(cinfo, dloc, 0, chksum, chksumtype, 0)) == 0)
			continue;
		      /* got it, now reconstruct */
		      newfd = opentmpfile();
#ifdef FEDORA
		      sprintf(cmd, "applydeltarpm -a '%s' /dev/fd/%d /dev/fd/%d", archstr, fileno(fp), newfd);
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
	      solv_free(matchname);
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

#if defined(ENABLE_RPMDB) && (defined(SUSE) || defined(FEDORA))
  if (newpkgs)
    {
      Queue conflicts;
      struct fcstate fcstate;

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
	    printf("file %s of package %s conflicts with package %s\n", pool_id2str(pool, conflicts.elements[i]), pool_solvid2str(pool, conflicts.elements[i + 1]), pool_solvid2str(pool, conflicts.elements[i + 3]));
	  printf("\n");
	  if (yesno("Re-run solver (y/n/q)? "))
	    {
	      for (i = 0; i < newpkgs; i++)
		if (newpkgsfps[i])
		  fclose(newpkgsfps[i]);
	      newpkgsfps = solv_free(newpkgsfps);
	      solver_free(solv);
	      pool_add_fileconflicts_deps(pool, &conflicts);
	      pool_createwhatprovides(pool);	/* Hmm... */
	      goto rerunsolver;
	    }
	}
      queue_free(&conflicts);
    }
#endif

  printf("Committing transaction:\n\n");
  transaction_order(trans, 0);
  for (i = 0; i < trans->steps.count; i++)
    {
#if defined(ENABLE_RPMDB) && (defined(SUSE) || defined(FEDORA))
      const char *evr, *evrp, *nvra;
#endif
      Solvable *s;
      int j;
      FILE *fp;

      p = trans->steps.elements[i];
      s = pool_id2solvable(pool, p);
      Id type = transaction_type(trans, p, SOLVER_TRANSACTION_RPM_ONLY);
      switch(type)
	{
	case SOLVER_TRANSACTION_ERASE:
	  printf("erase %s\n", pool_solvid2str(pool, p));
#if defined(ENABLE_RPMDB) && (defined(SUSE) || defined(FEDORA))
	  if (!s->repo->rpmdbid || !s->repo->rpmdbid[p - s->repo->start])
	    continue;
	  /* strip epoch from evr */
	  evr = evrp = pool_id2str(pool, s->evr);
	  while (*evrp >= '0' && *evrp <= '9')
	    evrp++;
	  if (evrp > evr && evrp[0] == ':' && evrp[1])
	    evr = evrp + 1;
	  nvra = pool_tmpjoin(pool, pool_id2str(pool, s->name), "-", evr);
	  nvra = pool_tmpappend(pool, nvra, ".", pool_id2str(pool, s->arch));
	  runrpm("-e", nvra, -1);	/* too bad that --querybynumber doesn't work */
#endif
#if defined(ENABLE_DEBIAN) && defined(DEBIAN)
	  rundpkg("--remove", pool_id2str(pool, s->name), 0);
#endif
	  break;
	case SOLVER_TRANSACTION_INSTALL:
	case SOLVER_TRANSACTION_MULTIINSTALL:
	  printf("install %s\n", pool_solvid2str(pool, p));
	  for (j = 0; j < newpkgs; j++)
	    if (checkq.elements[j] == p)
	      break;
	  fp = j < newpkgs ? newpkgsfps[j] : 0;
	  if (!fp)
	    continue;
	  rewind(fp);
	  lseek(fileno(fp), 0, SEEK_SET);
#if defined(ENABLE_RPMDB) && (defined(SUSE) || defined(FEDORA))
	  runrpm(type == SOLVER_TRANSACTION_MULTIINSTALL ? "-i" : "-U", "/dev/fd/3", fileno(fp));
#endif
#if defined(ENABLE_DEBIAN) && defined(DEBIAN)
	  rundpkg("--install", "/dev/fd/3", fileno(fp));
#endif
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
  solv_free(newpkgsfps);
  queue_free(&checkq);
  transaction_free(trans);
  solver_free(solv);
  queue_free(&job);
  pool_free(pool);
  free_repoinfos(repoinfos, nrepoinfos);
  solv_free(commandlinepkgs);
#ifdef FEDORA
  yum_substitute(pool, 0);
#endif
  exit(0);
}
