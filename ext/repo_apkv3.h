/*
 * Copyright (c) 2025, SUSE LLC
 *
 * This program is licensed under the BSD license, read LICENSE.BSD
 * for further information
 */

extern Id apkv3_add_pkg(Repo *repo, Repodata *data, const char *fn, FILE *fp, int flags);
extern int apkv3_add_idx(Repo *repo, Repodata *data, FILE *fp, int flags);

