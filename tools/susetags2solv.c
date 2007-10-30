#include <sys/types.h>
#include <limits.h>
#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "pool.h"
#include "repo_susetags.h"
#include "repo_write.h"
#include "attr_store.h"

extern Attrstore *attr;

int
main(int argc, char **argv)
{
  int with_attr = 0;
  argv++;
  argc--;
  while (argc--)
    {
      const char *s = argv[0];
      if (*s++ == '-')
        while (*s)
          switch (*s++)
	    {
	      case 'a': with_attr = 1; break;
	      default : break;
	    }
      argv++;
    }
  Pool *pool = pool_create();
  Repo *repo = pool_addrepo_susetags(pool, stdin, 0, with_attr);
  pool_writerepo(pool, repo, stdout);
  if (with_attr && attr)
    {
      FILE *fp = fopen ("test.attr", "w");
      write_attr_store (fp, attr);
      fclose (fp);
    }
  pool_free(pool);
  exit(0);
}
