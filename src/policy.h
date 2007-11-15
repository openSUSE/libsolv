/*
 * Copyright (c) 2007, Novell Inc.
 *
 * This program is licensed under the BSD license, read LICENSE.BSD
 * for further information
 */

/*
 * Generic policy interface for SAT solver
 * 
 */

#include "solver.h"

#define POLICY_MODE_CHOOSE	0
#define POLICY_MODE_RECOMMEND	1
#define POLICY_MODE_SUGGEST	2

/* legacy, do not use! */
extern void prune_best_version_arch(Pool *pool, Queue *plist);

extern void policy_filter_unwanted(Solver *solv, Queue *plist, Id inst, int mode);

extern int  policy_illegal_archchange(Pool *pool, Solvable *s1, Solvable *s2);
extern int  policy_illegal_vendorchange(Pool *pool, Solvable *s1, Solvable *s2);
extern void policy_findupdatepackages(Solver *solv,
				      Solvable *s,
				      Queue *qs,
				      int allowall); /* do not regard policies for vendor,architecuture,... change */
