/*
 * Copyright (c) 2007, Novell Inc.
 *
 * This program is licensed under the BSD license, read LICENSE.BSD
 * for further information
 */

/*
 * repo_solv.h
 * 
 */

#ifndef REPO_SOLVE_H
#define REPO_SOLVE_H

#ifdef __cplusplus
extern "C" {
#endif

#include "pool.h"
#include "repo.h"

extern Repo *pool_addrepo_solv(Pool *pool, FILE *fp, const char *name);

#ifdef __cplusplus
}
#endif

#endif /* REPO_SOLVE_H */
