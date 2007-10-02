/*
 * source_solv.h
 * 
 */

#ifndef SOURCE_SOLVE_H
#define SOURCE_SOLVE_H

#include "pool.h"
#include "source.h"

extern Source *pool_addsource_solv(Pool *pool, FILE *fp, const char *name);

#endif /* SOURCE_SOLVE_H */
