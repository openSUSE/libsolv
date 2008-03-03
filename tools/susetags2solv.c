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

#include "pool.h"
#include "repo.h"
#include "repo_susetags.h"
#include "repo_content.h"
#include "common_write.h"

static void
usage(int status)
{
  fprintf(stderr, "\nUsage:\n"
          "susetags2solv [-a][-s][-c <content>][-h]\n"
	  "  reads a 'susetags' repository from <stdin> and writes a .solv file to <stdout>\n"
	  "  -c <contentfile> : parse given contentfile (for product information)\n"
          "  -d <descrdir> : do not read from stdin, but use data in descrdir\n"
	  "  -h : print help & exit\n"
	  "  -k : don't mix kinds (experimental!)\n"
	  "  -b <base>: save fas multiple files starting with <base>\n"
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

int
main(int argc, char **argv)
{
  const char *contentfile = 0;
  const char *attrname = 0;
  const char *descrdir = 0;
  const char *basefile = 0;
  Id vendor = 0;
  int flags = 0;
  int c;

  while ((c = getopt(argc, argv, "hkn:c:d:b:")) >= 0)
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
	default:
	  usage(1);
	  break;
	}
    }
  Pool *pool = pool_create();
  Repo *repo = repo_create(pool, "<susetags>");
  if (contentfile)
    {
      FILE *fp = fopen (contentfile, "r");
      if (!fp)
        {
	  perror(contentfile);
	  exit(1);
	}
      repo_add_content(repo, fp);
      if (!strncmp (id2str(pool, pool->solvables[repo->start].name), "product:", 8))
        vendor = pool->solvables[repo->start].vendor;
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
	      repo_add_susetags(repo, fp, vendor, 0, flags);
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
	      repo_add_susetags(repo, fp, vendor, 0, flags | SUSETAGS_EXTEND);
	      fclose(fp);
 	    }
	  else if (!strncmp(fn, "packages.", 9))
	    {
	      char lang[6];
	      char *p;
	      sprintf(fnp, "%s/%s", descrdir, fn);
	      p = strrchr(fn, '.');
	      if (p && !strcmp(p, ".gz"))
		{
		  *p = 0;
		  p = strrchr(fn, '.');
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
	      repo_add_susetags(repo, fp, vendor, lang, flags | SUSETAGS_EXTEND);
	      fclose(fp);
	    }
	}
      free(files);
      free(fnp);
    }
  else
    repo_add_susetags(repo, stdin, vendor, 0, flags);

  tool_write(repo, basefile, attrname);
  pool_free(pool);
  exit(0);
}
