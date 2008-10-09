/*
 * Copyright (c) 2007, Novell Inc.
 *
 * This program is licensed under the BSD license, read LICENSE.BSD
 * for further information
 */

/*
 * repo_helix.h
 * 
 */

#ifndef SATSOLVER_REPO_HELIX_H
#define SATSOLVER_REPO_HELIX_H

#ifdef __cplusplus
extern "C" {
#endif

#include <stdio.h>
#include "pool.h"
#include "repo.h"

extern void repo_add_helix(Repo *repo, FILE *fp, int flags);

#ifdef __cplusplus
}
#endif
    

#endif /* SATSOLVER_REPO_HELIX_H */
