/*
 * Copyright (c) 2018, SUSE LLC.
 *
 * This program is licensed under the BSD license, read LICENSE.BSD
 * for further information
 */

#include <sys/types.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <dirent.h>
#include <sys/stat.h>
#include <sys/wait.h>
#include <errno.h>

#include "pool.h"
#include "repo.h"

#ifdef ENABLE_RPMPKG
#include "repo_rpmdb.h"
#endif

#ifdef ENABLE_RPMMD
#include "repo_repomdxml.h"
#include "repo_rpmmd.h"
#include "repo_updateinfoxml.h"
#include "repo_deltainfoxml.h"
#endif

#ifdef ENABLE_SUSEREPO
#include "repo_content.h"
#include "repo_susetags.h"
#endif

#ifdef SUSE
#include "repo_autopattern.h"
#endif
#ifdef ENABLE_APPDATA
#include "repo_appdata.h"
#endif
#include "common_write.h"
#include "solv_xfopen.h"


#ifdef SUSE
int add_auto = 0;
#endif
#ifdef ENABLE_APPDATA
int add_appdata = 0;
#endif
int recursive = 0;
int add_filelist = 0;
int add_changelog = 0;
int filtered_filelist = 0;


#define REPO_PLAINDIR		1
#define REPO_RPMMD		2
#define REPO_RPMMD_REPODATA	3
#define REPO_SUSETAGS		4

int
autodetect_repotype(Pool *pool, const char *dir)
{
  struct stat stb;
  char *tmp;
  FILE *fp;

  tmp = pool_tmpjoin(pool, dir, "/repomd.xml", 0);
  if (stat(tmp, &stb) == 0)
    return REPO_RPMMD;
  tmp = pool_tmpjoin(pool, dir, "/repodata/repomd.xml", 0);
  if (stat(tmp, &stb) == 0)
    return REPO_RPMMD_REPODATA;
  tmp = pool_tmpjoin(pool, dir, "/content", 0);
  if ((fp = fopen(tmp, "r")) != 0)
    {
      char buf[512], *descrdir = 0;
      while (fgets(buf, sizeof(buf), fp))
	{
	  int l = strlen(buf);
	  char *bp = buf;
	  if (buf[l - 1] != '\n')
	    {
	      int c;
	      while ((c = getc(fp)) != EOF && c != '\n')
		;
	      continue;
	    }
	  while (l && (buf[l - 1] == '\n' || buf[l - 1] == ' ' || buf[l - 1] == '\t'))
	    l--;
	  buf[l] = 0;
	  while (*bp == ' ' || *bp == '\t')
	    bp++;
	  if (strncmp(bp, "DESCRDIR", 8) != 0 || (bp[8] != ' ' && bp[8] != '\t'))
	    continue;
	  bp += 9;
	  while (*bp == ' ' || *bp == '\t')
	    bp++;
	  descrdir = bp;
	  break;
	}
      fclose(fp);
      if (descrdir)
	{
	  tmp = pool_tmpjoin(pool, dir, "/", descrdir);
	  if (stat(tmp, &stb) == 0 && S_ISDIR(stb.st_mode))
	    return REPO_SUSETAGS;
	}
    }
  tmp = pool_tmpjoin(pool, dir, "/suse/setup/descr", 0);
  if (stat(tmp, &stb) == 0 && S_ISDIR(stb.st_mode))
    return REPO_SUSETAGS;
  return REPO_PLAINDIR;
}


#ifdef ENABLE_RPMPKG

int
read_plaindir_repo(Repo *repo, const char *dir)
{
  Pool *pool = repo->pool;
  Repodata *data;
  int c;
  FILE *fp;
  int wstatus;
  int fds[2];
  pid_t pid;
  char *buf = 0;
  char *buf_end = 0;
  char *bp = 0;
  char *rpm;
  int res = 0;
  Id p;

  /* run find command */
  if (pipe(fds))
    {
      perror("pipe");
      exit(1);
    }
  while ((pid = fork()) == (pid_t)-1)
    {
      if (errno != EAGAIN)
	{
	  perror("fork");
	  exit(1);
	}
      sleep(3);
    }
  if (pid == 0)
    {
      if (chdir(dir))
	{
	  perror(dir);
	  _exit(1);
	}
      close(fds[0]);
      if (fds[1] != 1)
	{
	  if (dup2(fds[1], 1) == -1)
	    {
	      perror("dup2");
	      _exit(1);
	    }
	  close(fds[1]);
	}
      if (recursive)
	execl("/usr/bin/find", "/usr/bin/find", ".", "-name", ".", "-o", "-name", ".*", "-prune", "-o", "-name", "*.delta.rpm", "-o", "-name", "*.patch.rpm", "-o", "-name", "*.rpm", "-a", "-type", "f", "-print0", (char *)0);
      else
	execl("/usr/bin/find", "/usr/bin/find", ".", "-maxdepth", "1", "-name", ".", "-o", "-name", ".*", "-prune", "-o", "-name", "*.delta.rpm", "-o", "-name", "*.patch.rpm", "-o", "-name", "*.rpm", "-a", "-type", "f", "-print0", (char *)0);
      perror("/usr/bin/find");
      _exit(1);
    }
  close(fds[1]);
  if ((fp = fdopen(fds[0], "r")) == 0)
    {
      perror("fdopen");
      exit(1);
    }
  data = repo_add_repodata(repo, 0);
  bp = buf;
  while ((c = getc(fp)) != EOF)
    {
      if (bp == buf_end)
	{
	  size_t len = bp - buf;
	  buf = solv_realloc(buf, len + 4096);
	  bp = buf + len;
	  buf_end = bp + 4096;
	}
      *bp++ = c;
      if (c)
	continue;
      bp = buf;
      rpm = solv_dupjoin(dir, "/", bp[0] == '.' && bp[1] == '/' ? bp + 2 : bp);
      if ((p = repo_add_rpm(repo, rpm, REPO_REUSE_REPODATA|REPO_NO_INTERNALIZE|REPO_NO_LOCATION|(filtered_filelist ? RPM_ADD_FILTERED_FILELIST : 0))) == 0)
	{
	  fprintf(stderr, "%s: %s\n", rpm, pool_errstr(pool));
#if 0
	  res = 1;
#endif
	}
      else
	repodata_set_location(data, p, 0, 0, bp[0] == '.' && bp[1] == '/' ? bp + 2 : bp);
      solv_free(rpm);
    }
  solv_free(buf);
  fclose(fp);
  while (waitpid(pid, &wstatus, 0) == -1)
    {
      if (errno == EINTR)
	continue;
      perror("waitpid");
      exit(1);
    }
  if (wstatus)
    {
      fprintf(stderr, "find: exit status %d\n", (wstatus >> 8) | (wstatus & 255) << 8);
#if 0
      res = 1;
#endif
    }
  repo_internalize(repo);
  return res;
}

#else

int
read_plaindir_repo(Repo *repo, const char *dir)
{
  fprintf(stderr, "plaindir repo type is not supported\n");
  exit(1);
}

#endif

#ifdef ENABLE_SUSEREPO

static const char *
susetags_find(char **files, int nfiles, const char *what)
{
  int i;
  size_t l = strlen(what);
  for (i = 0; i < nfiles; i++)
    {
      char *fn = files[i];
      if (strncmp(fn, what, l) != 0)
	continue;
      if (fn[l] == 0)
	return fn;
      if (fn[l] != '.')
	continue;
      if (strchr(fn + l + 1, '.') != 0)
	continue;
      if (solv_xfopen_iscompressed(fn) <= 0)
	continue;
      return fn;
    }
  return 0;
}

static FILE *
susetags_open(const char *dir, const char *filename, char **tmpp, int missingok)
{
  FILE *fp;
  if (!filename)
    {
      *tmpp = 0;
      return 0;
    }
  *tmpp = solv_dupjoin(dir, "/", filename);
  if ((fp = solv_xfopen(*tmpp, "r")) == 0)
    {
      if (!missingok)
	{
	  perror(*tmpp);
	  exit(1);
	}
      *tmpp = solv_free(*tmpp);
      return 0;
    }
  return fp;
}

static void
susetags_extend(Repo *repo, const char *dir, char **files, int nfiles, char *what, Id defvendor, char *language, int missingok)
{
  const char *filename;
  FILE *fp;
  char *tmp;

  filename = susetags_find(files, nfiles, what);
  if (!filename)
    return;
  if ((fp = susetags_open(dir, filename, &tmp, missingok)) != 0)
    {
      if (repo_add_susetags(repo, fp, defvendor, language, REPO_EXTEND_SOLVABLES))
	{
	  fprintf(stderr, "%s: %s\n", tmp, pool_errstr(repo->pool));
	  exit(1);
	}
      fclose(fp);
      solv_free(tmp);
    }
}

static void
susetags_extend_languages(Repo *repo, const char *dir, char **files, int nfiles, Id defvendor, int missingok)
{
  int i;
  for (i = 0; i < nfiles; i++)
    {
      char *fn = files[i];
      char lang[64], *p;
      if (strncmp(fn, "packages.", 9) != 0)
	continue;
      if (strlen(fn + 9) + 1 >= sizeof(lang))
	continue;
      strncpy(lang, fn + 9, sizeof(lang) - 1);
      lang[sizeof(lang) - 1] = 0;
      p = strrchr(lang, '.');
      if (p)
	{
          if (solv_xfopen_iscompressed(lang) <= 0)
	    continue;
	  *p = 0;
	}
      if (strchr(lang, '.'))
	continue;
      if (!strcmp(lang, "en"))
	continue;	/* already did that one */
      if (!strcmp(lang, "DU"))
	continue;	/* disk usage */
      if (!strcmp(lang, "FL"))
	continue;	/* file list */
      if (!strcmp(lang, "DL"))
	continue;	/* deltas */
      susetags_extend(repo, dir, files, nfiles, fn, defvendor, lang, missingok);
    }
}

static int
susetags_dircmp(const void *ap, const void *bp, void *dp)
{
  return strcmp(*(const char **)ap, *(const char **)bp);
}

int
read_susetags_repo(Repo *repo, const char *dir)
{
  Pool *pool = repo->pool;
  const char *filename;
  char *ddir;
  char *tmp;
  FILE *fp;
  Id defvendor = 0;
  const char *descrdir = 0;
  char **files = 0;
  int nfiles = 0;
  DIR *dp;
  struct dirent *de;

  /* read content file */
  repo_add_repodata(repo, 0);
  tmp = solv_dupjoin(dir, "/content", 0);
  if ((fp = fopen(tmp, "r")) != 0)
    {
      if (repo_add_content(repo, fp, REPO_REUSE_REPODATA))
        {
	  fprintf(stderr, "%s: %s\n", tmp, pool_errstr(pool));
	  exit(1);
        }
      fclose(fp);
      descrdir = repo_lookup_str(repo, SOLVID_META, SUSETAGS_DESCRDIR);
      defvendor = repo_lookup_id(repo, SOLVID_META, SUSETAGS_DEFAULTVENDOR);
    }
  if (!descrdir)
    descrdir = "suse/setup/descr";
  tmp = solv_free(tmp);

  /* get content of descrdir directory */
  ddir = solv_dupjoin(dir, "/", descrdir);
  if ((dp = opendir(ddir)) == 0)
    {
      perror(ddir);
      exit(1);
    }
  while ((de = readdir(dp)) != 0)
    {
      if (de->d_name[0] == 0 || de->d_name[0] == '.')
	continue;
      files = solv_extend(files, nfiles, 1, sizeof(char *), 63);
      files[nfiles++] = solv_strdup(de->d_name);
    }
  closedir(dp);
  if (nfiles > 1)
    solv_sort(files, nfiles, sizeof(char *), susetags_dircmp, 0);
  
  /* add packages */
  filename = susetags_find(files, nfiles, "packages");
  if (filename && (fp = susetags_open(ddir, filename, &tmp, 1)) != 0)
    {
      if (repo_add_susetags(repo, fp, defvendor, 0, SUSETAGS_RECORD_SHARES))
	{
	  fprintf(stderr, "%s: %s\n", tmp, pool_errstr(pool));
	  exit(1);
	}
      fclose(fp);
      tmp = solv_free(tmp);

      /* now extend the packages */
      susetags_extend(repo, ddir, files, nfiles, "packages.DU", defvendor, 0, 1);
      susetags_extend(repo, ddir, files, nfiles, "packages.en", defvendor, 0, 1);
      susetags_extend_languages(repo, ddir, files, nfiles, defvendor, 1);
      if (add_filelist)
        susetags_extend(repo, ddir, files, nfiles, "packages.FL", defvendor, 0, 1);
    }

  /* add deltas */
  filename = susetags_find(files, nfiles, "packages.DL");
  if (filename && (fp = susetags_open(ddir, filename, &tmp, 1)) != 0)
    {
      if (repo_add_susetags(repo, fp, defvendor, 0, 0))
	{
	  fprintf(stderr, "%s: %s\n", tmp, pool_errstr(pool));
	  exit(1);
	}
      fclose(fp);
      tmp = solv_free(tmp);
    }

  /* add legacy patterns */
  tmp = solv_dupjoin(ddir, "/patterns", 0);
  if ((fp = fopen(tmp, "r")) != 0)
    {
      char pbuf[4096];

      repo_add_repodata(repo, 0);
      while (fgets(pbuf, sizeof(pbuf), fp))
	{
	  char *p;
	  FILE *pfp;
	  if (strchr(pbuf, '/') != 0)
	    continue;
	  if ((p = strchr(pbuf, '\n')) != 0)
	    *p = 0;
	  if (*pbuf == 0)
	    continue;
	  solv_free(tmp);
	  tmp = solv_dupjoin(ddir, "/", pbuf);
	  if ((pfp = solv_xfopen(tmp, "r")) != 0)
	    {
	      if (repo_add_susetags(repo, pfp, defvendor, 0, REPO_NO_INTERNALIZE|REPO_REUSE_REPODATA))
		{
		  fprintf(stderr, "%s: %s\n", tmp, pool_errstr(pool));
		  exit(1);
		}
	      fclose(pfp);
	    }
	}
      fclose(fp);
    }
  tmp = solv_free(tmp);
 
#ifdef ENABLE_APPDATA
  /* appdata */
  filename = add_appdata ? susetags_find(files, nfiles, "appdata.xml") : 0;
  if (filename && (fp = susetags_open(ddir, filename, &tmp, 1)) != 0)
    {
      if (repo_add_appdata(repo, fp, 0))
	{
	  fprintf(stderr, "%s: %s\n", tmp, pool_errstr(pool));
	  exit(1);
	}
      fclose(fp);
      tmp = solv_free(tmp);
    }
#endif

  while (nfiles > 0)
    solv_free(files[--nfiles]);
  solv_free(files);
  solv_free(ddir);
  repo_internalize(repo);
  return 0;
}

#else

int
read_susetags_repo(Repo *repo, const char *dir)
{
  fprintf(stderr, "susetags repo type is not supported\n");
  exit(1);
}

#endif


#ifdef ENABLE_RPMMD

# ifdef ENABLE_ZCHUNK_COMPRESSION

static int
repomd_exists(const char *dir, const char *filename)
{
  char *path;
  struct stat stb;
  int r;
  
  if (!filename)
    return 0;
  path = solv_dupjoin(dir, "/", filename);
  r = stat(path, &stb) == 0;
  solv_free(path);
  return r;
}

# endif

static const char *
repomd_find(Repo *repo, const char *dir, const char *what, int findzchunk)
{
  Pool *pool = repo->pool;
  Dataiterator di;
  const char *filename;

# ifdef ENABLE_ZCHUNK_COMPRESSION
  if (findzchunk)
    {
      char *what_zck = solv_dupjoin(what, "_zck", 0);
      filename = repomd_find(repo, dir, what_zck, 0);
      solv_free(what_zck);
      if (filename && repomd_exists(dir, filename))
	return filename;
    }
# endif
  filename = 0;
  dataiterator_init(&di, pool, repo, SOLVID_META, REPOSITORY_REPOMD_TYPE, what, SEARCH_STRING);
  dataiterator_prepend_keyname(&di, REPOSITORY_REPOMD);
  if (dataiterator_step(&di))
    {
      dataiterator_setpos_parent(&di);
      filename = pool_lookup_str(pool, SOLVID_POS, REPOSITORY_REPOMD_LOCATION);
    }
  dataiterator_free(&di);
  if (filename && strncmp(filename, "repodata/", 9) == 0)
    filename += 9;
  return filename;
}

static FILE *
repomd_open(const char *dir, const char *filename, char **tmpp, int missingok)
{
  FILE *fp;
  if (!filename)
    {
      *tmpp = 0;
      return 0;
    }
  *tmpp = solv_dupjoin(dir, "/", filename);
  if ((fp = solv_xfopen(*tmpp, "r")) == 0)
    {
      if (!missingok)
	{
	  perror(*tmpp);
	  exit(1);
	}
      *tmpp = solv_free(*tmpp);
      return 0;
    }
  return fp;
}

static void
repomd_extend(Repo *repo, const char *dir, const char *what, const char *language, int missingok)
{
  const char *filename;
  FILE *fp;
  char *tmp;

  filename = repomd_find(repo, dir, what, 1);
  if (!filename)
    return;
  fp = repomd_open(dir, filename, &tmp, missingok);
  if (fp)
    {
      if (repo_add_rpmmd(repo, fp, language, REPO_EXTEND_SOLVABLES))
	{
	  fprintf(stderr, "%s: %s\n", tmp, pool_errstr(repo->pool));
	  exit(1);
	}
      fclose(fp);
    }
  solv_free(tmp);
}

static void
repomd_extend_languages(Repo *repo, const char *dir, int missingok)
{
  char **susedatas = 0;
  int nsusedatas = 0, i;
  Dataiterator di;
  dataiterator_init(&di, repo->pool, repo, SOLVID_META, REPOSITORY_REPOMD_TYPE, "susedata.", SEARCH_STRINGSTART);
  dataiterator_prepend_keyname(&di, REPOSITORY_REPOMD);
  while (dataiterator_step(&di))
    {
      char *str = solv_strdup(di.kv.str);
      size_t l = strlen(str);
      if (l > 4 && !strcmp(str + l - 4, "_zck"))
	str[l - 4] = 0;
      for (i = 0; i < nsusedatas; i++)
        if (!strcmp(susedatas[i], str))
	  break;
      if (i < nsusedatas)
	{
	  solv_free(str);
	  continue;	/* already have that entry */
	}
      susedatas = solv_extend(susedatas, nsusedatas, 1, sizeof(char *), 15);
      susedatas[nsusedatas++] = str;
    }
  dataiterator_free(&di);
  for (i = 0; i < nsusedatas; i++)
    {
      repomd_extend(repo, dir, susedatas[i], susedatas[i] + 9, missingok);
      susedatas[i] = solv_free(susedatas[i]);
    }
  solv_free(susedatas);
}

static void
add_rpmmd_file(Repo *repo, const char *dir, const char *filename, int missingok)
{
  FILE *fp;
  char *tmp;

  fp = repomd_open(dir, filename, &tmp, missingok);
  if (!fp)
    return;
  if (repo_add_rpmmd(repo, fp, 0, 0))
    {
      fprintf(stderr, "%s: %s\n", tmp, pool_errstr(repo->pool));
      exit(1);
    }
  fclose(fp);
  solv_free(tmp);
}

int
read_rpmmd_repo(Repo *repo, const char *dir)
{
  Pool *pool = repo->pool;
  FILE *fp;
  char *tmp = 0;
  const char *filename;

  /* add repomd.xml and suseinfo.xml */
  fp = repomd_open(dir, "repomd.xml", &tmp, 0);
  if (repo_add_repomdxml(repo, fp, 0))
    {
      fprintf(stderr, "%s: %s\n", tmp, pool_errstr(pool));
      exit(1);
    }
  fclose(fp);
  tmp = solv_free(tmp);
  filename = repomd_find(repo, dir, "suseinfo", 0);
  if (filename && (fp = repomd_open(dir, filename, &tmp, 0)) != 0)
    {
      if (repo_add_repomdxml(repo, fp, REPO_REUSE_REPODATA))
	{
	  fprintf(stderr, "%s: %s\n", tmp, pool_errstr(pool));
	  exit(1);
	}
      fclose(fp);
      tmp = solv_free(tmp);
    }
  
  /* first all primary packages */
  filename = repomd_find(repo, dir, "primary", 1);
  if (filename)
    {
      add_rpmmd_file(repo, dir, filename, 0);
      repomd_extend(repo, dir, "susedata", 0, 1);
      repomd_extend_languages(repo, dir, 1);
      if (add_filelist)
        repomd_extend(repo, dir, "filelists", 0, 1);
      if (add_changelog)
        repomd_extend(repo, dir, "other", 0, 1);
    }

  /* some legacy stuff */
  filename = repomd_find(repo, dir, "products", 0);
  if (!filename)
    filename = repomd_find(repo, dir, "product", 0);
  if (filename)
    add_rpmmd_file(repo, dir, filename, 1);
  filename = repomd_find(repo, dir, "patterns", 0);
    add_rpmmd_file(repo, dir, filename, 1);
  
  /* updateinfo */
  filename = repomd_find(repo, dir, "updateinfo", 1);
  if (filename && (fp = repomd_open(dir, filename, &tmp, 0)) != 0)
    {
      if (repo_add_updateinfoxml(repo, fp, 0))
	{
	  fprintf(stderr, "%s: %s\n", tmp, pool_errstr(pool));
	  exit(1);
	}
      fclose(fp);
      tmp = solv_free(tmp);
    }

  /* deltainfo */
  filename = repomd_find(repo, dir, "deltainfo", 1);
  if (!filename)
    filename = repomd_find(repo, dir, "prestodelta", 1);
  if (filename && (fp = repomd_open(dir, filename, &tmp, 1)) != 0)
    {
      if (repo_add_deltainfoxml(repo, fp, 0))
	{
	  fprintf(stderr, "%s: %s\n", tmp, pool_errstr(pool));
	  exit(1);
	}
      fclose(fp);
      tmp = solv_free(tmp);
    }

#ifdef ENABLE_APPDATA
  /* appdata */
  filename = add_appdata ? repomd_find(repo, dir, "appdata", 1) : 0;
  if (filename && (fp = repomd_open(dir, filename, &tmp, 1)) != 0)
    {
      if (repo_add_appdata(repo, fp, 0))
	{
	  fprintf(stderr, "%s: %s\n", tmp, pool_errstr(pool));
	  exit(1);
	}
      fclose(fp);
      tmp = solv_free(tmp);
    }
#endif

  repo_internalize(repo);
  return 0;
}

#else

int
read_rpmmd_repo(Repo *repo, const char *dir)
{
  fprintf(stderr, "rpmmd repo type is not supported\n");
  exit(1);
}

#endif

static void
usage(int status)
{
  fprintf(stderr, "\nUsage:\n"
          "repo2solv [-R] [-X] [-A] [-o <out.solv>] <dir>\n"
	  "  Convert a repository in <dir> to a solv file\n"
	  "  -h : print help & exit\n"
	  "  -o <out.solv>: write to this file instead of stdout\n"
	  "  -F : add filelist\n"
	  "  -R : also search subdirectories for rpms\n"
	  "  -X : generate pattern/product pseudo packages\n"
	  "  -A : add appdata packages\n"
	 );
   exit(status);
}

int
main(int argc, char **argv)
{
  int c, res;
  int repotype = 0;
  char *outfile = 0;
  char *dir;
  struct stat stb;
  
  Pool *pool = pool_create();
  Repo *repo = repo_create(pool, "<repo>");

  while ((c = getopt(argc, argv, "hAXRFCo:")) >= 0)
    {
      switch(c)
	{
        case 'h':
          usage(0);
          break;
	case 'X':
#ifdef SUSE
	  add_auto = 1;
#endif
	  break;
	case 'A':
#ifdef ENABLE_APPDATA
	  add_appdata = 1;
#endif
	  break;
	case 'R':
	  repotype = REPO_PLAINDIR;
	  recursive = 1;
	  break;
	case 'F':
	  add_filelist = 1;
	  break;
	case 'C':
	  add_changelog = 1;
	  break;
	case 'o':
	  outfile = optarg;
	  break;
        default:
          usage(1);
          break;
	}
    }
  if (optind + 1 != argc)
    usage(1);
  dir = argv[optind];
  if (stat(dir, &stb))
    {
      perror(dir);
      exit(1);
    }
  if (!S_ISDIR(stb.st_mode))
    {
      fprintf(stderr, "%s: not a directory\n", dir);
      exit(1);
    }
  dir = solv_strdup(dir);
  if (repotype == 0)
    repotype = autodetect_repotype(pool, dir);

  switch (repotype)
    {
    case REPO_RPMMD:
      res = read_rpmmd_repo(repo, dir);
      break;
    case REPO_RPMMD_REPODATA:
      dir = solv_dupappend(dir, "/repodata", 0);
      res = read_rpmmd_repo(repo, dir);
      break;
    case REPO_SUSETAGS:
      res = read_susetags_repo(repo, dir);
      break;
    case REPO_PLAINDIR:
      res = read_plaindir_repo(repo, dir);
      break;
    default:
      fprintf(stderr, "unknown repotype %d\n", repotype);
      exit(1);
    }
  if (outfile && freopen(outfile, "w", stdout) == 0)
    {
      perror(outfile);
      exit(1);
    }
#ifdef SUSE
  if (add_auto)
    repo_add_autopattern(repo, 0);
  repo_mark_retracted_packages(repo, pool_str2id(pool, "retracted-patch-package()", 1));
#endif
  tool_write(repo, stdout);
  pool_free(pool);
  solv_free(dir);
  exit(res);
}
