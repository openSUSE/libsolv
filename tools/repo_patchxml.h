/*
 * Copyright (c) 2007, Novell Inc.
 *
 * This program is licensed under the BSD license, read LICENSE.BSD
 * for further information
 */

#define PATCHXML_KINDS_SEPARATELY (1 << 2)

extern void repo_add_patchxml(Repo *repo, FILE *fp, int flags);
