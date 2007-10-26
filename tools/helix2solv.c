/*
 * helix2solv.c
 * 
 * parse 'helix' type xml and write out .solv file
 *
 * reads from stdin
 * writes to stdout
 */

#include <sys/types.h>
#include <limits.h>
#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "pool.h"
#include "repo_helix.h"
#include "repo_write.h"

int
main(int argc, char **argv)
{
  Pool *pool = pool_create();
  Repo *repo = pool_addrepo_helix(pool, stdin);
  pool_writerepo(pool, repo, stdout);
  pool_free(pool);
  exit(0);
}
