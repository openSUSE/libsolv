/*
 * solvable.h
 * 
 * A solvable represents an object with name-epoch:version-release.arch and dependencies
 */

#ifndef SOLVABLE_H
#define SOLVABLE_H

#include "pooltypes.h"
#include "repo.h"

typedef struct _Solvable {
  Id name;
  Id arch;
  Id evr;			/* epoch:version-release */
  Id vendor;

  Repo *repo;		/* repo we belong to */

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

#endif /* SOLVABLE_H */
