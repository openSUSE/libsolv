/*
 * Copyright (c) 2012, Novell Inc.
 *
 * This program is licensed under the BSD license, read LICENSE.BSD
 * for further information
 */

/*
 * comps2solv.c
 * 
 * parse Mandriva/Mageie synthesis file
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
#include "repo.h"
#include "repo_mdk.h"
#include "common_write.h"

int
main(int argc, char **argv)
{
  Pool *pool = pool_create();
  Repo *repo = repo_create(pool, "<stdin>");
  repo_add_mdk(repo, stdin, 0);
  tool_write(repo, 0, 0);
  pool_free(pool);
  exit(0);
}
