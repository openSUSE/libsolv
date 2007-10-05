/*
 * source.h
 * 
 */

#ifndef SOURCE_H
#define SOURCE_H

#include "pooltypes.h"

typedef struct _Source {
  const char *name;
  struct _Pool *pool;		       /* pool containing source data */
  int start;			       /* start of this source solvables within pool->solvables */
  int nsolvables;		       /* number of solvables source is contributing to pool */

  Id *idarraydata;		       /* array of metadata Ids, solvable dependencies are offsets into this array */
  int idarraysize;
  Offset lastoff;

  Id *rpmdbid;
} Source;

extern unsigned int source_addid(Source *source, unsigned int olddeps, Id id);
extern unsigned int source_addid_dep(Source *source, unsigned int olddeps, Id id, int isreq);
extern unsigned int source_reserve_ids(Source *source, unsigned int olddeps, int num);
extern unsigned int source_fix_legacy(Source *source, unsigned int provides, unsigned int supplements);

extern Source *pool_addsource_empty(Pool *pool);

extern const char *source_name(const Source *source);
#endif /* SOURCE_H */
