/*
 * Copyright (c) 2007, Novell Inc.
 *
 * This program is licensed under the BSD license, read LICENSE.BSD
 * for further information
 */

/*
 * selection.h
 * 
 */

#ifndef LIBSOLV_SELECTION_H
#define LIBSOLV_SELECTION_H

#include "pool.h"

#define SELECTION_NAME			(1 << 0)
#define SELECTION_PROVIDES		(1 << 1)
#define SELECTION_FILELIST		(1 << 2)

#define SELECTION_INSTALLED_ONLY	(1 << 8)
#define SELECTION_GLOB			(1 << 9)
#define SELECTION_FLAT			(1 << 10)
#define SELECTION_NOCASE		(1 << 11)

extern int selection_make(Pool *pool, Queue *selection, const char *name, int flags);
extern void selection_limit(Pool *pool, Queue *sel1, Queue *sel2);
extern void selection_add(Pool *pool, Queue *sel1, Queue *sel2);
extern void selection_solvables(Pool *pool, Queue *selection, Queue *pkgs);

#endif
