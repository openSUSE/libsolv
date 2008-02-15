/*
 * Copyright (c) 2007, Novell Inc.
 *
 * This program is licensed under the BSD license, read LICENSE.BSD
 * for further information
 */

#include <sys/types.h>
#include <limits.h>
#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "pool.h"
#include "repo.h"
#include "repo_susetags.h"
#include "repo_content.h"
#include "common_write.h"

static void
usage(const char *err)
{
  if (err)
    fprintf(stderr, "\n** Error:\n  %s\n", err);
  fprintf(stderr, "\nUsage:\n"
          "susetags2solv [-a][-s][-c <content>][-h]\n"
	  "  reads a 'susetags' repository from <stdin> and writes a .solv file to <stdout>\n"
	  "  -a : with attributes\n"
	  "  -c <contentfile> : parse given contentfile (for product information)\n"
	  "  -h : print help & exit\n"
	  "  -k : don't mix kinds (experimental!)\n"
	  "  -n <name>: save attributes as <name>.attr\n"
	 );
   exit(0);
}

int
main(int argc, char **argv)
{
  const char *contentfile = 0;
  const char *attrname = 0;
  Id vendor = 0;
  int flags = 0;
  argv++;
  argc--;
  while (argc--)
    {
      const char *s = argv[0];
      if (*s++ == '-')
        while (*s)
          switch (*s++)
	    {
	      case 'h': usage(NULL); break;
	      case 'n':
	        if (argc)
		  {
		    attrname = argv[1];
		    argv++;
		    argc--;
		  }
	        else
		  usage("argument required for '-n'");
		break;
	      case 'c':
	        if (argc)
		  {
		    contentfile = argv[1];
		    argv++;
		    argc--;
		  }
	        else
		  usage("argument required for '-c'");
		break;
	      case 'k':
	        flags |= SUSETAGS_KINDS_SEPARATELY;
	      break;
	      default : break;
	    }
      argv++;
    }
  Pool *pool = pool_create();
  Repo *repo = repo_create(pool, "<stdin>");
  if (contentfile)
    {
      FILE *fp = fopen (contentfile, "r");
      if (!fp)
        {
	  perror("opening content file");
	  exit (1);
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
	char *newname = (char *)malloc (len + 6); /* alloc for <attrname>+'.attr'+'\0' */
	strcpy (newname, attrname);
	strcpy (newname+len, ".attr");
	attrname = newname;
      }
    }
  repo_add_susetags(repo, stdin, vendor, flags);
  tool_write(repo, 0, attrname);
  pool_free(pool);
  exit(0);
}
