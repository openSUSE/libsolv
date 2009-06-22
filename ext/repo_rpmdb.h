/*
 * Copyright (c) 2007-2008, Novell Inc.
 *
 * This program is licensed under the BSD license, read LICENSE.BSD
 * for further information
 */

#include "queue.h"

extern void repo_add_rpmdb(Repo *repo, Repo *ref, const char *rootdir, int flags);
extern void repo_add_rpms(Repo *repo, const char **rpms, int nrpms, int flags);

#define RPMDB_REPORT_PROGRESS (1 << 8)

#define RPM_ITERATE_FILELIST_ONLYDIRS	(1 << 0)
#define RPM_ITERATE_FILELIST_WITHMD5	(1 << 1)

void *rpm_byrpmdbid(Id rpmdbid, const char *rootdir, void **statep);
void *rpm_byfp(FILE *fp, const char *name, void **statep);
void rpm_iterate_filelist(void *rpmhandle, int flags, void (*cb)(void *, const char *, int, const char *), void *cbdata);
int  rpm_installedrpmdbids(const char *rootdir, Queue *rpmdbidq);
