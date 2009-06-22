/*
 * Copyright (c) 2007, Novell Inc.
 *
 * This program is licensed under the BSD license, read LICENSE.BSD
 * for further information
 */

#define RPMMD_KINDS_SEPARATELY (1 << 2)

extern void repo_add_rpmmd(Repo *repo, FILE *fp, const char *language, int flags);
