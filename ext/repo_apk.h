/*
 * Copyright (c) 2024, SUSE LLC
 *
 * This program is licensed under the BSD license, read LICENSE.BSD
 * for further information
 */

#define APK_ADD_INSTALLED_DB		(1 << 8)
#define APK_ADD_WITH_PKGID		(1 << 9)
#define APK_ADD_WITH_HDRID		(1 << 10)

extern Id repo_add_apk_pkg(Repo *repo, const char *fn, int flags);
extern int repo_add_apk_repo(Repo *repo, FILE *fp, int flags);

