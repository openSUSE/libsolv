/*
 * Copyright (c) 2007, Novell Inc.
 *
 * This program is licensed under the BSD license, read LICENSE.BSD
 * for further information
 */

/*
 * solver.h
 *
 */

#ifndef SATSOLVER_SOLVER_H
#define SATSOLVER_SOLVER_H

#ifdef __cplusplus
extern "C" {
#endif

#include "pooltypes.h"
#include "pool.h"
#include "repo.h"
#include "queue.h"
#include "bitmap.h"
/*
 * Callback definitions in order to "overwrite" the policies by an external application.
 */
 
typedef void  (*BestSolvableCb) (Pool *pool, Queue *canditates);
typedef int  (*ArchCheckCb) (Pool *pool, Solvable *solvable1, Solvable *solvable2);
typedef int  (*VendorCheckCb) (Pool *pool, Solvable *solvable1, Solvable *solvable2);
typedef void (*UpdateCandidateCb) (Pool *pool, Solvable *solvable, Queue *canditates);


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

struct solver;

typedef struct solver {
  Pool *pool;
  Repo *installed;
  
  Rule *rules;				/* all rules */
  Id nrules;				/* index of the last rule */

  Id jobrules;				/* user rules */
  Id updaterules;			/* policy rules, e.g. keep packages installed or update. All literals > 0 */
  Id featurerules;			/* feature rules */
  Id learntrules;			/* learnt rules */

  Map noupdate;				/* don't try to update these
                                           installed solvables */
  Queue weakruleq;
  Map weakrulemap;

  Id *watches;				/* Array of rule offsets
					 * watches has nsolvables*2 entries and is addressed from the middle
					 * middle-solvable : decision to conflict, offset point to linked-list of rules
					 * middle+solvable : decision to install: offset point to linked-list of rules
					 */

  Queue ruletojob;

  /* our decisions: */
  Queue decisionq;
  Queue decisionq_why;			/* index of rule, Offset into rules */
  int directdecisions;			/* number of decisions with no rule */

  Id *decisionmap;			/* map for all available solvables, > 0: level of decision when installed, < 0 level of decision when conflict */

  /* learnt rule history */
  Queue learnt_why;
  Queue learnt_pool;

  Queue branches;
  int (*solution_callback)(struct solver *solv, void *data);
  void *solution_callback_data;

  int propagate_index;

  Queue problems;
  Queue recommendations;		/* recommended packages */
  Queue suggestions;			/* suggested packages */


  Map recommendsmap;			/* recommended packages from decisionmap */
  Map suggestsmap;			/* suggested packages from decisionmap */
  int recommends_index;			/* recommendsmap/suggestsmap is created up to this level */

  Id *obsoletes;			/* obsoletes for each installed solvable */
  Id *obsoletes_data;			/* data area for obsoletes */

  /*-------------------------------------------------------------------------------------------------------------
   * Solver configuration
   *-------------------------------------------------------------------------------------------------------------*/

  int fixsystem;			/* repair errors in rpm dependency graph */
  int allowdowngrade;			/* allow to downgrade installed solvable */
  int allowarchchange;			/* allow to change architecture of installed solvables */
  int allowvendorchange;		/* allow to change vendor of installed solvables */
  int allowuninstall;			/* allow removal of installed solvables */
  int updatesystem;			/* distupgrade */
  int allowvirtualconflicts;		/* false: conflicts on package name, true: conflicts on package provides */
  int allowselfconflicts;		/* true: packages wich conflict with itself are installable */
  int noupdateprovide;			/* true: update packages needs not to provide old package */
  int dosplitprovides;			/* true: consider legacy split provides */
  int dontinstallrecommended;		/* true: do not install recommended packages */
  int dontshowinstalledrecommended;	/* true: do not show recommended packages that are already installed */
  
  /* Callbacks for defining the bahaviour of the SAT solver */

  /* Finding best candidate
   *
   * Callback definition:
   * void  bestSolvable (Pool *pool, Queue *canditates)
   *     candidates       : List of canditates which has to be sorted by the function call
   *     return candidates: Sorted list of the candidates(first is the best).
   */
   BestSolvableCb bestSolvableCb;

  /* Checking if two solvables has compatible architectures
   *
   * Callback definition:
   *     int  archCheck (Pool *pool, Solvable *solvable1, Solvable *solvable2);
   *     
   *     return 0 it the two solvables has compatible architectures
   */
   ArchCheckCb archCheckCb;

  /* Checking if two solvables has compatible vendors
   *
   * Callback definition:
   *     int  vendorCheck (Pool *pool, Solvable *solvable1, Solvable *solvable2);
   *     
   *     return 0 it the two solvables has compatible architectures
   */
   VendorCheckCb vendorCheckCb;
    
  /* Evaluate update candidate
   *
   * Callback definition:
   * void pdateCandidateCb (Pool *pool, Solvable *solvable, Queue *canditates)
   *     solvable   : for which updates should be search
   *     candidates : List of candidates (This list depends on other
   *                  restrictions like architecture and vendor policies too)
   */
   UpdateCandidateCb   updateCandidateCb;
    

  /* some strange queue that doesn't belong here */

  Queue covenantq;                      /* Covenants honored by this solver (generic locks) */

  
} Solver;

/*
 * queue commands
 */

typedef enum {
  SOLVCMD_NULL=0,
  SOLVER_INSTALL_SOLVABLE,
  SOLVER_ERASE_SOLVABLE,
  SOLVER_INSTALL_SOLVABLE_NAME,
  SOLVER_ERASE_SOLVABLE_NAME,
  SOLVER_INSTALL_SOLVABLE_PROVIDES,
  SOLVER_ERASE_SOLVABLE_PROVIDES,
  SOLVER_INSTALL_SOLVABLE_UPDATE,
  SOLVER_INSTALL_SOLVABLE_ONE_OF,
  SOLVER_WEAKEN_SOLVABLE_DEPS,
  SOLVER_WEAK = 0x100,
} SolverCmd;

typedef enum {
  SOLVER_PROBLEM_UPDATE_RULE,
  SOLVER_PROBLEM_JOB_RULE,
  SOLVER_PROBLEM_JOB_NOTHING_PROVIDES_DEP,
  SOLVER_PROBLEM_NOT_INSTALLABLE,
  SOLVER_PROBLEM_NOTHING_PROVIDES_DEP,
  SOLVER_PROBLEM_SAME_NAME,
  SOLVER_PROBLEM_PACKAGE_CONFLICT,
  SOLVER_PROBLEM_PACKAGE_OBSOLETES,
  SOLVER_PROBLEM_DEP_PROVIDERS_NOT_INSTALLABLE,
  SOLVER_PROBLEM_SELF_CONFLICT
} SolverProbleminfo;


extern Solver *solver_create(Pool *pool, Repo *installed);
extern void solver_free(Solver *solv);
extern void solver_solve(Solver *solv, Queue *job);
extern int solver_dep_installed(Solver *solv, Id dep);
extern int solver_splitprovides(Solver *solv, Id dep);

extern Id solver_next_problem(Solver *solv, Id problem);
extern Id solver_next_solution(Solver *solv, Id problem, Id solution);
extern Id solver_next_solutionelement(Solver *solv, Id problem, Id solution, Id element, Id *p, Id *rp);
extern Id solver_findproblemrule(Solver *solv, Id problem);
extern SolverProbleminfo solver_problemruleinfo(Solver *solv, Queue *job, Id rid, Id *depp, Id *sourcep, Id *targetp);

/* XXX: why is this not static? */
Id *create_decisions_obsoletesmap(Solver *solv);

static inline int
solver_dep_fulfilled(Solver *solv, Id dep)
{
  Pool *pool = solv->pool;
  Id p, *pp;

  if (ISRELDEP(dep))
    {
      Reldep *rd = GETRELDEP(pool, dep);
      if (rd->flags == REL_AND)
        {
          if (!solver_dep_fulfilled(solv, rd->name))
            return 0;
          return solver_dep_fulfilled(solv, rd->evr);
        }
      if (rd->flags == REL_NAMESPACE && rd->name == NAMESPACE_SPLITPROVIDES)
        return solver_splitprovides(solv, rd->evr);
      if (rd->flags == REL_NAMESPACE && rd->name == NAMESPACE_INSTALLED)
        return solver_dep_installed(solv, rd->evr);
    }
  FOR_PROVIDES(p, pp, dep)
    {
      if (solv->decisionmap[p] > 0)
        return 1;
    }
  return 0;
}

static inline int
solver_is_supplementing(Solver *solv, Solvable *s)
{
  Id sup, *supp;
  if (!s->supplements && !s->freshens)
    return 0;
  if (s->supplements)
    {
      supp = s->repo->idarraydata + s->supplements;
      while ((sup = *supp++) != 0)
        if (solver_dep_fulfilled(solv, sup))
          break;
      if (!sup)
        return 0;
    }
  if (s->freshens)
    {
      supp = s->repo->idarraydata + s->freshens;
      while ((sup = *supp++) != 0)
        if (solver_dep_fulfilled(solv, sup))
          break;
      if (!sup)
        return 0;
    }
  return 1;
}

static inline int
solver_is_enhancing(Solver *solv, Solvable *s)
{
  Id enh, *enhp;
  if (!s->enhances)
    return 0;
  enhp = s->repo->idarraydata + s->enhances;
  while ((enh = *enhp++) != 0)
    if (solver_dep_fulfilled(solv, enh))
      return 1;
  return 0;
}

void solver_calc_duchanges(Solver *solv, DUChanges *mps, int nmps);
int solver_calc_installsizechange(Solver *solv);

static inline void
solver_create_state_maps(Solver *solv, Map *installedmap, Map *conflictsmap)
{
  pool_create_state_maps(solv->pool, &solv->decisionq, installedmap, conflictsmap);
}

#ifdef __cplusplus
}
#endif

#endif /* SATSOLVER_SOLVER_H */
