#include <sys/types.h>
#include <limits.h>
#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "pool.h"
#include "source_content.h"
#include "source_write.h"

int
main(int argc, char **argv)
{
  Pool *pool = pool_create();
  Source *source = pool_addsource_content(pool, stdin);
  pool_writesource(pool, source, stdout);
  pool_free(pool);
  return 0;
}
