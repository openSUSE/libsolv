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
#include "transaction.h"
#include "rules.h"
#include "problems.h"

/*
 * Callback definitions in order to "overwrite" the policies by an external application.
 */
 
typedef void  (*BestSolvableCb) (Pool *pool, Queue *canditates);
typedef int  (*ArchCheckCb) (Pool *pool, Solvable *solvable1, Solvable *solvable2);
typedef int  (*VendorCheckCb) (Pool *pool, Solvable *solvable1, Solvable *solvable2);
typedef void (*UpdateCandidateCb) (Pool *pool, Solvable *solvable, Queue *canditates);


struct _Solver;

typedef struct _Solver {
  Pool *pool;
  Queue job;				/* copy of the job we're solving */

  Transaction trans;			/* calculated transaction */

  Repo *installed;			/* copy of pool->installed */
  
  /* list of rules, ordered
   * rpm rules first, then features, updates, jobs, learnt
   * see start/end offsets below
   */
  Rule *rules;				/* all rules */
  Id nrules;				/* [Offset] index of the last rule */

  Queue ruleassertions;			/* Queue of all assertion rules */

  /* start/end offset for rule 'areas' */
    
  Id rpmrules_end;                      /* [Offset] rpm rules end */
    
  Id featurerules;			/* feature rules start/end */
  Id featurerules_end;
    
  Id updaterules;			/* policy rules, e.g. keep packages installed or update. All literals > 0 */
  Id updaterules_end;
    
  Id jobrules;				/* user rules */
  Id jobrules_end;

  Id infarchrules;			/* inferior arch rules */
  Id infarchrules_end;

  Id duprules;				/* dist upgrade rules */
  Id duprules_end;
    
  Id learntrules;			/* learnt rules, (end == nrules) */

  Map noupdate;				/* don't try to update these
                                           installed solvables */
  Map noobsoletes;			/* ignore obsoletes for these
					   (multiinstall) */

  Map updatemap;			/* bring those packages to the newest version */
  Queue weakruleq;			/* index into 'rules' for weak ones */
  Map weakrulemap;			/* map rule# to '1' for weak rules, 1..learntrules */

  Id *watches;				/* Array of rule offsets
					 * watches has nsolvables*2 entries and is addressed from the middle
					 * middle-solvable : decision to conflict, offset point to linked-list of rules
					 * middle+solvable : decision to install: offset point to linked-list of rules
					 */

  Queue ruletojob;                      /* index into job queue: jobs for which a rule exits */

  /* our decisions: */
  Queue decisionq;                      /* >0:install, <0:remove/conflict */
  Queue decisionq_why;			/* index of rule, Offset into rules */

  Id *decisionmap;			/* map for all available solvables,
					 * = 0: undecided
					 * > 0: level of decision when installed,
					 * < 0: level of decision when conflict */

  /* learnt rule history */
  Queue learnt_why;
  Queue learnt_pool;

  Queue branches;
  int (*solution_callback)(struct _Solver *solv, void *data);
  void *solution_callback_data;

  int propagate_index;                  /* index into decisionq for non-propagated decisions */

  Queue problems;                       /* list of lists of conflicting rules, < 0 for job rules */
  Queue solutions;			/* refined problem storage space */

  Queue recommendations;		/* recommended packages */
  Queue suggestions;			/* suggested packages */
  Queue orphaned;			/* orphaned packages */

  int stats_learned;			/* statistic */
  int stats_unsolvable;			/* statistic */

  Map recommendsmap;			/* recommended packages from decisionmap */
  Map suggestsmap;			/* suggested packages from decisionmap */
  int recommends_index;			/* recommendsmap/suggestsmap is created up to this level */

  Id *obsoletes;			/* obsoletes for each installed solvable */
  Id *obsoletes_data;			/* data area for obsoletes */
  Id *multiversionupdaters;		/* updaters for multiversion packages in updatesystem mode */

  /*-------------------------------------------------------------------------------------------------------------
   * Solver configuration
   *-------------------------------------------------------------------------------------------------------------*/

  int fixsystem;			/* repair errors in rpm dependency graph */
  int allowdowngrade;			/* allow to downgrade installed solvable */
  int allowarchchange;			/* allow to change architecture of installed solvables */
  int allowvendorchange;		/* allow to change vendor of installed solvables */
  int allowuninstall;			/* allow removal of installed solvables */
  int updatesystem;			/* update all packages to the newest version */
  int noupdateprovide;			/* true: update packages needs not to provide old package */
  int dosplitprovides;			/* true: consider legacy split provides */
  int dontinstallrecommended;		/* true: do not install recommended packages */
  int ignorealreadyrecommended;		/* true: ignore recommended packages that were already recommended by the installed packages */
  int dontshowinstalledrecommended;	/* true: do not show recommended packages that are already installed */
  
  /* distupgrade also needs updatesystem and dosplitprovides */
  int distupgrade;
  int distupgrade_removeunsupported;

  int noinfarchcheck;			/* true: do not forbid inferior architectures */

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

  
  Map dupmap;				/* packages from dup repos */
  Map dupinvolvedmap;			/* packages involved in dup process */

  Queue *ruleinfoq;			/* tmp space for solver_ruleinfo() */
} Solver;

/*
 * queue commands
 */

#define SOLVER_SOLVABLE			0x01
#define SOLVER_SOLVABLE_NAME		0x02
#define SOLVER_SOLVABLE_PROVIDES	0x03
#define SOLVER_SOLVABLE_ONE_OF		0x04

#define SOLVER_SELECTMASK		0xff

#define SOLVER_NOOP			0x0000
#define SOLVER_INSTALL       		0x0100
#define SOLVER_ERASE         		0x0200
#define SOLVER_UPDATE			0x0300
#define SOLVER_WEAKENDEPS      		0x0400
#define SOLVER_NOOBSOLETES   		0x0500
#define SOLVER_LOCK			0x0600
#define SOLVER_DISTUPGRADE		0x0700

#define SOLVER_JOBMASK			0xff00

#define SOLVER_WEAK			0x010000
#define SOLVER_ESSENTIAL		0x020000

/* old API compatibility, do not use in new code */
#if 1
#define SOLVER_INSTALL_SOLVABLE (SOLVER_INSTALL|SOLVER_SOLVABLE)
#define SOLVER_ERASE_SOLVABLE (SOLVER_ERASE|SOLVER_SOLVABLE)
#define SOLVER_INSTALL_SOLVABLE_NAME (SOLVER_INSTALL|SOLVER_SOLVABLE_NAME)
#define SOLVER_ERASE_SOLVABLE_NAME (SOLVER_ERASE|SOLVER_SOLVABLE_NAME)
#define SOLVER_INSTALL_SOLVABLE_PROVIDES (SOLVER_INSTALL|SOLVER_SOLVABLE_PROVIDES)
#define SOLVER_ERASE_SOLVABLE_PROVIDES (SOLVER_ERASE|SOLVER_SOLVABLE_PROVIDES)
#define SOLVER_INSTALL_SOLVABLE_UPDATE (SOLVER_UPDATE|SOLVER_SOLVABLE)
#define SOLVER_INSTALL_SOLVABLE_ONE_OF (SOLVER_INSTALL|SOLVER_SOLVABLE_ONE_OF)
#define SOLVER_WEAKEN_SOLVABLE_DEPS (SOLVER_WEAKENDEPS|SOLVER_SOLVABLE)
#define SOLVER_NOOBSOLETES_SOLVABLE (SOLVER_NOOBSOLETES|SOLVER_SOLVABLE)
#define SOLVER_NOOBSOLETES_SOLVABLE_NAME (SOLVER_NOOBSOLETES|SOLVER_SOLVABLE_NAME)
#define SOLVER_NOOBSOLETES_SOLVABLE_PROVIDES (SOLVER_NOOBSOLETES|SOLVER_SOLVABLE_PROVIDES)
#endif

/* backward compatibility */
#define SOLVER_PROBLEM_UPDATE_RULE 		SOLVER_RULE_UPDATE
#define SOLVER_PROBLEM_JOB_RULE			SOLVER_RULE_JOB
#define SOLVER_PROBLEM_JOB_NOTHING_PROVIDES_DEP	SOLVER_RULE_JOB_NOTHING_PROVIDES_DEP
#define SOLVER_PROBLEM_NOT_INSTALLABLE		SOLVER_RULE_RPM_NOT_INSTALLABLE
#define SOLVER_PROBLEM_NOTHING_PROVIDES_DEP	SOLVER_RULE_RPM_NOTHING_PROVIDES_DEP
#define SOLVER_PROBLEM_SAME_NAME		SOLVER_RULE_RPM_SAME_NAME
#define SOLVER_PROBLEM_PACKAGE_CONFLICT		SOLVER_RULE_RPM_PACKAGE_CONFLICT
#define SOLVER_PROBLEM_PACKAGE_OBSOLETES	SOLVER_RULE_RPM_PACKAGE_OBSOLETES
#define SOLVER_PROBLEM_DEP_PROVIDERS_NOT_INSTALLABLE SOLVER_RULE_RPM_PACKAGE_REQUIRES
#define SOLVER_PROBLEM_SELF_CONFLICT		SOLVER_RULE_RPM_SELF_CONFLICT
#define SOLVER_PROBLEM_RPM_RULE			SOLVER_RULE_RPM
#define SOLVER_PROBLEM_DISTUPGRADE_RULE		SOLVER_RULE_DISTUPGRADE
#define SOLVER_PROBLEM_INFARCH_RULE		SOLVER_RULE_INFARCH


extern Solver *solver_create(Pool *pool);
extern void solver_free(Solver *solv);
extern void solver_solve(Solver *solv, Queue *job);

extern void solver_run_sat(Solver *solv, int disablerules, int doweak);
extern void solver_reset(Solver *solv);

extern int solver_dep_installed(Solver *solv, Id dep);
extern int solver_splitprovides(Solver *solv, Id dep);

extern void solver_calculate_noobsmap(Pool *pool, Queue *job, Map *noobsmap);

/* obsolete */
extern SolverRuleinfo solver_problemruleinfo(Solver *solv, Queue *job, Id rid, Id *depp, Id *sourcep, Id *targetp);

/* XXX: why is this not static? */
Id *solver_create_decisions_obsoletesmap(Solver *solv);

static inline int
solver_dep_fulfilled(Solver *solv, Id dep)
{
  Pool *pool = solv->pool;
  Id p, pp;

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
  if (!s->supplements)
    return 0;
  supp = s->repo->idarraydata + s->supplements;
  while ((sup = *supp++) != 0)
    if (solver_dep_fulfilled(solv, sup))
      return 1;
  return 0;
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
void solver_trivial_installable(Solver *solv, Queue *pkgs, Queue *res);

void solver_find_involved(Solver *solv, Queue *installedq, Solvable *s, Queue *q);

static inline void
solver_create_state_maps(Solver *solv, Map *installedmap, Map *conflictsmap)
{
  pool_create_state_maps(solv->pool, &solv->decisionq, installedmap, conflictsmap);
}

/* iterate over all literals of a rule */
/* WARNING: loop body must not relocate whatprovidesdata, e.g. by
 * looking up the providers of a dependency */
#define FOR_RULELITERALS(l, dp, r)				\
    for (l = r->d < 0 ? -r->d - 1 : r->d,			\
         dp = !l ? &r->w2 : pool->whatprovidesdata + l,		\
         l = r->p; l; l = (dp != &r->w2 + 1 ? *dp++ : 0))

/* iterate over all packages selected by a job */
#define FOR_JOB_SELECT(p, pp, select, what) \
    for (pp = (select == SOLVER_SOLVABLE ? 0 :	\
               select == SOLVER_SOLVABLE_ONE_OF ? what : \
               pool_whatprovides(pool, what)),				\
         p = (select == SOLVER_SOLVABLE ? what : pool->whatprovidesdata[pp++]) ; p ; p = pool->whatprovidesdata[pp++]) \
      if (select != SOLVER_SOLVABLE_NAME || pool_match_nevr(pool, pool->solvables + p, what))

#ifdef __cplusplus
}
#endif

#endif /* SATSOLVER_SOLVER_H */
