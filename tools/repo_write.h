/*
 * Copyright (c) 2007, Novell Inc.
 *
 * This program is licensed under the BSD license, read LICENSE.BSD
 * for further information
 */

/*
 * repo_write.h
 *
 */

#ifndef REPO_WRITE_H
#define REPO_WRITE_H

#include <stdio.h>

#include "pool.h"
#include "repo.h"

extern void repo_write(Repo *repo, FILE *fp);

#endif
