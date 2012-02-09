/*
 * Copyright (c) 2012, Novell Inc.
 *
 * This program is licensed under the BSD license, read LICENSE.BSD
 * for further information
 */

#include "pool.h"
#include "repo.h"

extern Id testcase_str2dep(Pool *pool, char *s);
extern const char *testcase_solvid2str(Pool *pool, Id p);
extern Repo *testcase_str2repo(Pool *pool, const char *str);
extern Id testcase_str2solvid(Pool *pool, const char *str);
extern const char *testcase_job2str(Pool *pool, Id how, Id what);
extern Id testcase_str2job(Pool *pool, const char *str, Id *whatp);
extern int testcase_write_susetags(Repo *repo, FILE *fp);

