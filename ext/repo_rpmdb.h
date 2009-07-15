/*
 * Copyright (c) 2007-2008, Novell Inc.
 *
 * This program is licensed under the BSD license, read LICENSE.BSD
 * for further information
 */

#include "queue.h"
#include "repo.h"

struct headerToken_s;

extern void repo_add_rpmdb(Repo *repo, Repo *ref, const char *rootdir, int flags);
extern void repo_add_rpms(Repo *repo, const char **rpms, int nrpms, int flags);
void repo_add_rpmdb_pubkeys(Repo *repo, const char *rootdir, int flags);
void repo_add_pubkeys(Repo *repo, const char **keys, int nkeys, int flags);

#define RPMDB_REPORT_PROGRESS	(1 << 8)
#define RPM_ADD_WITH_PKGID	(1 << 9)
#define RPM_ADD_NO_FILELIST	(1 << 10)
#define RPM_ADD_NO_RPMLIBREQS	(1 << 11)
#define RPM_ADD_WITH_SHA1SUM	(1 << 12)
#define RPM_ADD_WITH_SHA256SUM	(1 << 13)

#define RPM_ITERATE_FILELIST_ONLYDIRS	(1 << 0)
#define RPM_ITERATE_FILELIST_WITHMD5	(1 << 1)

void *rpm_byrpmdbid(Id rpmdbid, const char *rootdir, void **statep);
void *rpm_byfp(FILE *fp, const char *name, void **statep);
void *rpm_byrpmh(struct headerToken_s *h, void **statep);


char *rpm_query(void *rpmhandle, Id what);
void rpm_iterate_filelist(void *rpmhandle, int flags, void (*cb)(void *, const char *, int, const char *), void *cbdata);
int  rpm_installedrpmdbids(const char *rootdir, const char *index, const char *match, Queue *rpmdbidq);
