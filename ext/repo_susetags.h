/*
 * Copyright (c) 2007, Novell Inc.
 *
 * This program is licensed under the BSD license, read LICENSE.BSD
 * for further information
 */

/* read susetags file <fp> into <repo>
 * if <attrname> given, write attributes as '<attrname>.attr'
 */

#define SUSETAGS_KINDS_SEPARATELY	(1 << 8)
#define SUSETAGS_EXTEND			(1 << 9)

extern void repo_add_susetags(Repo *repo, FILE *fp, Id defvendor, const char *language, int flags);
