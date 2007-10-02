/*
 * solvable.h
 * 
 * A solvable represents an object with name-epoch:version-release.arch and dependencies
 */

#ifndef SOLVABLE_H
#define SOLVABLE_H

#include "pooltypes.h"

typedef struct _Solvable {
  Id name;
  Id arch;
  Id evr;

  // dependencies are pointers into idarray of source the solvable originates from
  Id *provides;			// terminated with Id 0
  Id *obsoletes;
  Id *conflicts;

  Id *requires;
  Id *recommends;
  Id *suggests;

  Id *supplements;
  Id *enhances;

  Id *freshens;
} Solvable;

#endif /* SOLVABLE_H */
