/*
 * Copyright (c) 2007-2008, Novell Inc.
 *
 * This program is licensed under the BSD license, read LICENSE.BSD
 * for further information
 */

#include "queue.h"
#include "repo.h"

struct headerToken_s;

extern int repo_add_rpmdb(Repo *repo, Repo *ref, int flags);
extern Id repo_add_rpm(Repo *repo, const char *rpm, int flags);
extern int repo_add_rpmdb_pubkeys(Repo *repo, int flags);
extern Id repo_add_pubkey(Repo *repo, const char *key, int flags);

#define RPMDB_REPORT_PROGRESS	(1 << 8)
#define RPM_ADD_WITH_PKGID	(1 << 9)
#define RPM_ADD_NO_FILELIST	(1 << 10)
#define RPM_ADD_NO_RPMLIBREQS	(1 << 11)
#define RPM_ADD_WITH_SHA1SUM	(1 << 12)
#define RPM_ADD_WITH_SHA256SUM	(1 << 13)
#define RPM_ADD_TRIGGERS	(1 << 14)
#define RPM_ADD_WITH_HDRID	(1 << 15)
#define RPM_ADD_WITH_LEADSIGID	(1 << 16)
#define RPM_ADD_WITH_CHANGELOG  (1 << 17)

#define RPM_ITERATE_FILELIST_ONLYDIRS	(1 << 0)
#define RPM_ITERATE_FILELIST_WITHMD5	(1 << 1)
#define RPM_ITERATE_FILELIST_WITHCOL	(1 << 2)
#define RPM_ITERATE_FILELIST_NOGHOSTS	(1 << 3)

extern void *rpm_byrpmdbid(Id rpmdbid, const char *rootdir, void **statep);
extern void *rpm_byfp(FILE *fp, const char *name, void **statep);
extern void *rpm_byrpmh(struct headerToken_s *h, void **statep);
extern int  rpm_installedrpmdbids(const char *rootdir, const char *index, const char *match, Queue *rpmdbidq);

extern char *rpm_query(void *rpmhandle, Id what);
extern void rpm_iterate_filelist(void *rpmhandle, int flags, void (*cb)(void *, const char *, int, const char *), void *cbdata);
extern Id   repo_add_rpm_handle(Repo *repo, void *rpmhandle, int flags);
