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

typedef struct _Solvable {
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

} Solvable;

#endif /* SATSOLVER_SOLVABLE_H */
