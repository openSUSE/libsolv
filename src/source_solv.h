/*
 * source_solv.h
 * 
 */

#ifndef SOURCE_SOLVE_H
#define SOURCE_SOLVE_H

#ifdef __cplusplus
extern "C" {
#endif

#include "pool.h"
#include "source.h"

extern Source *pool_addsource_solv(Pool *pool, FILE *fp, const char *name);

#ifdef __cplusplus
}
#endif

#endif /* SOURCE_SOLVE_H */
