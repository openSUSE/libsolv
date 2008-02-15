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
#include "repo_patchxml.h"
#include "common_write.h"

static void
usage(const char *err)
{
  if (err)
    fprintf(stderr, "\n** Error:\n  %s\n", err);
  fprintf(stderr, "\nUsage:\n"
          "patchxml2solv [-a][-h][-k][-n <attrname>]\n"
	  "  reads a 'patchxml' file from <stdin> and writes a .solv file to <stdout>\n"
	  "  -h : print help & exit\n"
	  "  -k : don't mix kinds (experimental!)\n"
	  "  -n <name>: save attributes as <name>.attr\n"
	 );
   exit(0);
}

int
main(int argc, char **argv)
{
  int flags = 0;
  char *attrname = 0;
  
  Pool *pool = pool_create();
  Repo *repo = repo_create(pool, "<stdin>");

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
	      case 'k':
	        flags |= PATCHXML_KINDS_SEPARATELY;
	      break;
	      default : break;
	    }
      argv++;
    }

  repo_add_patchxml(repo, stdin, flags);
  tool_write(repo, 0, 0);
  pool_free(pool);
  exit(0);
}
