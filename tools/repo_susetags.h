/*
 * Copyright (c) 2007, Novell Inc.
 *
 * This program is licensed under the BSD license, read LICENSE.BSD
 * for further information
 */

/* read susetags file <fp> into <repo>
 * if <attrname> given, write attributes as '<attrname>.attr'
 */

extern void repo_add_susetags(Repo *repo, FILE *fp, Id vendor, const char *attrname);
