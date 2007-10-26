/*
 * repo_write.h
 *
 */

#ifndef REPO_WRITE_H
#define REPO_WRITE_H

#include <stdio.h>

#include "pool.h"
#include "repo.h"

extern void pool_writerepo(Pool *pool, Repo *repo, FILE *fp);

#endif
