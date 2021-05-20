/*
 * Copyright (c) 2007, Novell Inc.
 *
 * This program is licensed under the BSD license, read LICENSE.BSD
 * for further information
 */

extern int repo_add_updateinfoxml(Repo *repo, FILE *fp, int flags);

extern void repo_mark_retracted_packages(Repo *repo, Id retractedmarker);

