/*
 * Copyright (c) 2007-2009, Novell Inc.
 *
 * This program is licensed under the BSD license, read LICENSE.BSD
 * for further information
 */

/*
 * problems.c
 *
 */

#include <stdio.h>
#include <stdlib.h>
#include <unistd.h>
#include <string.h>
#include <assert.h>

#include "solver.h"
#include "bitmap.h"
#include "pool.h"
#include "util.h"
#include "evr.h"
#include "solverdebug.h"


/**********************************************************************************/

/* a problem is an item on the solver's problem list. It can either be >0, in that
 * case it is a update/infarch/dup rule, or it can be <0, which makes it refer to a job
 * consisting of multiple job rules.
 */

void
solver_disableproblem(Solver *solv, Id v)
{
  Rule *r;
  int i;
  Id *jp;

  if (v > 0)
    {
      if (v >= solv->infarchrules && v < solv->infarchrules_end)
	{
	  Pool *pool = solv->pool;
	  Id name = pool->solvables[-solv->rules[v].p].name;
	  while (v > solv->infarchrules && pool->solvables[-solv->rules[v - 1].p].name == name)
	    v--;
	  for (; v < solv->infarchrules_end && pool->solvables[-solv->rules[v].p].name == name; v++)
	    solver_disablerule(solv, solv->rules + v);
	  return;
	}
      if (v >= solv->duprules && v < solv->duprules_end)
	{
	  Pool *pool = solv->pool;
	  Id name = pool->solvables[-solv->rules[v].p].name;
	  while (v > solv->duprules && pool->solvables[-solv->rules[v - 1].p].name == name)
	    v--;
	  for (; v < solv->duprules_end && pool->solvables[-solv->rules[v].p].name == name; v++)
	    solver_disablerule(solv, solv->rules + v);
	  return;
	}
      solver_disablerule(solv, solv->rules + v);
#if 0
      /* XXX: doesn't work */
      if (v >= solv->updaterules && v < solv->updaterules_end)
	{
	  /* enable feature rule if we disabled the update rule */
	  r = solv->rules + (v - solv->updaterules + solv->featurerules);
	  if (r->p)
	    solver_enablerule(solv, r);
	}
#endif
      return;
    }
  v = -(v + 1);
  jp = solv->ruletojob.elements;
  for (i = solv->jobrules, r = solv->rules + i; i < solv->jobrules_end; i++, r++, jp++)
    if (*jp == v)
      solver_disablerule(solv, r);
}

/*-------------------------------------------------------------------
 * enableproblem
 */

void
solver_enableproblem(Solver *solv, Id v)
{
  Rule *r;
  int i;
  Id *jp;

  if (v > 0)
    {
      if (v >= solv->infarchrules && v < solv->infarchrules_end)
	{
	  Pool *pool = solv->pool;
	  Id name = pool->solvables[-solv->rules[v].p].name;
	  while (v > solv->infarchrules && pool->solvables[-solv->rules[v - 1].p].name == name)
	    v--;
	  for (; v < solv->infarchrules_end && pool->solvables[-solv->rules[v].p].name == name; v++)
	    solver_enablerule(solv, solv->rules + v);
	  return;
	}
      if (v >= solv->duprules && v < solv->duprules_end)
	{
	  Pool *pool = solv->pool;
	  Id name = pool->solvables[-solv->rules[v].p].name;
	  while (v > solv->duprules && pool->solvables[-solv->rules[v - 1].p].name == name)
	    v--;
	  for (; v < solv->duprules_end && pool->solvables[-solv->rules[v].p].name == name; v++)
	    solver_enablerule(solv, solv->rules + v);
	  return;
	}
      if (v >= solv->featurerules && v < solv->featurerules_end)
	{
	  /* do not enable feature rule if update rule is enabled */
	  r = solv->rules + (v - solv->featurerules + solv->updaterules);
	  if (r->d >= 0)
	    return;
	}
      solver_enablerule(solv, solv->rules + v);
      if (v >= solv->updaterules && v < solv->updaterules_end)
	{
	  /* disable feature rule when enabling update rule */
	  r = solv->rules + (v - solv->updaterules + solv->featurerules);
	  if (r->p)
	    solver_disablerule(solv, r);
	}
      return;
    }
  v = -(v + 1);
  jp = solv->ruletojob.elements;
  for (i = solv->jobrules, r = solv->rules + i; i < solv->jobrules_end; i++, r++, jp++)
    if (*jp == v)
      solver_enablerule(solv, r);
}


/*-------------------------------------------------------------------
 * enable weak rules
 * 
 * Reenable all disabled weak rules (marked in weakrulemap)
 * 
 */

static void
enableweakrules(Solver *solv)
{
  int i;
  Rule *r;

  for (i = 1, r = solv->rules + i; i < solv->learntrules; i++, r++)
    {
      if (r->d >= 0) /* already enabled? */
	continue;
      if (!MAPTST(&solv->weakrulemap, i))
	continue;
      solver_enablerule(solv, r);
    }
}


/*-------------------------------------------------------------------
 * 
 * refine_suggestion
 * 
 * at this point, all rules that led to conflicts are disabled.
 * we re-enable all rules of a problem set but rule "sug", then
 * continue to disable more rules until there as again a solution.
 */

/* FIXME: think about conflicting assertions */

static void
refine_suggestion(Solver *solv, Id *problem, Id sug, Queue *refined, int essentialok)
{
  Pool *pool = solv->pool;
  int i, j;
  Id v;
  Queue disabled;
  int disabledcnt;

  IF_POOLDEBUG (SAT_DEBUG_SOLUTIONS)
    {
      POOL_DEBUG(SAT_DEBUG_SOLUTIONS, "refine_suggestion start\n");
      for (i = 0; problem[i]; i++)
	{
	  if (problem[i] == sug)
	    POOL_DEBUG(SAT_DEBUG_SOLUTIONS, "=> ");
	  solver_printproblem(solv, problem[i]);
	}
    }
  queue_empty(refined);
  if (!essentialok && sug < 0 && (solv->job.elements[-sug - 1] & SOLVER_ESSENTIAL) != 0)
    return;
  queue_init(&disabled);
  queue_push(refined, sug);

  /* re-enable all problem rules with the exception of "sug"(gestion) */
  solver_reset(solv);

  for (i = 0; problem[i]; i++)
    if (problem[i] != sug)
      solver_enableproblem(solv, problem[i]);

  if (sug < 0)
    solver_reenablepolicyrules(solv, -(sug + 1));
  else if (sug >= solv->updaterules && sug < solv->updaterules_end)
    {
      /* enable feature rule */
      Rule *r = solv->rules + solv->featurerules + (sug - solv->updaterules);
      if (r->p)
	solver_enablerule(solv, r);
    }

  enableweakrules(solv);

  for (;;)
    {
      int njob, nfeature, nupdate;
      queue_empty(&solv->problems);
      solver_reset(solv);

      if (!solv->problems.count)
        solver_run_sat(solv, 0, 0);

      if (!solv->problems.count)
	{
	  POOL_DEBUG(SAT_DEBUG_SOLUTIONS, "no more problems!\n");
	  IF_POOLDEBUG (SAT_DEBUG_SCHUBI)
	    solver_printdecisions(solv);
	  break;		/* great, no more problems */
	}
      disabledcnt = disabled.count;
      /* start with 1 to skip over proof index */
      njob = nfeature = nupdate = 0;
      for (i = 1; i < solv->problems.count - 1; i++)
	{
	  /* ignore solutions in refined */
          v = solv->problems.elements[i];
	  if (v == 0)
	    break;	/* end of problem reached */
	  for (j = 0; problem[j]; j++)
	    if (problem[j] != sug && problem[j] == v)
	      break;
	  if (problem[j])
	    continue;
	  if (v >= solv->featurerules && v < solv->featurerules_end)
	    nfeature++;
	  else if (v > 0)
	    nupdate++;
	  else
	    {
	      if (!essentialok && (solv->job.elements[-v -1] & SOLVER_ESSENTIAL) != 0)
		continue;	/* not that one! */
	      njob++;
	    }
	  queue_push(&disabled, v);
	}
      if (disabled.count == disabledcnt)
	{
	  /* no solution found, this was an invalid suggestion! */
	  POOL_DEBUG(SAT_DEBUG_SOLUTIONS, "no solution found!\n");
	  refined->count = 0;
	  break;
	}
      if (!njob && nupdate && nfeature)
	{
	  /* got only update rules, filter out feature rules */
	  POOL_DEBUG(SAT_DEBUG_SOLUTIONS, "throwing away feature rules\n");
	  for (i = j = disabledcnt; i < disabled.count; i++)
	    {
	      v = disabled.elements[i];
	      if (v < solv->featurerules || v >= solv->featurerules_end)
	        disabled.elements[j++] = v;
	    }
	  disabled.count = j;
	  nfeature = 0;
	}
      if (disabled.count == disabledcnt + 1)
	{
	  /* just one suggestion, add it to refined list */
	  v = disabled.elements[disabledcnt];
	  if (!nfeature)
	    queue_push(refined, v);	/* do not record feature rules */
	  solver_disableproblem(solv, v);
	  if (v >= solv->updaterules && v < solv->updaterules_end)
	    {
	      Rule *r = solv->rules + (v - solv->updaterules + solv->featurerules);
	      if (r->p)
		solver_enablerule(solv, r);	/* enable corresponding feature rule */
	    }
	  if (v < 0)
	    solver_reenablepolicyrules(solv, -(v + 1));
	}
      else
	{
	  /* more than one solution, disable all */
	  /* do not push anything on refine list, as we do not know which solution to choose */
	  /* thus, the user will get another problem if he selects this solution, where he
           * can choose the right one */
	  IF_POOLDEBUG (SAT_DEBUG_SOLUTIONS)
	    {
	      POOL_DEBUG(SAT_DEBUG_SOLUTIONS, "more than one solution found:\n");
	      for (i = disabledcnt; i < disabled.count; i++)
		solver_printproblem(solv, disabled.elements[i]);
	    }
	  for (i = disabledcnt; i < disabled.count; i++)
	    {
	      v = disabled.elements[i];
	      solver_disableproblem(solv, v);
	      if (v >= solv->updaterules && v < solv->updaterules_end)
		{
		  Rule *r = solv->rules + (v - solv->updaterules + solv->featurerules);
		  if (r->p)
		    solver_enablerule(solv, r);
		}
	    }
	}
    }
  /* all done, get us back into the same state as before */
  /* enable refined rules again */
  for (i = 0; i < disabled.count; i++)
    solver_enableproblem(solv, disabled.elements[i]);
  queue_free(&disabled);
  /* reset policy rules */
  for (i = 0; problem[i]; i++)
    solver_enableproblem(solv, problem[i]);
  solver_disablepolicyrules(solv);
  /* disable problem rules again */
  for (i = 0; problem[i]; i++)
    solver_disableproblem(solv, problem[i]);
  POOL_DEBUG(SAT_DEBUG_SOLUTIONS, "refine_suggestion end\n");
}


/*-------------------------------------------------------------------
 * sorting helper for problems
 *
 * bring update rules before job rules
 * make essential job rules last
 */

static int
problems_sortcmp(const void *ap, const void *bp, void *dp)
{
  Queue *job = dp;
  Id a = *(Id *)ap, b = *(Id *)bp;
  if (a < 0 && b > 0)
    return 1;
  if (a > 0 && b < 0)
    return -1;
  if (a < 0 && b < 0)
    {
      int af = job->elements[-a - 1] & SOLVER_ESSENTIAL;
      int bf = job->elements[-b - 1] & SOLVER_ESSENTIAL;
      int x = af - bf;
      if (x)
	return x;
    }
  return a - b;
}

/*
 * convert a solution rule into a job modifier
 */
static void
convertsolution(Solver *solv, Id why, Queue *solutionq)
{
  Pool *pool = solv->pool;
  if (why < 0)
    {
      queue_push(solutionq, 0);
      queue_push(solutionq, -why);
      return;
    }
  if (why >= solv->infarchrules && why < solv->infarchrules_end)
    {
      Id p, name;
      /* infarch rule, find replacement */
      assert(solv->rules[why].p < 0);
      name = pool->solvables[-solv->rules[why].p].name;
      while (why > solv->infarchrules && pool->solvables[-solv->rules[why - 1].p].name == name)
	why--;
      p = 0;
      for (; why < solv->infarchrules_end && pool->solvables[-solv->rules[why].p].name == name; why++)
	if (solv->decisionmap[-solv->rules[why].p] > 0)
	  {
	    p = -solv->rules[why].p;
	    break;
	  }
      if (!p)
	return;		/* false alarm */
      queue_push(solutionq, SOLVER_SOLUTION_INFARCH);
      queue_push(solutionq, p);
      return;
    }
  if (why >= solv->duprules && why < solv->duprules_end)
    {
      Id p, name;
      /* dist upgrade rule, find replacement */
      assert(solv->rules[why].p < 0);
      name = pool->solvables[-solv->rules[why].p].name;
      while (why > solv->duprules && pool->solvables[-solv->rules[why - 1].p].name == name)
	why--;
      p = 0;
      for (; why < solv->duprules_end && pool->solvables[-solv->rules[why].p].name == name; why++)
	if (solv->decisionmap[-solv->rules[why].p] > 0)
	  {
	    p = -solv->rules[why].p;
	    break;
	  }
      if (!p)
	return;		/* false alarm */
      queue_push(solutionq, SOLVER_SOLUTION_DISTUPGRADE);
      queue_push(solutionq, p);
      return;
    }
  if (why >= solv->updaterules && why < solv->updaterules_end)
    {
      /* update rule, find replacement package */
      Id p, *dp, rp = 0;
      Rule *rr;

      assert(why >= solv->updaterules && why < solv->updaterules_end);
      /* check if this is a false positive, i.e. the update rule is fulfilled */
      rr = solv->rules + why;
      FOR_RULELITERALS(p, dp, rr)
	if (p > 0 && solv->decisionmap[p] > 0)
	  break;
      if (p)
	return;		/* false alarm */

      p = solv->installed->start + (why - solv->updaterules);
      rr = solv->rules + solv->featurerules + (why - solv->updaterules);
      if (!rr->p)
	rr = solv->rules + why;
      if (solv->dupmap_all && solv->rules[why].p != p && solv->decisionmap[p] > 0)
	{
	  /* distupgrade case, allow to keep old package */
	  queue_push(solutionq, SOLVER_SOLUTION_DISTUPGRADE);
	  queue_push(solutionq, p);
	  return;
	}
      if (solv->decisionmap[p] > 0)
	return;		/* false alarm, turned out we can keep the package */
      if (rr->w2)
	{
	  int mvrp = 0;		/* multi-version replacement */
	  FOR_RULELITERALS(rp, dp, rr)
	    {
	      if (rp > 0 && solv->decisionmap[rp] > 0 && pool->solvables[rp].repo != solv->installed)
		{
		  mvrp = rp;
		  if (!(solv->noobsoletes.size && MAPTST(&solv->noobsoletes, rp)))
		    break;
		}
	    }
	  if (!rp && mvrp)
	    {
	      /* found only multi-version replacements */
	      /* have to split solution into two parts */
	      queue_push(solutionq, p);
	      queue_push(solutionq, mvrp);
	    }
	}
      queue_push(solutionq, p);
      queue_push(solutionq, rp);
      return;
    }
}

/*
 * convert problem data into a form usable for refining.
 * Returns the number of problems.
 */
int
solver_prepare_solutions(Solver *solv)
{
  int i, j = 1, idx = 1;  

  if (!solv->problems.count)
    return 0;
  queue_push(&solv->solutions, 0); 
  queue_push(&solv->solutions, -1); /* unrefined */
  for (i = 1; i < solv->problems.count; i++) 
    {   
      Id p = solv->problems.elements[i];
      queue_push(&solv->solutions, p); 
      if (p) 
        continue;
      solv->problems.elements[j++] = idx; 
      if (i + 1 >= solv->problems.count)
        break;
      solv->problems.elements[j++] = solv->problems.elements[++i];  /* copy proofidx */
      idx = solv->solutions.count;
      queue_push(&solv->solutions, -1); 
    }   
  solv->problems.count = j;  
  return j / 2;
}

/*
 * refine the simple solution rule list provided by
 * the solver into multiple lists of job modifiers.
 */
static void
create_solutions(Solver *solv, int probnr, int solidx)
{
  Pool *pool = solv->pool;
  Queue redoq;
  Queue problem, solution, problems_save;
  int i, j, nsol;
  int essentialok;
  int recocount;
  unsigned int now;

  now = sat_timems(0);
  recocount = solv->recommendations.count;
  solv->recommendations.count = 0;	/* so that revert() doesn't mess with it later */
  queue_init(&redoq);
  /* save decisionq, decisionq_why, decisionmap */
  for (i = 0; i < solv->decisionq.count; i++)
    {
      Id p = solv->decisionq.elements[i];
      queue_push(&redoq, p);
      queue_push(&redoq, solv->decisionq_why.elements[i]);
      queue_push(&redoq, solv->decisionmap[p > 0 ? p : -p]);
    }
  /* save problems queue */
  problems_save = solv->problems;
  memset(&solv->problems, 0, sizeof(solv->problems));

  /* extract problem from queue */
  queue_init(&problem);
  for (i = solidx + 1; i < solv->solutions.count; i++)
    {
      Id v = solv->solutions.elements[i];
      if (!v)
	break;
      queue_push(&problem, v);
    }
  if (problem.count > 1)
    sat_sort(problem.elements, problem.count, sizeof(Id), problems_sortcmp, &solv->job);
  queue_push(&problem, 0);	/* mark end for refine_suggestion */
  problem.count--;
#if 0
  for (i = 0; i < problem.count; i++)
    printf("PP %d %d\n", i, problem.elements[i]);
#endif

  /* refine each solution element */
  nsol = 0;
  essentialok = 0;
  queue_init(&solution);
  for (i = 0; i < problem.count; i++)
    {
      int solstart = solv->solutions.count;
      refine_suggestion(solv, problem.elements, problem.elements[i], &solution, essentialok);
      queue_push(&solv->solutions, 0);	/* reserve room for number of elements */
      for (j = 0; j < solution.count; j++)
	convertsolution(solv, solution.elements[j], &solv->solutions);
      if (solv->solutions.count == solstart + 1)
	{
	  solv->solutions.count--;
	  if (!essentialok && i + 1 == problem.count && !nsol)
	    {
	      /* nothing found, start over */
	      essentialok = 1;
	      i = -1;
	    }
	  continue;
	}
      /* patch in number of solution elements */
      solv->solutions.elements[solstart] = (solv->solutions.count - (solstart + 1)) / 2;
      queue_push(&solv->solutions, 0);	/* add end marker */
      queue_push(&solv->solutions, 0);	/* add end marker */
      solv->solutions.elements[solidx + 1 + nsol++] = solstart;
    }
  solv->solutions.elements[solidx + 1 + nsol] = 0;	/* end marker */
  solv->solutions.elements[solidx] = nsol;
  queue_free(&problem);
  queue_free(&solution);

  /* restore decisions */
  memset(solv->decisionmap, 0, pool->nsolvables * sizeof(Id));
  queue_empty(&solv->decisionq);
  queue_empty(&solv->decisionq_why);
  for (i = 0; i < redoq.count; i += 3)
    {
      Id p = redoq.elements[i];
      queue_push(&solv->decisionq, p);
      queue_push(&solv->decisionq_why, redoq.elements[i + 1]);
      solv->decisionmap[p > 0 ? p : -p] = redoq.elements[i + 2];
    }
  solv->recommendations.count = recocount;
  queue_free(&redoq);
  /* restore problems */
  queue_free(&solv->problems);
  solv->problems = problems_save;
  POOL_DEBUG(SAT_DEBUG_STATS, "create_solutions for problem #%d took %d ms\n", probnr, sat_timems(now));
}


/**************************************************************************/

unsigned int
solver_problem_count(Solver *solv)
{
  return solv->problems.count / 2;
}

Id
solver_next_problem(Solver *solv, Id problem)
{
  if (!problem)
    return solv->problems.count ? 1 : 0;
  return (problem + 1) * 2 - 1 < solv->problems.count ? problem + 1 : 0;
}

unsigned int
solver_solution_count(Solver *solv, Id problem)
{
  Id solidx = solv->problems.elements[problem * 2 - 1];
  if (solv->solutions.elements[solidx] < 0)
    create_solutions(solv, problem, solidx);
  return solv->solutions.elements[solidx];
}

Id
solver_next_solution(Solver *solv, Id problem, Id solution)
{
  Id solidx = solv->problems.elements[problem * 2 - 1];
  if (solv->solutions.elements[solidx] < 0)
    create_solutions(solv, problem, solidx);
  return solv->solutions.elements[solidx + solution + 1] ? solution + 1 : 0;
}

unsigned int
solver_solutionelement_count(Solver *solv, Id problem, Id solution)
{
  Id solidx = solv->problems.elements[problem * 2 - 1];
  solidx = solv->solutions.elements[solidx + solution];
  return solv->solutions.elements[solidx];
}


/*
 *  return the next item of the proposed solution
 *  here are the possibilities for p / rp and what
 *  the solver expects the application to do:
 *    p                             rp
 *  -------------------------------------------------------
 *    SOLVER_SOLUTION_INFARCH       pkgid
 *    -> add (SOLVER_INSTALL|SOLVER_SOLVABLE, rp) to the job
 *    SOLVER_SOLUTION_DISTUPGRADE   pkgid
 *    -> add (SOLVER_INSTALL|SOLVER_SOLVABLE, rp) to the job
 *    SOLVER_SOLUTION_JOB           jobidx
 *    -> remove job (jobidx - 1, jobidx) from job queue
 *    pkgid (> 0)                   0
 *    -> add (SOLVER_ERASE|SOLVER_SOLVABLE, p) to the job
 *    pkgid (> 0)                   pkgid (> 0)
 *    -> add (SOLVER_INSTALL|SOLVER_SOLVABLE, rp) to the job
 *       (this will replace package p)
 *         
 * Thus, the solver will either ask the application to remove
 * a specific job from the job queue, or ask to add an install/erase
 * job to it.
 *
 */

Id
solver_next_solutionelement(Solver *solv, Id problem, Id solution, Id element, Id *p, Id *rp)
{
  Id solidx = solv->problems.elements[problem * 2 - 1];
  solidx = solv->solutions.elements[solidx + solution];
  if (!solidx)
    return 0;
  solidx += 1 + element * 2;
  if (!solv->solutions.elements[solidx] && !solv->solutions.elements[solidx + 1])
    return 0;
  *p = solv->solutions.elements[solidx];
  *rp = solv->solutions.elements[solidx + 1];
  return element + 1;
}

void
solver_take_solutionelement(Solver *solv, Id p, Id rp, Queue *job)
{
  int i;

  if (p == SOLVER_SOLUTION_JOB)
    {
      job->elements[rp - 1] = SOLVER_NOOP;
      job->elements[rp] = 0;
      return;
    }
  if (rp <= 0 && p <= 0)
    return;	/* just in case */
  if (rp > 0)
    p = SOLVER_INSTALL|SOLVER_SOLVABLE;
  else
    {
      rp = p;
      p = SOLVER_ERASE|SOLVER_SOLVABLE;
    }
  for (i = 0; i < job->count; i += 2)
    if (job->elements[i] == p && job->elements[i + 1] == rp)
      return;
  queue_push2(job, p, rp);
}

void
solver_take_solution(Solver *solv, Id problem, Id solution, Queue *job)
{
  Id p, rp, element = 0;
  while ((element = solver_next_solutionelement(solv, problem, solution, element, &p, &rp)) != 0)
    solver_take_solutionelement(solv, p, rp, job);
}


/*-------------------------------------------------------------------
 * 
 * find problem rule
 */

static void
findproblemrule_internal(Solver *solv, Id idx, Id *reqrp, Id *conrp, Id *sysrp, Id *jobrp)
{
  Id rid, d;
  Id lreqr, lconr, lsysr, ljobr;
  Rule *r;
  Id jobassert = 0;
  int i, reqset = 0;	/* 0: unset, 1: installed, 2: jobassert, 3: assert */

  /* find us a jobassert rule */
  for (i = idx; (rid = solv->learnt_pool.elements[i]) != 0; i++)
    {
      if (rid < solv->jobrules || rid >= solv->jobrules_end)
	continue;
      r = solv->rules + rid;
      d = r->d < 0 ? -r->d - 1 : r->d;
      if (!d && r->w2 == 0 && r->p > 0)
	{
	  jobassert = r->p;
	  break;
	}
    }

  /* the problem rules are somewhat ordered from "near to the problem" to
   * "near to the job" */
  lreqr = lconr = lsysr = ljobr = 0;
  while ((rid = solv->learnt_pool.elements[idx++]) != 0)
    {
      assert(rid > 0);
      if (rid >= solv->learntrules)
	findproblemrule_internal(solv, solv->learnt_why.elements[rid - solv->learntrules], &lreqr, &lconr, &lsysr, &ljobr);
      else if ((rid >= solv->jobrules && rid < solv->jobrules_end) || (rid >= solv->infarchrules && rid < solv->infarchrules_end) || (rid >= solv->duprules && rid < solv->duprules_end))
	{
	  if (!*jobrp)
	    *jobrp = rid;
	}
      else if (rid >= solv->updaterules && rid < solv->updaterules_end)
	{
	  if (!*sysrp)
	    *sysrp = rid;
	}
      else
	{
	  assert(rid < solv->rpmrules_end);
	  r = solv->rules + rid;
	  d = r->d < 0 ? -r->d - 1 : r->d;
	  if (!d && r->w2 < 0)
	    {
	      if (!*conrp)
		*conrp = rid;
	    }
	  else
	    {
	      if (!d && r->w2 == 0 && reqset < 3)
		{
		  if (*reqrp > 0 && r->p < -1)
		    {
		      Id op = -solv->rules[*reqrp].p;
		      if (op > 1 && solv->pool->solvables[op].arch != solv->pool->solvables[-r->p].arch)
			continue;	/* different arch, skip */
		    }
		  /* prefer assertions */
		  *reqrp = rid;
		  reqset = 3;
		}
	      else if (jobassert && r->p == -jobassert)
		{
		  /* prefer rules of job assertions */
		  *reqrp = rid;
		  reqset = 2;
		}
	      else if (solv->installed && r->p < 0 && solv->pool->solvables[-r->p].repo == solv->installed && reqset <= 1)
		{
		  /* prefer rules of job installed package so that the user doesn't get confused by strange packages */
		  *reqrp = rid;
		  reqset = 1;
		}
	      else if (!*reqrp)
		*reqrp = rid;
	    }
	}
    }
  if (!*reqrp && lreqr)
    *reqrp = lreqr;
  if (!*conrp && lconr)
    *conrp = lconr;
  if (!*jobrp && ljobr)
    *jobrp = ljobr;
  if (!*sysrp && lsysr)
    *sysrp = lsysr;
}

/* 
 * find problem rule
 *
 * search for a rule that describes the problem to the
 * user. Actually a pretty hopeless task that may leave the user
 * puzzled. To get all of the needed information use
 * solver_findallproblemrules() instead.
 */

Id
solver_findproblemrule(Solver *solv, Id problem)
{
  Id reqr, conr, sysr, jobr;
  Id idx = solv->problems.elements[2 * problem - 2];
  reqr = conr = sysr = jobr = 0;
  findproblemrule_internal(solv, idx, &reqr, &conr, &sysr, &jobr);
  if (reqr)
    return reqr;	/* some requires */
  if (conr)
    return conr;	/* some conflict */
  if (sysr)
    return sysr;	/* an update rule */
  if (jobr)
    return jobr;	/* a user request */
  assert(0);
}

/*-------------------------------------------------------------------*/

static void
findallproblemrules_internal(Solver *solv, Id idx, Queue *rules)
{
  Id rid;
  while ((rid = solv->learnt_pool.elements[idx++]) != 0)
    {
      if (rid >= solv->learntrules)
        {
	  findallproblemrules_internal(solv, solv->learnt_why.elements[rid - solv->learntrules], rules);
          continue;
	}
      queue_pushunique(rules, rid);
    }
}

/*
 * find all problem rule
 *
 * return all rules that lead to the problem. This gives the user
 * all of the information to understand the problem, but the result
 * can be a large number of rules.
 */

void
solver_findallproblemrules(Solver *solv, Id problem, Queue *rules)
{
  queue_empty(rules);
  findallproblemrules_internal(solv, solv->problems.elements[2 * problem - 2], rules);
}

/* obsolete function */
SolverRuleinfo
solver_problemruleinfo(Solver *solv, Queue *job, Id rid, Id *depp, Id *sourcep, Id *targetp)
{
  return solver_ruleinfo(solv, rid, sourcep, targetp, depp);
}

/* EOF */
