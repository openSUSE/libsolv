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
#include <unistd.h>
#include <zlib.h>

#include "pool.h"
#include "repo.h"
#include "repo_rpmmd.h"
#include "common_write.h"


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

static void
usage(int status)
{
  fprintf(stderr, "\nUsage:\n"
          "rpmmd2solv [-a][-h][-k][-n <attrname>]\n"
	  "  reads a 'rpmmd' repository from <stdin> and writes a .solv file to <stdout>\n"
	  "  -h : print help & exit\n"
	  "  -k : don't mix kinds (experimental!)\n"
	  "  -n <name>: save attributes as <name>.attr\n"
	 );
   exit(status);
}

int
main(int argc, char **argv)
{
  int c, flags = 0;
  const char *attrname = 0;
  const char *basefile = 0;
  const char *dir = 0;

  
  Pool *pool = pool_create();
  Repo *repo = repo_create(pool, "<stdin>");

  while ((c = getopt (argc, argv, "hkn:b:d:")) >= 0)
    {
      switch(c)
	{
        case 'h':
          usage(0);
          break;
        case 'k':
          flags |= RPMMD_KINDS_SEPARATELY;   /* do not use! */
          break;
        case 'n':
          attrname = optarg;
          break;
        case 'b':
          basefile = optarg;
          break;
        case 'd':
          dir = optarg;
          break;
        default:
          usage(1);
          break;
	}
    }
  if (dir)
    {
      FILE *fp;
      int l;
      char *fnp;
      l = strlen(dir);
      fnp = sat_malloc(l + 20);
      sprintf(fnp, "%s/primary.xml.gz", dir);
      if (!(fp = myfopen(fnp)))
	{
	  perror(fnp);
	  exit(1);
	}
      repo_add_rpmmd(repo, fp, flags);
      fclose(fp);
      sat_free(fnp);
    }
  else
    repo_add_rpmmd(repo, stdin, flags);
  tool_write(repo, basefile, attrname);
  pool_free(pool);
  exit(0);
}
