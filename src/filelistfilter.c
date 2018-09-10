/*
 * Copyright (c) 2018, SUSE LLC
 *
 * This program is licensed under the BSD license, read LICENSE.BSD
 * for further information
 */

/*
 * filelistfilter.c
 *
 * Support repodata with a filelist filtered by a custom filter
 */

#define _GNU_SOURCE
#include <string.h>
#include <fnmatch.h>

#include <stdio.h>
#include <stdlib.h>
#include <stdarg.h>
#include <unistd.h>

#include "repo.h"
#include "pool.h"
#include "util.h"

int
repodata_filelistfilter_matches(Repodata *data, const char *str)
{
  /* '.*bin\/.*', '^\/etc\/.*', '^\/usr\/lib\/sendmail$' */
  /* for now hardcoded */
  if (strstr(str, "bin/"))
    return 1;
  if (!strncmp(str, "/etc/", 5))
    return 1;
  if (!strcmp(str, "/usr/lib/sendmail"))
    return 1;
  return 0;
}

