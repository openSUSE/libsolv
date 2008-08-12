/*
 * Copyright (c) 2007, Novell Inc.
 *
 * This program is licensed under the BSD license, read LICENSE.BSD
 * for further information
 */

extern void repo_add_rpmdb(Repo *repo, Repodata *repodata, Repo *ref, const char *rootdir);
extern void repo_add_rpms(Repo *repo, Repodata *repodata, const char **rpms, int nrpms);
