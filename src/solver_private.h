/*
 * Copyright (c) 2011, Novell Inc.
 *
 * This program is licensed under the BSD license, read LICENSE.BSD
 * for further information
 */

/*
 * solver_p.h - private functions
 *
 */

#ifndef LIBSOLV_SOLVER_P_H
#define LIBSOLV_SOLVER_P_H

#if 0
struct _Solver {
  Pool *pool;				/* back pointer to pool */
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
    
  Id choicerules;			/* choice rules (always weak) */
  Id choicerules_end;
  Id *choicerules_ref;

  Id learntrules;			/* learnt rules, (end == nrules) */

  Map noupdate;				/* don't try to update these
                                           installed solvables */
  Map noobsoletes;			/* ignore obsoletes for these (multiinstall) */

  Map updatemap;			/* bring these installed packages to the newest version */
  int updatemap_all;			/* bring all packages to the newest version */

  Map fixmap;				/* fix these packages */
  int fixmap_all;			/* fix all packages */

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

  int decisioncnt_weak;
  int decisioncnt_orphan;

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
    
  Map dupmap;				/* dup these packages*/
  int dupmap_all;			/* dup all packages */
  Map dupinvolvedmap;			/* packages involved in dup process */

  Map droporphanedmap;			/* packages to drop in dup mode */
  int droporphanedmap_all;

  Map cleandepsmap;			/* try to drop these packages as of cleandeps erases */

  Queue *ruleinfoq;			/* tmp space for solver_ruleinfo() */
};
#endif

extern void solver_run_sat(Solver *solv, int disablerules, int doweak);
extern void solver_reset(Solver *solv);

extern int solver_dep_installed(Solver *solv, Id dep);
extern int solver_splitprovides(Solver *solv, Id dep);

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

#endif /* LIBSOLV_SOLVER_P_H */
