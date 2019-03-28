/*
 * Copyright (c) 2019, SUSE LLC
 *
 * This program is licensed under the BSD license, read LICENSE.BSD
 * for further information
 */

/*
 * conda.h
 *
 */

#ifndef LIBSOLV_CONDA_H
#define LIBSOLV_CONDA_H

int pool_evrcmp_conda(const Pool *pool, const char *evr1, const char *evr2, int mode);
int solvable_conda_matchversion(Solvable *s, const char *version);

#endif /* LIBSOLV_CONDA_H */

