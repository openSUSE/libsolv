/*
 * Copyright (c) 2009, Novell Inc.
 *
 * This program is licensed under the BSD license, read LICENSE.BSD
 * for further information
 */

extern void repo_add_debpackages(Repo *repo, FILE *fp, int flags);
extern void repo_add_debs(Repo *repo, const char **debs, int ndebs, int flags);

#define DEBS_ADD_WITH_PKGID	(1 << 8)
