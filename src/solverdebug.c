/*
 * Copyright (c) 2008, Novell Inc.
 *
 * This program is licensed under the BSD license, read LICENSE.BSD
 * for further information
 */

/*
 * solverdebug.c
 *
 * debug functions for the SAT solver
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
#include "policy.h"


/*
 * create obsoletesmap from solver decisions
 *
 * for solvables in installed repo:
 *   0 - not obsoleted
 *   p - one of the packages that obsolete us
 * for all others:
 *   n - number of packages this package obsoletes
 *
 */

Id *
solver_create_decisions_obsoletesmap(Solver *solv)
{
  Pool *pool = solv->pool;
  Repo *installed = solv->installed;
  Id p, *obsoletesmap = NULL;
  int i;
  Solvable *s;

  obsoletesmap = (Id *)sat_calloc(pool->nsolvables, sizeof(Id));
  if (installed)
    {
      for (i = 0; i < solv->decisionq.count; i++)
	{
	  Id *pp, n;
	  int noobs;

	  n = solv->decisionq.elements[i];
	  if (n < 0)
	    continue;
	  if (n == SYSTEMSOLVABLE)
	    continue;
	  s = pool->solvables + n;
	  if (s->repo == installed)		/* obsoletes don't count for already installed packages */
	    continue;
	  noobs = solv->noobsoletes.size && MAPTST(&solv->noobsoletes, n);
	  FOR_PROVIDES(p, pp, s->name)
	    {
	      Solvable *ps = pool->solvables + p;
	      if (noobs && (s->name != ps->name || s->evr != ps->evr || s->arch != ps->arch))
		continue;
	      if (!solv->implicitobsoleteusesprovides && s->name != ps->name)
		continue;
	      if (pool->solvables[p].repo == installed && !obsoletesmap[p])
		{
		  obsoletesmap[p] = n;
		  obsoletesmap[n]++;
		}
	    }
	}
      for (i = 0; i < solv->decisionq.count; i++)
	{
	  Id obs, *obsp;
	  Id *pp, n;

	  n = solv->decisionq.elements[i];
	  if (n < 0)
	    continue;
	  if (n == SYSTEMSOLVABLE)
	    continue;
	  s = pool->solvables + n;
	  if (s->repo == installed)		/* obsoletes don't count for already installed packages */
	    continue;
	  if (!s->obsoletes)
	    continue;
	  if (solv->noobsoletes.size && MAPTST(&solv->noobsoletes, n))
	    continue;
	  obsp = s->repo->idarraydata + s->obsoletes;
	  while ((obs = *obsp++) != 0)
	    {
	      FOR_PROVIDES(p, pp, obs)
		{
		  if (!solv->obsoleteusesprovides && !pool_match_nevr(pool, pool->solvables + p, obs))
		    continue;
		  if (pool->solvables[p].repo == installed && !obsoletesmap[p])
		    {
		      obsoletesmap[p] = n;
		      obsoletesmap[n]++;
		    }
		}
	    }
	}
    }
  return obsoletesmap;
}

void
solver_printruleelement(Solver *solv, int type, Rule *r, Id v)
{
  Pool *pool = solv->pool;
  Solvable *s;
  if (v < 0)
    {
      s = pool->solvables + -v;
      POOL_DEBUG(type, "    !%s [%d]", solvable2str(pool, s), -v);
    }
  else
    {
      s = pool->solvables + v;
      POOL_DEBUG(type, "    %s [%d]", solvable2str(pool, s), v);
    }
  if (r)
    {
      if (r->w1 == v)
	POOL_DEBUG(type, " (w1)");
      if (r->w2 == v)
	POOL_DEBUG(type, " (w2)");
    }
  if (solv->decisionmap[s - pool->solvables] > 0)
    POOL_DEBUG(type, " Install.level%d", solv->decisionmap[s - pool->solvables]);
  if (solv->decisionmap[s - pool->solvables] < 0)
    POOL_DEBUG(type, " Conflict.level%d", -solv->decisionmap[s - pool->solvables]);
  POOL_DEBUG(type, "\n");
}


/*
 * print rule
 */

void
solver_printrule(Solver *solv, int type, Rule *r)
{
  Pool *pool = solv->pool;
  int i;
  Id d, v;

  if (r >= solv->rules && r < solv->rules + solv->nrules)   /* r is a solver rule */
    POOL_DEBUG(type, "Rule #%d:", (int)(r - solv->rules));
  else
    POOL_DEBUG(type, "Rule:");		       /* r is any rule */
  if (r && r->d < 0)
    POOL_DEBUG(type, " (disabled)");
  POOL_DEBUG(type, "\n");
  d = r->d < 0 ? -r->d - 1 : r->d;
  for (i = 0; ; i++)
    {
      if (i == 0)
	  /* print direct literal */
	v = r->p;
      else if (!d)
	{
	  if (i == 2)
	    break;
	  /* binary rule --> print w2 as second literal */
	  v = r->w2;
	}
      else
	  /* every other which is in d */
	v = solv->pool->whatprovidesdata[d + i - 1];
      if (v == ID_NULL)
	break;
      solver_printruleelement(solv, type, r, v);
    }
  POOL_DEBUG(type, "    next rules: %d %d\n", r->n1, r->n2);
}

void
solver_printruleclass(Solver *solv, int type, Rule *r)
{
  Pool *pool = solv->pool;
  Id p = r - solv->rules;
  assert(p >= 0);
  if (p < solv->learntrules)
    if (MAPTST(&solv->weakrulemap, p))
      POOL_DEBUG(type, "WEAK ");
  if (p >= solv->learntrules)
    POOL_DEBUG(type, "LEARNT ");
  else if (p >= solv->jobrules && p < solv->jobrules_end)
    POOL_DEBUG(type, "JOB ");
  else if (p >= solv->updaterules && p < solv->updaterules_end)
    POOL_DEBUG(type, "UPDATE ");
  else if (p >= solv->featurerules && p < solv->featurerules_end)
    POOL_DEBUG(type, "FEATURE ");
  solver_printrule(solv, type, r);
}

void
solver_printproblem(Solver *solv, Id v)
{
  Pool *pool = solv->pool;
  int i;
  Rule *r;
  Id *jp;

  if (v > 0)
    solver_printruleclass(solv, SAT_DEBUG_SOLUTIONS, solv->rules + v);
  else
    {
      v = -(v + 1);
      POOL_DEBUG(SAT_DEBUG_SOLUTIONS, "JOB %d\n", v);
      jp = solv->ruletojob.elements;
      for (i = solv->jobrules, r = solv->rules + i; i < solv->learntrules; i++, r++, jp++)
	if (*jp == v)
	  {
	    POOL_DEBUG(SAT_DEBUG_SOLUTIONS, "- ");
	    solver_printrule(solv, SAT_DEBUG_SOLUTIONS, r);
	  }
      POOL_DEBUG(SAT_DEBUG_SOLUTIONS, "ENDJOB\n");
    }
}

void
solver_printwatches(Solver *solv, int type)
{
  Pool *pool = solv->pool;
  int counter;

  POOL_DEBUG(type, "Watches: \n");
  for (counter = -(pool->nsolvables - 1); counter < pool->nsolvables; counter++)
    POOL_DEBUG(type, "    solvable [%d] -- rule [%d]\n", counter, solv->watches[counter + pool->nsolvables]);
}

/*
 * printdecisions
 */

void
solver_printdecisions(Solver *solv)
{
  Pool *pool = solv->pool;
  Repo *installed = solv->installed;
  Id p, *obsoletesmap = solver_create_decisions_obsoletesmap(solv);
  int i;
  Solvable *s;

  IF_POOLDEBUG (SAT_DEBUG_SCHUBI)
    {
      POOL_DEBUG(SAT_DEBUG_SCHUBI, "----- Decisions -----\n");
      for (i = 0; i < solv->decisionq.count; i++)
	{
	  p = solv->decisionq.elements[i];
	  solver_printruleelement(solv, SAT_DEBUG_SCHUBI, 0, p);
	}
      POOL_DEBUG(SAT_DEBUG_SCHUBI, "----- Decisions end -----\n");
    }

  POOL_DEBUG(SAT_DEBUG_RESULT, "\n");
  POOL_DEBUG(SAT_DEBUG_RESULT, "transaction:\n");

  /* print solvables to be erased */

  if (installed)
    {
      FOR_REPO_SOLVABLES(installed, p, s)
	{
	  if (solv->decisionmap[p] >= 0)
	    continue;
	  if (obsoletesmap[p])
	    continue;
	  POOL_DEBUG(SAT_DEBUG_RESULT, "  erase   %s\n", solvable2str(pool, s));
	}
    }

  /* print solvables to be installed */

  for (i = 0; i < solv->decisionq.count; i++)
    {
      int j;
      p = solv->decisionq.elements[i];
      if (p < 0)
	continue;
      if (p == SYSTEMSOLVABLE)
	continue;
      s = pool->solvables + p;
      if (installed && s->repo == installed)
	continue;

      if (!obsoletesmap[p])
        {
          POOL_DEBUG(SAT_DEBUG_RESULT, "  install   %s", solvable2str(pool, s));
        }
      else
	{
	  Id xp, *xpp;
	  FOR_PROVIDES(xp, xpp, s->name)
	    {
	      Solvable *s2 = pool->solvables + xp;
	      if (s2->name != s->name)
		continue;
	      if (evrcmp(pool, s->evr, s2->evr, EVRCMP_MATCH_RELEASE) < 0)
		break;
	    }
	  if (xp)
	    POOL_DEBUG(SAT_DEBUG_RESULT, "  downgrade %s", solvable2str(pool, s));
	  else
	    POOL_DEBUG(SAT_DEBUG_RESULT, "  upgrade   %s", solvable2str(pool, s));
          POOL_DEBUG(SAT_DEBUG_RESULT, "  (obsoletes");
	  for (j = installed->start; j < installed->end; j++)
	    if (obsoletesmap[j] == p)
	      POOL_DEBUG(SAT_DEBUG_RESULT, " %s", solvable2str(pool, pool->solvables + j));
	  POOL_DEBUG(SAT_DEBUG_RESULT, ")");
	}
      POOL_DEBUG(SAT_DEBUG_RESULT, "\n");
    }
  POOL_DEBUG(SAT_DEBUG_RESULT, "\n");

  sat_free(obsoletesmap);

  if (solv->recommendations.count)
    {
      POOL_DEBUG(SAT_DEBUG_RESULT, "recommended packages:\n");
      for (i = 0; i < solv->recommendations.count; i++)
	{
	  s = pool->solvables + solv->recommendations.elements[i];
          if (solv->decisionmap[solv->recommendations.elements[i]] > 0)
	    {
	      if (installed && s->repo == installed)
	        POOL_DEBUG(SAT_DEBUG_RESULT, "  %s (installed)\n", solvable2str(pool, s));
	      else
	        POOL_DEBUG(SAT_DEBUG_RESULT, "  %s (selected)\n", solvable2str(pool, s));
	    }
          else
	    POOL_DEBUG(SAT_DEBUG_RESULT, "  %s\n", solvable2str(pool, s));
	}
      POOL_DEBUG(SAT_DEBUG_RESULT, "\n");
    }

  if (solv->suggestions.count)
    {
      POOL_DEBUG(SAT_DEBUG_RESULT, "suggested packages:\n");
      for (i = 0; i < solv->suggestions.count; i++)
	{
	  s = pool->solvables + solv->suggestions.elements[i];
          if (solv->decisionmap[solv->suggestions.elements[i]] > 0)
	    {
	      if (installed && s->repo == installed)
	        POOL_DEBUG(SAT_DEBUG_RESULT, "  %s (installed)\n", solvable2str(pool, s));
	      else
	        POOL_DEBUG(SAT_DEBUG_RESULT, "  %s (selected)\n", solvable2str(pool, s));
	    }
	  else
	    POOL_DEBUG(SAT_DEBUG_RESULT, "  %s\n", solvable2str(pool, s));
	}
      POOL_DEBUG(SAT_DEBUG_RESULT, "\n");
    }
}

void
solver_printprobleminfo(Solver *solv, Queue *job, Id problem)
{
  Pool *pool = solv->pool;
  Id probr;
  Id dep, source, target;
  Solvable *s, *s2;

  probr = solver_findproblemrule(solv, problem);
  switch (solver_problemruleinfo(solv, job, probr, &dep, &source, &target))
    {
    case SOLVER_PROBLEM_UPDATE_RULE:
      s = pool_id2solvable(pool, source);
      POOL_DEBUG(SAT_DEBUG_RESULT, "problem with installed package %s\n", solvable2str(pool, s));
      return;
    case SOLVER_PROBLEM_JOB_RULE:
      POOL_DEBUG(SAT_DEBUG_RESULT, "conflicting requests\n");
      return;
    case SOLVER_PROBLEM_RPM_RULE:
      POOL_DEBUG(SAT_DEBUG_RESULT, "some dependency problem\n");
      return;
    case SOLVER_PROBLEM_JOB_NOTHING_PROVIDES_DEP:
      POOL_DEBUG(SAT_DEBUG_RESULT, "nothing provides requested %s\n", dep2str(pool, dep));
      return;
    case SOLVER_PROBLEM_NOT_INSTALLABLE:
      s = pool_id2solvable(pool, source);
      POOL_DEBUG(SAT_DEBUG_RESULT, "package %s is not installable\n", solvable2str(pool, s));
      return;
    case SOLVER_PROBLEM_NOTHING_PROVIDES_DEP:
      s = pool_id2solvable(pool, source);
      POOL_DEBUG(SAT_DEBUG_RESULT, "nothing provides %s needed by %s\n", dep2str(pool, dep), solvable2str(pool, s));
      return;
    case SOLVER_PROBLEM_SAME_NAME:
      s = pool_id2solvable(pool, source);
      s2 = pool_id2solvable(pool, target);
      POOL_DEBUG(SAT_DEBUG_RESULT, "cannot install both %s and %s\n", solvable2str(pool, s), solvable2str(pool, s2));
      return;
    case SOLVER_PROBLEM_PACKAGE_CONFLICT:
      s = pool_id2solvable(pool, source);
      s2 = pool_id2solvable(pool, target);
      POOL_DEBUG(SAT_DEBUG_RESULT, "package %s conflicts with %s provided by %s\n", solvable2str(pool, s), dep2str(pool, dep), solvable2str(pool, s2));
      return;
    case SOLVER_PROBLEM_PACKAGE_OBSOLETES:
      s = pool_id2solvable(pool, source);
      s2 = pool_id2solvable(pool, target);
      POOL_DEBUG(SAT_DEBUG_RESULT, "package %s obsoletes %s provided by %s\n", solvable2str(pool, s), dep2str(pool, dep), solvable2str(pool, s2));
      return;
    case SOLVER_PROBLEM_DEP_PROVIDERS_NOT_INSTALLABLE:
      s = pool_id2solvable(pool, source);
      POOL_DEBUG(SAT_DEBUG_RESULT, "package %s requires %s, but none of the providers can be installed\n", solvable2str(pool, s), dep2str(pool, dep));
      return;
    case SOLVER_PROBLEM_SELF_CONFLICT:
      s = pool_id2solvable(pool, source);
      POOL_DEBUG(SAT_DEBUG_RESULT, "package %s conflicts with %s provided by itself\n", solvable2str(pool, s), dep2str(pool, dep));
      return;
    }
}

void
solver_printsolutions(Solver *solv, Queue *job)
{
  Pool *pool = solv->pool;
  int pcnt;
  Id p, rp, how, what;
  Id problem, solution, element;
  Solvable *s, *sd;

  POOL_DEBUG(SAT_DEBUG_RESULT, "Encountered problems! Here are the solutions:\n\n");
  pcnt = 1;
  problem = 0;
  while ((problem = solver_next_problem(solv, problem)) != 0)
    {
      POOL_DEBUG(SAT_DEBUG_RESULT, "Problem %d:\n", pcnt++);
      POOL_DEBUG(SAT_DEBUG_RESULT, "====================================\n");
      solver_printprobleminfo(solv, job, problem);
      POOL_DEBUG(SAT_DEBUG_RESULT, "\n");
      solution = 0;
      while ((solution = solver_next_solution(solv, problem, solution)) != 0)
        {
	  element = 0;
	  while ((element = solver_next_solutionelement(solv, problem, solution, element, &p, &rp)) != 0)
	    {
	      if (p == 0)
		{
		  /* job, rp is index into job queue */
		  how = job->elements[rp - 1] & ~SOLVER_WEAK;
		  what = job->elements[rp];
		  switch (how)
		    {
		    case SOLVER_INSTALL_SOLVABLE:
		      s = pool->solvables + what;
		      if (solv->installed && s->repo == solv->installed)
			POOL_DEBUG(SAT_DEBUG_RESULT, "- do not keep %s installed\n", solvable2str(pool, s));
		      else
			POOL_DEBUG(SAT_DEBUG_RESULT, "- do not install %s\n", solvable2str(pool, s));
		      break;
		    case SOLVER_ERASE_SOLVABLE:
		      s = pool->solvables + what;
		      if (solv->installed && s->repo == solv->installed)
			POOL_DEBUG(SAT_DEBUG_RESULT, "- do not deinstall %s\n", solvable2str(pool, s));
		      else
			POOL_DEBUG(SAT_DEBUG_RESULT, "- do not forbid installation of %s\n", solvable2str(pool, s));
		      break;
		    case SOLVER_INSTALL_SOLVABLE_NAME:
		      POOL_DEBUG(SAT_DEBUG_RESULT, "- do not install %s\n", dep2str(pool, what));
		      break;
		    case SOLVER_ERASE_SOLVABLE_NAME:
		      POOL_DEBUG(SAT_DEBUG_RESULT, "- do not deinstall %s\n", dep2str(pool, what));
		      break;
		    case SOLVER_INSTALL_SOLVABLE_PROVIDES:
		      POOL_DEBUG(SAT_DEBUG_RESULT, "- do not install a solvable providing %s\n", dep2str(pool, what));
		      break;
		    case SOLVER_ERASE_SOLVABLE_PROVIDES:
		      POOL_DEBUG(SAT_DEBUG_RESULT, "- do not deinstall all solvables providing %s\n", dep2str(pool, what));
		      break;
		    case SOLVER_INSTALL_SOLVABLE_UPDATE:
		      s = pool->solvables + what;
		      POOL_DEBUG(SAT_DEBUG_RESULT, "- do not install most recent version of %s\n", solvable2str(pool, s));
		      break;
		    default:
		      POOL_DEBUG(SAT_DEBUG_RESULT, "- do something different\n");
		      break;
		    }
		}
	      else
		{
		  /* policy, replace p with rp */
		  s = pool->solvables + p;
		  sd = rp ? pool->solvables + rp : 0;
		  if (sd)
		    {
		      int gotone = 0;
		      if (!solv->allowdowngrade && evrcmp(pool, s->evr, sd->evr, EVRCMP_MATCH_RELEASE) > 0)
			{
			  POOL_DEBUG(SAT_DEBUG_RESULT, "- allow downgrade of %s to %s\n", solvable2str(pool, s), solvable2str(pool, sd));
			  gotone = 1;
			}
		      if (!solv->allowarchchange && s->name == sd->name && s->arch != sd->arch && policy_illegal_archchange(solv, s, sd))
			{
			  POOL_DEBUG(SAT_DEBUG_RESULT, "- allow architecture change of %s to %s\n", solvable2str(pool, s), solvable2str(pool, sd));
			  gotone = 1;
			}
		      if (!solv->allowvendorchange && s->name == sd->name && s->vendor != sd->vendor && policy_illegal_vendorchange(solv, s, sd))
			{
			  if (sd->vendor)
			    POOL_DEBUG(SAT_DEBUG_RESULT, "- allow vendor change from '%s' (%s) to '%s' (%s)\n", id2str(pool, s->vendor), solvable2str(pool, s), id2str(pool, sd->vendor), solvable2str(pool, sd));
			  else
			    POOL_DEBUG(SAT_DEBUG_RESULT, "- allow vendor change from '%s' (%s) to no vendor (%s)\n", id2str(pool, s->vendor), solvable2str(pool, s), solvable2str(pool, sd));
			  gotone = 1;
			}
		      if (!gotone)
			POOL_DEBUG(SAT_DEBUG_RESULT, "- allow replacement of %s with %s\n", solvable2str(pool, s), solvable2str(pool, sd));
		    }
		  else
		    {
		      POOL_DEBUG(SAT_DEBUG_RESULT, "- allow deinstallation of %s\n", solvable2str(pool, s));
		    }

		}
	    }
	  POOL_DEBUG(SAT_DEBUG_RESULT, "\n");
        }
    }
}

void
solver_printtrivial(Solver *solv)
{
  Pool *pool = solv->pool;
  Queue in, out;
  Map installedmap;
  Id p;
  const char *n; 
  Solvable *s; 
  int i;

  queue_init(&in);
  for (p = 1, s = pool->solvables + p; p < solv->pool->nsolvables; p++, s++)
    {   
      n = id2str(pool, s->name);
      if (strncmp(n, "patch:", 6) != 0 && strncmp(n, "pattern:", 8) != 0)
        continue;
      queue_push(&in, p); 
    }   
  if (!in.count)
    {
      queue_free(&in);
      return;
    }
  solver_create_state_maps(solv, &installedmap, 0); 
  queue_init(&out);
  pool_trivial_installable(pool, solv->installed, &installedmap, &in, &out);
  POOL_DEBUG(SAT_DEBUG_RESULT, "trivial installable status:\n");
  for (i = 0; i < in.count; i++)
    POOL_DEBUG(SAT_DEBUG_RESULT, "  %s: %d\n", solvable2str(pool, pool->solvables + in.elements[i]), out.elements[i]);
  POOL_DEBUG(SAT_DEBUG_RESULT, "\n");
  map_free(&installedmap);
  queue_free(&in);
  queue_free(&out);
}
