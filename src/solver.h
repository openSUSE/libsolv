/*
 * solver.h
 *
 */

#ifndef SOLVER_H
#define SOLVER_H

#include "pooltypes.h"
#include "pool.h"
#include "source.h"
#include "queue.h"
#include "bitmap.h"

/* ----------------------------------------------
 * Rule
 *
 *   providerN(B) == Package Id of package providing tag B
 *   N = 1, 2, 3, in case of multiple providers
 *
 * A requires B : !A | provider1(B) | provider2(B)
 *
 * A conflicts B : (!A | !provider1(B)) & (!A | !provider2(B)) ...
 *
 * 'not' is encoded as a negative Id
 * 
 * Binary rule: p = first literal, d = 0, w2 = second literal, w1 = p
 */

typedef struct rule {
  Id p;			/* first literal in rule */
  Id d;			/* Id offset into 'list of providers terminated by 0' as used by whatprovides; pool->whatprovides + d */
			/* in case of binary rules, d == 0, w1 == p, w2 == other literal */
  Id w1, w2;		/* watches, literals not-yet-decided */
  				       /* if !w1, disabled */
  				       /* if !w2, assertion, not rule */
  Id n1, n2;		/* next rules in linked list, corresponding to w1,w2 */
} Rule;

typedef struct solver {
  Pool *pool;
  Source *system;

  int fixsystem;			/* repair errors in rpm dependency graph */
  int allowdowngrade;			/* allow to downgrade installed solvable */
  int allowarchchange;			/* allow to change architecture of installed solvables */
  int allowuninstall;			/* allow removal of system solvables, else keep all installed solvables */
  int updatesystem;			/* distupgrade */
  int allowvirtualconflicts;		/* false: conflicts on package name, true: conflicts on package provides */
  int noupdateprovide;			/* true: update packages needs not to provide old package */
  
  Rule *rules;				/* all rules */
  Id nrules;				/* rpm rules */

  Id jobrules;				/* user rules */
  Id systemrules;			/* policy rules, e.g. keep packages installed or update. All literals > 0 */
  Id weakrules;				/* rules that can be autodisabled */
  Id learntrules;			/* learnt rules */

  Id *weaksystemrules;			/* please try to install (r->d) */

  Id *watches;				/* Array of rule offsets
					 * watches has nsolvables*2 entries and is addressed from the middle
					 * middle-solvable : decision to conflict, offset point to linked-list of rules
					 * middle+solvable : decision to install: offset point to linked-list of rules
					 */

  Queue ruletojob;

  /* our decisions: */
  Queue decisionq;
  Queue decisionq_why;			/* index of rule, Offset into rules */
  Id *decisionmap;			/* map for all available solvables, > 0: level of decision when installed, < 0 level of decision when conflict */

  /* learnt rule history */
  Queue learnt_why;
  Queue learnt_pool;

  int propagate_index;

  Queue problems;
  Queue suggestions;			/* suggested packages */

  Map recommendsmap;			/* recommended packages from decisionmap */
  Map suggestsmap;			/* suggested packages from decisionmap */
  int recommends_index;			/* recommended level */

  Id *obsoletes;			/* obsoletes for each system solvable */
  Id *obsoletes_data;			/* data area for obsoletes */

  int rc_output;			/* output result compatible to redcarpet/zypp testsuite, set == 2 for pure rc (will suppress architecture) */
} Solver;

/*
 * queue commands
 */

enum solvcmds {
  SOLVCMD_NULL=0,
  SOLVER_INSTALL_SOLVABLE,
  SOLVER_ERASE_SOLVABLE,
  SOLVER_INSTALL_SOLVABLE_NAME,
  SOLVER_ERASE_SOLVABLE_NAME,
  SOLVER_INSTALL_SOLVABLE_PROVIDES,
  SOLVER_ERASE_SOLVABLE_PROVIDES,
  SOLVER_INSTALL_SOLVABLE_UPDATE
};

extern Solver *solver_create(Pool *pool, Source *system);
extern void solver_free(Solver *solv);
extern void solve(Solver *solv, Queue *job);

extern void prune_best_version_arch(Pool *pool, Queue *plist);

void printdecisions(Solver *solv);


#endif /* SOLVER_H */
