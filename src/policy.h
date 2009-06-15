/*
 * Copyright (c) 2007, Novell Inc.
 *
 * This program is licensed under the BSD license, read LICENSE.BSD
 * for further information
 */

/*
 * Generic policy interface for SAT solver
 * The policy* function can be "overloaded" by defining a callback in the solver struct.
 */

#include "solver.h"

#define POLICY_MODE_CHOOSE	0
#define POLICY_MODE_RECOMMEND	1
#define POLICY_MODE_SUGGEST	2

/* This functions can be used for sorting solvables to a specific order like architecture, version. */
/* Solvables which does not fit to the system will be deleted from the list.                        */    
extern void prune_best_arch_name_version(Solver *solv, Pool *pool, Queue *plist);

extern void prune_to_best_arch(Pool *pool, Queue *plist);
extern void prune_to_best_version(Pool *pool, Queue *plist);


/* The following default policies can be overloaded by the application by using callbacks which are
 * descibed in solver.h:
 *  
 *  Finding best candidate
 * 
 * Callback definition:
 * void  bestSolvable (Pool *pool, Queue *canditates)
 *     candidates       : List of canditates which has to be sorted by the function call
 *     return candidates: Sorted list of the candidates(first is the best).
 *
 * Checking if two solvables has compatible architectures
 *
 * Callback definition:
 *     int  archCheck (Pool *pool, Solvable *solvable1, Solvable *solvable2);
 *     
 *     return 0 it the two solvables has compatible architectures
 *
 * Checking if two solvables has compatible vendors
 *
 * Callback definition:
 *     int  vendorCheck (Pool *pool, Solvable *solvable1, Solvable *solvable2);
 *     
 *     return 0 it the two solvables has compatible architectures
 *
 * Evaluate update candidate
 *
 * Callback definition:
 * void pdateCandidateCb (Pool *pool, Solvable *solvable, Queue *canditates)
 *     solvable   : for which updates should be search
 *     candidates : List of candidates (This list depends on other
 *                  restrictions like architecture and vendor policies too)
 */   
extern void policy_filter_unwanted(Solver *solv, Queue *plist, int mode);
extern int  policy_illegal_archchange(Solver *solv, Solvable *s1, Solvable *s2);
extern int  policy_illegal_vendorchange(Solver *solv, Solvable *s1, Solvable *s2);
extern void policy_findupdatepackages(Solver *solv,
				      Solvable *s,
				      Queue *qs,
				      int allowall); /* do not regard policies for vendor,architecuture,... change */

extern void policy_create_obsolete_index(Solver *solv);

