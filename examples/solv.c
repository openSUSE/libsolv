/*
 * Copyright (c) 2009, Novell Inc.
 *
 * This program is licensed under the BSD license, read LICENSE.BSD
 * for further information
 */

/* solv, a little software installer demoing the sat solver library */

/* things missing:
 * - vendor policy loading
 * - soft locks file handling
 * - multi version handling
 */

#define _GNU_SOURCE

#include <stdio.h>
#include <stdlib.h>
#include <dirent.h>
#include <unistd.h>
#include <zlib.h>
#include <fcntl.h>
#include <assert.h>
#include <sys/utsname.h>
#include <sys/types.h>
#include <sys/wait.h>

#include "pool.h"
#include "poolarch.h"
#include "repo.h"
#include "evr.h"
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
#endif

#define SOLVCACHE_PATH "/var/cache/solv"


struct repoinfo {
  Repo *repo;

  char *alias;
  char *name;
  int enabled;
  int autorefresh;
  char *baseurl;
  char *metalink;
  char *path;
  int type;
  int gpgcheck;
  int priority;
  int keeppackages;
};

#ifdef FEDORA
char *
yum_substitute(Pool *pool, char *line)
{
  char *p, *p2;

  p = line;
  while ((p2 = strchr(p, '$')) != 0)
    {
      if (!strncmp(p2, "$releasever", 11))
	{
	  static char *releaseevr;
	  if (!releaseevr)
	    {
	      FILE *fp;
	      char buf[1024], *bp;

	      fp = popen("rpm --nodigest --nosignature -q --qf '%{VERSION}\n' --whatprovides redhat-release", "r");
	      fread(buf, 1, sizeof(buf), fp);
	      fclose(fp);
	      bp = buf;
	      while (*bp != ' ' && *bp != '\t' && *bp != '\n')
		bp++;
	      *bp = 0;
	      releaseevr = strdup(buf);
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
	  static char *basearch;
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
	  while (buf2[l - 1] == '\n' || buf2[l - 1] == ' ' || buf2[l - 1] == '\t')
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
	    cinfo->gpgcheck = *vp == '0' ? 0 : 1;
	  else if (!strcmp(kp, "baseurl"))
	    cinfo->baseurl = strdup(vp);
	  else if (!strcmp(kp, "mirrorlist"))
	    {
	      if (strstr(vp, "metalink"))
	        cinfo->metalink = strdup(vp);
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
      return 0;
    }
  return 1;
}

char *
findmetalinkurl(FILE *fp)
{
  char buf[4096], *bp, *ep;
  while((bp = fgets(buf, sizeof(buf), fp)) != 0)
    {
      while (*bp == ' ' || *bp == '\t')
	bp++;
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
      return strdup(bp);
    }
  return 0;
}

FILE *
curlfopen(struct repoinfo *cinfo, const char *file, int uncompress, const unsigned char *chksum, Id chksumtype)
{
  pid_t pid;
  int fd, l;
  int status;
  char url[4096];
  const char *baseurl = cinfo->baseurl;

  if (!baseurl)
    {
      if (!cinfo->metalink)
        return 0;
      if (file != cinfo->metalink)
	{
	  FILE *fp = curlfopen(cinfo, cinfo->metalink, 0, 0, 0);
	  if (!fp)
	    return 0;
	  cinfo->baseurl = findmetalinkurl(fp);
	  fclose(fp);
	  if (!cinfo->baseurl)
	    return 0;
	  return curlfopen(cinfo, file, uncompress, chksum, chksumtype);
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
  while (waitpid(pid, &status, 0) != pid)
    ;
  if (lseek(fd, 0, SEEK_END) == 0)
    {
      /* empty file */
      close(fd);
      return 0;
    }
  lseek(fd, 0, SEEK_SET);
  if (chksumtype && !verify_checksum(fd, file, chksum, chksumtype))
    {
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
	return 0;
      memset(&cio, 0, sizeof(cio));
      cio.read = cookie_gzread;
      cio.close = cookie_gzclose;
      return fopencookie(gzf, "r", cio);
    }
  fcntl(fd, F_SETFD, FD_CLOEXEC);
  return fdopen(fd, "r");
}

void
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

char *calccachepath(Repo *repo)
{
  char *q, *p = pool_tmpjoin(repo->pool, SOLVCACHE_PATH, "/", repo->name);
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
usecachedrepo(Repo *repo, unsigned char *cookie)
{
  FILE *fp;
  unsigned char mycookie[32];

  if (!(fp = fopen(calccachepath(repo), "r")))
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
  rewind(fp);
  if (repo_add_solv(repo, fp))
    {
      fclose(fp);
      return 0;
    }
  fclose(fp);
  return 1;
}

void
writecachedrepo(Repo *repo, unsigned char *cookie)
{
  Id *addedfileprovides = 0;
  FILE *fp;
  int i, fd;
  char *tmpl;
  Repodata *info;

  mkdir(SOLVCACHE_PATH, 0755);
  tmpl = sat_dupjoin(SOLVCACHE_PATH, "/", ".newsolv-XXXXXX");
  fd = mkstemp(tmpl);
  if (!fd)
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
  info = repo_add_repodata(repo, 0);
  pool_addfileprovides_ids(repo->pool, 0, &addedfileprovides);
  if (addedfileprovides && *addedfileprovides)
    {
      for (i = 0; addedfileprovides[i]; i++)
	repodata_add_idarray(info, SOLVID_META, REPOSITORY_ADDEDFILEPROVIDES, addedfileprovides[i]);
    }
  sat_free(addedfileprovides);
  repodata_internalize(info);
  repo_write(repo, fp, 0, 0, 0);
  repodata_free(info);
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
  if (!rename(tmpl, calccachepath(repo)))
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

static inline int
iscompressed(const char *name)
{
  int l = strlen(name);
  return l > 3 && !strcmp(name + l - 3, ".gz") ? 1 : 0;
}

static inline const char *
findinrepomd(Repo *repo, const char *what, const unsigned char **chksump, Id *chksumtypep)
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

void
read_repos(Pool *pool, struct repoinfo *repoinfos, int nrepoinfos)
{
  Repo *repo;
  struct repoinfo *cinfo;
  int i;
  FILE *fp;
  FILE *sigfp;
  Dataiterator di;
  const char *filename;
  const unsigned char *filechksum;
  Id filechksumtype;
  const char *descrdir;
  int defvendor;
  struct stat stb;
  unsigned char cookie[32];
  Pool *sigpool = 0;

  repo = repo_create(pool, "@System");
  printf("rpm database:");
  if (stat("/var/lib/rpm/Packages", &stb))
    memset(&stb, 0, sizeof(&stb));
  calc_checksum_stat(&stb, REPOKEY_TYPE_SHA256, cookie);
  if (usecachedrepo(repo, cookie))
    printf(" cached\n");
  else
    {
      FILE *ofp;
      printf(" reading\n");
      int done = 0;

#ifdef PRODUCTS_PATH
      repo_add_products(repo, PRODUCTS_PATH, 0, REPO_NO_INTERNALIZE);
#endif
      if ((ofp = fopen(calccachepath(repo), "r")) != 0)
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
      writecachedrepo(repo, cookie);
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

      if (!cinfo->autorefresh && usecachedrepo(repo, 0))
	{
	  printf("repo '%s':", cinfo->alias);
	  printf(" cached\n");
	  continue;
	}
      switch (cinfo->type)
	{
        case TYPE_RPMMD:
	  printf("rpmmd repo '%s':", cinfo->alias);
	  fflush(stdout);
	  if ((fp = curlfopen(cinfo, "repodata/repomd.xml", 0, 0, 0)) == 0)
	    {
	      printf(" no repomd.xml file, skipped\n");
	      repo_free(repo, 1);
	      cinfo->repo = 0;
	      break;
	    }
	  calc_checksum_fp(fp, REPOKEY_TYPE_SHA256, cookie);
	  if (usecachedrepo(repo, cookie))
	    {
	      printf(" cached\n");
              fclose(fp);
	      break;
	    }
	  sigfp = curlfopen(cinfo, "repodata/repomd.xml.asc", 0, 0, 0);
#ifndef FEDORA
	  if (!sigfp)
	    {
	      printf(" unsigned, skipped\n");
	      fclose(fp);
	      break;
	    }
#endif
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
	  repo_add_repomdxml(repo, fp, 0);
	  fclose(fp);
	  printf(" reading\n");
	  filename = findinrepomd(repo, "primary", &filechksum, &filechksumtype);
	  if (filename && (fp = curlfopen(cinfo, filename, iscompressed(filename), filechksum, filechksumtype)) != 0)
	    {
	      repo_add_rpmmd(repo, fp, 0, 0);
	      fclose(fp);
	    }

	  filename = findinrepomd(repo, "updateinfo", &filechksum, &filechksumtype);
	  if (filename && (fp = curlfopen(cinfo, filename, iscompressed(filename), filechksum, filechksumtype)) != 0)
	    {
	      repo_add_updateinfoxml(repo, fp, 0);
	      fclose(fp);
	    }

	  filename = findinrepomd(repo, "deltainfo", &filechksum, &filechksumtype);
	  if (!filename)
	    filename = findinrepomd(repo, "prestodelta", &filechksum, &filechksumtype);
	  if (filename && (fp = curlfopen(cinfo, filename, iscompressed(filename), filechksum, filechksumtype)) != 0)
	    {
	      repo_add_deltainfoxml(repo, fp, 0);
	      fclose(fp);
	    }
	  
	  writecachedrepo(repo, cookie);
	  break;

        case TYPE_SUSETAGS:
	  printf("susetags repo '%s':", cinfo->alias);
	  fflush(stdout);
	  repo = repo_create(pool, cinfo->alias);
	  cinfo->repo = repo;
	  repo->appdata = cinfo;
	  repo->priority = 99 - cinfo->priority;
	  descrdir = 0;
	  defvendor = 0;
	  if ((fp = curlfopen(cinfo, "content", 0, 0, 0)) == 0)
	    {
	      printf(" no content file, skipped\n");
	      repo_free(repo, 1);
	      cinfo->repo = 0;
	      break;
	    }
	  calc_checksum_fp(fp, REPOKEY_TYPE_SHA256, cookie);
	  if (usecachedrepo(repo, cookie))
	    {
	      printf(" cached\n");
	      fclose(fp);
	      break;
	    }
	  sigfp = curlfopen(cinfo, "content.asc", 0, 0, 0);
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
	  repo_add_content(repo, fp, 0);
	  fclose(fp);
	  defvendor = repo_lookup_id(repo, SOLVID_META, SUSETAGS_DEFAULTVENDOR);
	  descrdir = repo_lookup_str(repo, SOLVID_META, SUSETAGS_DESCRDIR);
	  if (!descrdir)
	    descrdir = "suse/setup/descr";
	  filename = 0;
	  dataiterator_init(&di, pool, repo, SOLVID_META, SUSETAGS_FILE_NAME, "packages.gz", SEARCH_STRING);
	  dataiterator_prepend_keyname(&di, SUSETAGS_FILE);
	  if (dataiterator_step(&di))
	    {
	      dataiterator_setpos_parent(&di);
	      filechksum = pool_lookup_bin_checksum(pool, SOLVID_POS, SUSETAGS_FILE_CHECKSUM, &filechksumtype);
	      filename = "packages.gz";
	    }
	  dataiterator_free(&di);
	  if (!filename)
	    {
	      dataiterator_init(&di, pool, repo, SOLVID_META, SUSETAGS_FILE_NAME, "packages", SEARCH_STRING);
	      dataiterator_prepend_keyname(&di, SUSETAGS_FILE);
	      if (dataiterator_step(&di))
		{
		  dataiterator_setpos_parent(&di);
		  filechksum = pool_lookup_bin_checksum(pool, SOLVID_POS, SUSETAGS_FILE_CHECKSUM, &filechksumtype);
		  filename = "packages";
		}
	      dataiterator_free(&di);
	    }
	  if (!filename)
	    {
	      printf(" no packages file entry, skipped\n");
	      break;
	    }
	  if (!filechksumtype)
	    {
	      printf(" no packages file checksum, skipped\n");
	      break;
	    }
	  printf(" reading\n");
	  if ((fp = curlfopen(cinfo, pool_tmpjoin(pool, descrdir, "/", filename), iscompressed(filename), filechksum, filechksumtype)) == 0)
	    break;
	  repo_add_susetags(repo, fp, defvendor, 0, 0);
	  fclose(fp);
	  writecachedrepo(repo, cookie);
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

void
mkselect(Pool *pool, const char *arg, int flags, Queue *out)
{
  Id id, p, pp;
  Id type = 0;
  const char *r, *r2;

  id = str2id(pool, arg, 0);
  if (id)
    {
      FOR_PROVIDES(p, pp, id)
	{
	  Solvable *s = pool_id2solvable(pool, p);
	  if (s->name == id)
	    {
	      type = SOLVER_SOLVABLE_NAME;
	      break;
	    }
	  type = SOLVER_SOLVABLE_PROVIDES;
	}
    }
  if (!type)
    {
      /* did not find a solvable, see if it's a relation */
      if ((r = strpbrk(arg, "<=>")) != 0)
	{
	  Id rid, rname, revr;
	  int rflags = 0;
	  for (r2 = r; r2 > arg && (r2[-1] == ' ' || r2[-1] == '\t'); )
	    r2--;
	  rname = r2 > arg ? strn2id(pool, arg, r2 - arg, 1) : 0;
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
	  while (*r == ' ' || *r == '\t')
	    r++;
	  revr = *r ? str2id(pool, r, 1) : 0;
	  rid = rname && revr ? rel2id(pool, rname, revr, rflags, 1) : 0;
	  if (rid)
	    {
	      FOR_PROVIDES(p, pp, rid)
		{
		  Solvable *s = pool_id2solvable(pool, p);
		  if (pool_match_nevr(pool, s, rid))
		    {
		      type = SOLVER_SOLVABLE_NAME;
		      break;
		    }
		  type = SOLVER_SOLVABLE_PROVIDES;
		}
	    }
	  if (type)
	    id = rid;
	}
    }
  if (type)
    {
      queue_push(out, type);
      queue_push(out, id);
    }
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
fc_cb(Pool *pool, Id p, void *cbdata)
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


int
main(int argc, char **argv)
{
  Pool *pool;
  Id p, pp;
  struct repoinfo *repoinfos;
  int nrepoinfos = 0;
  int i, mode, newpkgs;
  Queue job, checkq;
  Solver *solv = 0;
  Transaction *trans;
  char inbuf[128], *ip;
  int updateall = 0;
  int distupgrade = 0;
  int patchmode = 0;
  FILE **newpkgsfps;
  struct fcstate fcstate;

  argc--;
  argv++;
  if (!argv[0])
    {
      fprintf(stderr, "Usage: solv install|erase|update|show <select>\n");
      exit(1);
    }
  if (!strcmp(argv[0], "install") || !strcmp(argv[0], "in"))
    mode = SOLVER_INSTALL;
  else if (!strcmp(argv[0], "patch"))
    {
      mode = SOLVER_UPDATE;
      patchmode = 1;
    }
  else if (!strcmp(argv[0], "erase") || !strcmp(argv[0], "rm"))
    mode = SOLVER_ERASE;
  else if (!strcmp(argv[0], "show"))
    mode = 0;
  else if (!strcmp(argv[0], "update") || !strcmp(argv[0], "up"))
    mode = SOLVER_UPDATE;
  else if (!strcmp(argv[0], "dist-upgrade") || !strcmp(argv[0], "dup"))
    {
      mode = SOLVER_UPDATE;
      distupgrade = 1;
    }
  else
    {
      fprintf(stderr, "Usage: solv install|erase|update|show <select>\n");
      exit(1);
    }

  pool = pool_create();
  pool->nscallback = nscallback;
  // pool_setdebuglevel(pool, 2);
  setarch(pool);
  repoinfos = read_repoinfos(pool, REPOINFO_PATH, &nrepoinfos);
  read_repos(pool, repoinfos, nrepoinfos);
  // FOR_REPOS(i, repo)
  //   printf("%s: %d solvables\n", repo->name, repo->nsolvables);
  pool_addfileprovides(pool);
  pool_createwhatprovides(pool);

  queue_init(&job);
  for (i = 1; i < argc; i++)
    mkselect(pool, argv[i], 0, &job);
  if (!job.count && mode == SOLVER_UPDATE)
    updateall = 1;
  else if (!job.count)
    {
      printf("no package matched\n");
      exit(1);
    }

  if (!mode)
    {
      /* show mode, no solver needed */
      for (i = 0; i < job.count; i += 2)
	{
	  FOR_JOB_SELECT(p, pp, job.elements[i], job.elements[i + 1])
	    {
	      Solvable *s = pool_id2solvable(pool, p);
	      printf("  - %s [%s]\n", solvable2str(pool, s), s->repo->name);
	    }
	}
      exit(0);
    }

  if (updateall && patchmode)
    {
      int pruneyou = 0;
      Map installedmap;
      Solvable *s;

      map_init(&installedmap, pool->nsolvables);
      if (pool->installed)
        FOR_REPO_SOLVABLES(pool->installed, p, s)
	  MAPSET(&installedmap, p);

      /* install all patches */
      updateall = 0;
      mode = SOLVER_INSTALL;
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
    job.elements[i] |= mode;

  // multiversion test
  // queue_push2(&job, SOLVER_NOOBSOLETES|SOLVER_SOLVABLE_NAME, str2id(pool, "kernel-pae", 1));
  // queue_push2(&job, SOLVER_NOOBSOLETES|SOLVER_SOLVABLE_NAME, str2id(pool, "kernel-pae-base", 1));
  // queue_push2(&job, SOLVER_NOOBSOLETES|SOLVER_SOLVABLE_NAME, str2id(pool, "kernel-pae-extra", 1));

rerunsolver:
  for (;;)
    {
      Id problem, solution;
      int pcnt, scnt;

      solv = solver_create(pool);
      solv->ignorealreadyrecommended = 1;
      solv->updatesystem = updateall;
      solv->dosplitprovides = updateall;
      if (updateall && distupgrade)
	{
	  solv->distupgrade = 1;
          solv->allowdowngrade = 1;
          solv->allowarchchange = 1;
          solv->allowvendorchange = 1;
	}
      // queue_push2(&job, SOLVER_DISTUPGRADE, 3);
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
  if (!solv->trans.steps.count)
    {
      printf("Nothing to do.\n");
      exit(1);
    }
  printf("\n");
  printf("Transaction summary:\n\n");
  solver_printtransaction(solv);
  if (!yesno("OK to continue (y/n)? "))
    {
      printf("Abort.\n");
      exit(1);
    }

  trans = &solv->trans;
  queue_init(&checkq);
  newpkgs = transaction_installedresult(trans, &checkq);
  newpkgsfps = 0;

  if (newpkgs)
    {
      printf("Downloading %d packages\n", newpkgs);
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
	      dataiterator_init(&di, pool, 0, SOLVID_META, DELTA_PACKAGE_NAME, id2str(pool, s->name), SEARCH_STRING);
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
		      if (system(pool_tmpjoin(pool, "/usr/bin/applydeltarpm -c -s ", seq, 0)) != 0)
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
		      if ((fp = curlfopen(cinfo, dloc, 0, chksum, chksumtype)) == 0)
			continue;
		      /* got it, now reconstruct */
		      newfd = opentmpfile();
		      sprintf(cmd, "applydeltarpm /dev/fd/%d /dev/fd/%d", fileno(fp), newfd);
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
	  if ((newpkgsfps[i] = curlfopen(cinfo, loc, 0, chksum, chksumtype)) == 0)
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
      pool_findfileconflicts(pool, &checkq, newpkgs, &conflicts, &fc_cb, &fcstate);
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
  exit(0);
}
