/*
 * Copyright (c) 2019, SUSE LLC
 *
 * This program is licensed under the BSD license, read LICENSE.BSD
 * for further information
 */

#define CONDA_ADD_USE_ONLY_TAR_BZ2  (1 << 8)

extern int repo_add_conda(Repo *repo, FILE *fp, int flags);
