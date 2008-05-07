/*
 * Copyright (c) 2007, Novell Inc.
 *
 * This program is licensed under the BSD license, read LICENSE.BSD
 * for further information
 */

/*
 * repo_write.h
 *
 */

#ifndef REPO_WRITE_H
#define REPO_WRITE_H

#include <stdio.h>

#include "pool.h"
#include "repo.h"

/* Describes a repodata file */
typedef struct _Repodatafile
{
  /* These have the same meaning as the equally named fields in
     Repodata.  */
  char *location;
  char *checksum;
  unsigned int nchecksum;
  unsigned int checksumtype;
  struct _Repokey *keys;
  unsigned int nkeys;
  Id *addedfileprovides;
  unsigned char *rpmdbcookie;
} Repodatafile;

void repo_write(Repo *repo, FILE *fp, int (*keyfilter)(Repo *repo, Repokey *key, void *kfdata), void *kfdata, Repodatafile *fileinfo, int nsubfiles);

#endif
