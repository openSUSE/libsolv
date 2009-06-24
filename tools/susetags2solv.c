/*
 * Copyright (c) 2007, Novell Inc.
 *
 * This program is licensed under the BSD license, read LICENSE.BSD
 * for further information
 */

#define _GNU_SOURCE

#include <sys/types.h>
#include <limits.h>
#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <dirent.h>
#include <zlib.h>
#include <getopt.h>

#include "pool.h"
#include "repo.h"
#include "repo_susetags.h"
#include "repo_content.h"
#include "common_write.h"

static void
usage(int status)
{
  fprintf(stderr, "\nUsage:\n"
          "susetags2solv [-b <base>][-c <content>][-d <descrdir>][-h][-k][-n <name>]\n"
	  "  reads a 'susetags' repository from <stdin> and writes a .solv file to <stdout>\n"
	  "  -b <base>: save as multiple files starting with <base>\n"
	  "  -c <contentfile> : parse given contentfile (for product information)\n"
          "  -d <descrdir> : do not read from stdin, but use data in descrdir\n"
	  "  -h : print help & exit\n"
	  "  -k : don't mix kinds (experimental!)\n"
	  "  -n <name>: save attributes as <name>.attr\n"
	 );
   exit(status);
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
myfopen(const char *fn)
{
  cookie_io_functions_t cio;
  char *suf;
  gzFile *gzf;

  if (!fn)
    return 0;
  suf = strrchr(fn, '.');
  if (!suf || strcmp(suf, ".gz") != 0)
    return fopen(fn, "r");
  gzf = gzopen(fn, "r");
  if (!gzf)
    return 0;
  memset(&cio, 0, sizeof(cio));
  cio.read = cookie_gzread;
  cio.close = cookie_gzclose;
  return  fopencookie(gzf, "r", cio);
}

/* content file query */
static void
doquery(Pool *pool, Repo *repo, const char *arg)
{
  char qbuf[256];
  const char *str;
  Id id;

  snprintf(qbuf, sizeof(qbuf), "susetags:%s", arg);
  id = str2id(pool, qbuf, 0);
  if (!id)
    return;
  str = repo_lookup_str(repo, SOLVID_META, id);
  if (str)
    printf("%s\n", str);
}

int
main(int argc, char **argv)
{
  const char *contentfile = 0;
  const char *attrname = 0;
  const char *descrdir = 0;
  const char *basefile = 0;
  const char *query = 0;
  Id product = 0;
  int flags = 0;
  int c;

  while ((c = getopt(argc, argv, "hkn:c:d:b:q:")) >= 0)
    {
      switch (c)
	{
	case 'h':
	  usage(0);
	  break;
	case 'k':
	  flags |= SUSETAGS_KINDS_SEPARATELY;	/* do not use! */
	  break;
	case 'n':
	  attrname = optarg;
	  break;
	case 'c':
	  contentfile = optarg;
	  break;
	case 'd':
	  descrdir = optarg;
	  break;
	case 'b':
	  basefile = optarg;
	  break;
	case 'q':
	  query = optarg;
	  break;
	default:
	  usage(1);
	  break;
	}
    }
  Pool *pool = pool_create();
  Repo *repo = repo_create(pool, "<susetags>");

  repo_add_repodata(repo, 0);

  if (contentfile)
    {
      FILE *fp = fopen (contentfile, "r");
      if (!fp)
        {
	  perror(contentfile);
	  exit(1);
	}
      repo_add_content(repo, fp, REPO_REUSE_REPODATA | REPO_NO_INTERNALIZE);
      product = repo->start;
      fclose (fp);
    }

  if (attrname)
    {
      /* ensure '.attr' suffix */
      const char *dot = strrchr(attrname, '.');
      if (!dot || strcmp(dot, ".attr"))
      {
	int len = strlen (attrname);
	char *newname = (char *)malloc(len + 6); /* alloc for <attrname>+'.attr'+'\0' */
	strcpy (newname, attrname);
	strcpy (newname+len, ".attr");
	attrname = newname;
      }
    }

  /*
   * descrdir path given, open files and read from there
   */
  
  if (descrdir)
    {
      char *fnp;
      int ndirs, i;
      struct dirent **files;

      ndirs = scandir(descrdir, &files, 0, alphasort);
      if (ndirs < 0)
	{
	  perror(descrdir);
	  exit(1);
	}

      /* bring packages to front */
      for (i = 0; i < ndirs; i++)
	{
	  char *fn = files[i]->d_name;
	  if (!strcmp(fn, "packages") || !strcmp(fn, "packages.gz"))
	    break;
        }
      if (i == ndirs)
	{
	  fprintf(stderr, "found no packages file\n");
	  exit(1);
	}
      if (i)
	{
	  struct dirent *de = files[i];
	  memmove(files + 1, files, i * sizeof(de));
	  files[0] = de;
	}

      fnp = sat_malloc(strlen(descrdir) + 128);
      for (i = 0; i < ndirs; i++)
	{
	  char *fn = files[i]->d_name;

	  if (!strcmp(fn, "packages") || !strcmp(fn, "packages.gz"))
	    {
	      sprintf(fnp, "%s/%s", descrdir, fn);
	      FILE *fp = myfopen(fnp);
	      if (!fp)
		{
		  perror(fn);
		  exit(1);
		}
	      repo_add_susetags(repo, fp, product, 0, flags | REPO_REUSE_REPODATA | REPO_NO_INTERNALIZE);
	      fclose(fp);
	    }
	  else if (!strcmp(fn, "packages.DU") || !strcmp(fn, "packages.DU.gz"))
	    {
	      sprintf(fnp, "%s/%s", descrdir, fn);
	      FILE *fp = myfopen(fnp);
	      if (!fp)
		{
		  perror(fn);
		  exit(1);
		}
	      repo_add_susetags(repo, fp, product, 0, flags | SUSETAGS_EXTEND | REPO_REUSE_REPODATA | REPO_NO_INTERNALIZE);
	      fclose(fp);
 	    }
	  else if (!strcmp(fn, "packages.FL") || !strcmp(fn, "packages.FL.gz"))
	    {
#if 0
	      sprintf(fnp, "%s/%s", descrdir, fn);
	      FILE *fp = myfopen(fnp);
	      if (!fp)
		{
		  perror(fn);
		  exit(1);
		}
	      repo_add_susetags(repo, fp, product, 0, flags | SUSETAGS_EXTEND | REPO_REUSE_REPODATA | REPO_NO_INTERNALIZE);
	      fclose(fp);
#else
	      /* ignore for now. reactivate when filters work */
	      continue;
#endif
 	    }
	  else if (!strncmp(fn, "packages.", 9))
	    {
	      char lang[6];
	      char *p;
	      sprintf(fnp, "%s/%s", descrdir, fn);
	      p = strrchr(fnp, '.');
	      if (p && !strcmp(p, ".gz"))
		{
		  *p = 0;
		  p = strrchr(fnp, '.');
		}
	      if (!p || !p[1] || strlen(p + 1) > 5)
		continue;
	      strcpy(lang, p + 1);
	      sprintf(fnp, "%s/%s", descrdir, fn);
	      FILE *fp = myfopen(fnp);
	      if (!fp)
		{
		  perror(fn);
		  exit(1);
		}
	      repo_add_susetags(repo, fp, product, lang, flags | SUSETAGS_EXTEND | REPO_REUSE_REPODATA | REPO_NO_INTERNALIZE);
	      fclose(fp);
	    }
	}
      for (i = 0; i < ndirs; i++)
	free(files[i]);
      free(files);
      free(fnp);
      repo_internalize(repo);
    }
  else
    /* read data from stdin */
    repo_add_susetags(repo, stdin, product, 0, REPO_REUSE_REPODATA | REPO_NO_INTERNALIZE);
  repo_internalize(repo);

  if (query)
    doquery(pool, repo, query);
  else
    tool_write(repo, basefile, attrname);
  pool_free(pool);
  exit(0);
}
