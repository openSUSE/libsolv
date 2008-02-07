/*
 * Copyright (c) 2007, Novell Inc.
 *
 * This program is licensed under the BSD license, read LICENSE.BSD
 * for further information
 */

/*
 * solvable.h
 * 
 * A solvable represents an object with name-epoch:version-release.arch and dependencies
 */

#ifndef SATSOLVER_SOLVABLE_H
#define SATSOLVER_SOLVABLE_H

#include "pooltypes.h"

struct _Repo;

typedef enum {
  KIND_PACKAGE = 0,
  KIND_PRODUCT = 5,         /* strlen("prod:") */
  KIND_PATCH = 6,           /* strlen("patch:") */
  KIND_SOURCE = 7,          /* strlen("source:") */
  KIND_PATTERN = 8,         /* strlen("pattern:") */
  KIND_NOSOURCE = 9,        /* strlen("nosource:") */
  _KIND_MAX
} solvable_kind;

extern const char *kind_prefix( solvable_kind kind );

typedef struct _Solvable {
  unsigned int kind;           /* one of KIND_xxx */
  Id name;
  Id arch;
  Id evr;			/* epoch:version-release */
  Id vendor;

  struct _Repo *repo;		/* repo we belong to */

  /* dependencies are offsets into idarray of repo */
  Offset provides;			/* terminated with Id 0 */
  Offset obsoletes;
  Offset conflicts;

  Offset requires;
  Offset recommends;
  Offset suggests;

  Offset supplements;
  Offset enhances;

  Offset freshens;
} Solvable;

#endif /* SATSOLVER_SOLVABLE_H */
