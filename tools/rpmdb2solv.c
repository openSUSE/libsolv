/*
 * rpmdb2solv
 * 
 */

#include <sys/types.h>
#include <limits.h>
#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "pool.h"
#include "source_rpmdb.h"
#include "source_solv.h"
#include "source_write.h"

int
main(int argc, char **argv)
{
  Pool *pool = pool_create();
  Source *ref = NULL;
  FILE *fp;

  if (argc != 1)
    {
      Pool *refpool = pool;
      if ((fp = fopen(argv[1], "r")) == NULL)
	{
	  perror(argv[1]);
	  exit(0);
	}
      ref = pool_addsource_solv(refpool, fp, "rpmdb");
      fclose(fp);
    }

  Source *source = pool_addsource_rpmdb(pool, ref);
  if (ref)
    {
      if (ref->pool != pool)
	pool_free(ref->pool);
      else
	pool_freesource(pool, ref);
      ref = NULL;
    }

  pool_writesource(pool, source, stdout);
  pool_free(pool);

  exit(0);
}
