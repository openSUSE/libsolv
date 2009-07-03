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
#include "solverdebug.h"
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

/* OBSOLETE: use transaction code instead! */

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
	  Id pp, n;
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
	      if (!pool->implicitobsoleteusesprovides && s->name != ps->name)
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
	  Id pp, n;

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
		  if (!pool->obsoleteusesprovides && !pool_match_nevr(pool, pool->solvables + p, obs))
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
  else if (p >= solv->infarchrules && p < solv->infarchrules_end)
    POOL_DEBUG(type, "INFARCH ");
  else if (p >= solv->duprules && p < solv->duprules_end)
    POOL_DEBUG(type, "DUP ");
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
      for (i = solv->jobrules, r = solv->rules + i; i < solv->jobrules_end; i++, r++, jp++)
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
  Id p, type;
  int i, j;
  Solvable *s;
  Queue iq;

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

  queue_init(&iq);
  for (i = 0; i < solv->trans.steps.count; i++)
    {
      p = solv->trans.steps.elements[i];
      s = pool->solvables + p;
      type = transaction_type(&solv->trans, p, SOLVER_TRANSACTION_SHOW_ACTIVE|SOLVER_TRANSACTION_SHOW_ALL|SOLVER_TRANSACTION_SHOW_OBSOLETES|SOLVER_TRANSACTION_SHOW_MULTIINSTALL);
      switch(type)
        {
	case SOLVER_TRANSACTION_MULTIINSTALL:
          POOL_DEBUG(SAT_DEBUG_RESULT, "  multi install %s", solvable2str(pool, s));
	  break;
	case SOLVER_TRANSACTION_MULTIREINSTALL:
          POOL_DEBUG(SAT_DEBUG_RESULT, "  multi reinstall %s", solvable2str(pool, s));
	  break;
	case SOLVER_TRANSACTION_INSTALL:
          POOL_DEBUG(SAT_DEBUG_RESULT, "  install   %s", solvable2str(pool, s));
	  break;
	case SOLVER_TRANSACTION_REINSTALL:
          POOL_DEBUG(SAT_DEBUG_RESULT, "  reinstall %s", solvable2str(pool, s));
	  break;
	case SOLVER_TRANSACTION_DOWNGRADE:
          POOL_DEBUG(SAT_DEBUG_RESULT, "  downgrade %s", solvable2str(pool, s));
	  break;
	case SOLVER_TRANSACTION_CHANGE:
          POOL_DEBUG(SAT_DEBUG_RESULT, "  change    %s", solvable2str(pool, s));
	  break;
	case SOLVER_TRANSACTION_UPGRADE:
	case SOLVER_TRANSACTION_OBSOLETES:
          POOL_DEBUG(SAT_DEBUG_RESULT, "  upgrade   %s", solvable2str(pool, s));
	  break;
	case SOLVER_TRANSACTION_ERASE:
          POOL_DEBUG(SAT_DEBUG_RESULT, "  erase     %s", solvable2str(pool, s));
	  break;
	default:
	  break;
        }
      switch(type)
        {
	case SOLVER_TRANSACTION_INSTALL:
	case SOLVER_TRANSACTION_ERASE:
	  POOL_DEBUG(SAT_DEBUG_RESULT, "\n");
	  break;
	case SOLVER_TRANSACTION_REINSTALL:
	case SOLVER_TRANSACTION_DOWNGRADE:
	case SOLVER_TRANSACTION_CHANGE:
	case SOLVER_TRANSACTION_UPGRADE:
	case SOLVER_TRANSACTION_OBSOLETES:
	  transaction_all_obs_pkgs(&solv->trans, p, &iq);
	  if (iq.count)
	    {
	      POOL_DEBUG(SAT_DEBUG_RESULT, "  (obsoletes");
	      for (j = 0; j < iq.count; j++)
		POOL_DEBUG(SAT_DEBUG_RESULT, " %s", solvid2str(pool, iq.elements[j]));
	      POOL_DEBUG(SAT_DEBUG_RESULT, ")");
	    }
	  POOL_DEBUG(SAT_DEBUG_RESULT, "\n");
	  break;
	default:
	  break;
	}
    }
  queue_free(&iq);

  POOL_DEBUG(SAT_DEBUG_RESULT, "\n");

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
  if (solv->orphaned.count)
    {
      POOL_DEBUG(SAT_DEBUG_RESULT, "orphaned packages:\n");
      for (i = 0; i < solv->orphaned.count; i++)
	{
	  s = pool->solvables + solv->orphaned.elements[i];
          if (solv->decisionmap[solv->orphaned.elements[i]] > 0)
	    POOL_DEBUG(SAT_DEBUG_RESULT, "  %s (kept)\n", solvable2str(pool, s));
	  else
	    POOL_DEBUG(SAT_DEBUG_RESULT, "  %s (erased)\n", solvable2str(pool, s));
	}
      POOL_DEBUG(SAT_DEBUG_RESULT, "\n");
    }
}

static inline
const char *id2strnone(Pool *pool, Id id)
{
  return !id || id == 1 ? "(none)" : id2str(pool, id);
}

void
solver_printtransaction(Solver *solv)
{
  Transaction *trans = &solv->trans;
  Pool *pool = trans->pool;
  Queue classes, pkgs;
  int i, j, mode, l, linel;
  char line[76];
  const char *n;

  queue_init(&classes);
  queue_init(&pkgs);
  mode = 0;
  transaction_classify(trans, mode, &classes);
  for (i = 0; i < classes.count; i += 4)
    {
      Id class = classes.elements[i];
      Id cnt = classes.elements[i + 1];
      switch(class)
	{
	case SOLVER_TRANSACTION_ERASE:
	  POOL_DEBUG(SAT_DEBUG_RESULT, "erased packages (%d):\n", cnt);
	  break;
	case SOLVER_TRANSACTION_INSTALL:
	  POOL_DEBUG(SAT_DEBUG_RESULT, "installed packages (%d):\n", cnt);
	  break;
	case SOLVER_TRANSACTION_REINSTALLED:
	  POOL_DEBUG(SAT_DEBUG_RESULT, "reinstalled packages (%d):\n", cnt);
	  break;
	case SOLVER_TRANSACTION_DOWNGRADED:
	  POOL_DEBUG(SAT_DEBUG_RESULT, "downgraded packages (%d):\n", cnt);
	  break;
	case SOLVER_TRANSACTION_CHANGED:
	  POOL_DEBUG(SAT_DEBUG_RESULT, "changed packages (%d):\n", cnt);
	  break;
	case SOLVER_TRANSACTION_UPGRADED:
	  POOL_DEBUG(SAT_DEBUG_RESULT, "upgraded packages (%d):\n", cnt);
	  break;
	case SOLVER_TRANSACTION_VENDORCHANGE:
	  POOL_DEBUG(SAT_DEBUG_RESULT, "vendor change from '%s' to '%s' (%d):\n", id2strnone(pool, classes.elements[i + 2]), id2strnone(pool, classes.elements[i + 3]), cnt);
	  break;
	case SOLVER_TRANSACTION_ARCHCHANGE:
	  POOL_DEBUG(SAT_DEBUG_RESULT, "arch change from %s to %s (%d):\n", id2str(pool, classes.elements[i + 2]), id2str(pool, classes.elements[i + 3]), cnt);
	  break;
	default:
	  class = SOLVER_TRANSACTION_IGNORE;
	  break;
	}
      if (class == SOLVER_TRANSACTION_IGNORE)
	continue;
      transaction_classify_pkgs(trans, mode, class, classes.elements[i + 2], classes.elements[i + 3], &pkgs);
      *line = 0;
      linel = 0;
      for (j = 0; j < pkgs.count; j++)
	{
	  Id p = pkgs.elements[j];
	  Solvable *s = pool->solvables + p;
	  Solvable *s2;

	  switch(class)
	    {
	    case SOLVER_TRANSACTION_DOWNGRADED:
	    case SOLVER_TRANSACTION_UPGRADED:
	      s2 = pool->solvables + transaction_obs_pkg(trans, p);
	      POOL_DEBUG(SAT_DEBUG_RESULT, "  - %s -> %s\n", solvable2str(pool, s), solvable2str(pool, s2));
	      break;
	    case SOLVER_TRANSACTION_VENDORCHANGE:
	    case SOLVER_TRANSACTION_ARCHCHANGE:
	      n = id2str(pool, s->name);
	      l = strlen(n);
	      if (l + linel > sizeof(line) - 3)
		{
		  if (*line)
		    POOL_DEBUG(SAT_DEBUG_RESULT, "    %s\n", line);
		  *line = 0;
		  linel = 0;
		}
	      if (l + linel > sizeof(line) - 3)
	        POOL_DEBUG(SAT_DEBUG_RESULT, "    %s\n", n);
	      else
		{
		  if (*line)
		    {
		      strcpy(line + linel, ", ");
		      linel += 2;
		    }
		  strcpy(line + linel, n);
		  linel += l;
		}
	      break;
	    default:
	      POOL_DEBUG(SAT_DEBUG_RESULT, "  - %s\n", solvable2str(pool, s));
	      break;
	    }
	}
      if (*line)
	POOL_DEBUG(SAT_DEBUG_RESULT, "    %s\n", line);
      POOL_DEBUG(SAT_DEBUG_RESULT, "\n");
    }
  queue_free(&classes);
  queue_free(&pkgs);
}

void
solver_printprobleminfo(Solver *solv, Id problem)
{
  Pool *pool = solv->pool;
  Id probr;
  Id dep, source, target;

  probr = solver_findproblemrule(solv, problem);
  switch (solver_ruleinfo(solv, probr, &source, &target, &dep))
    {
    case SOLVER_RULE_DISTUPGRADE:
      POOL_DEBUG(SAT_DEBUG_RESULT, "%s does not belong to a distupgrade repository\n", solvid2str(pool, source));
      return;
    case SOLVER_RULE_INFARCH:
      POOL_DEBUG(SAT_DEBUG_RESULT, "%s has inferior architecture\n", solvid2str(pool, source));
      return;
    case SOLVER_RULE_UPDATE:
      POOL_DEBUG(SAT_DEBUG_RESULT, "problem with installed package %s\n", solvid2str(pool, source));
      return;
    case SOLVER_RULE_JOB:
      POOL_DEBUG(SAT_DEBUG_RESULT, "conflicting requests\n");
      return;
    case SOLVER_RULE_JOB_NOTHING_PROVIDES_DEP:
      POOL_DEBUG(SAT_DEBUG_RESULT, "nothing provides requested %s\n", dep2str(pool, dep));
      return;
    case SOLVER_RULE_RPM:
      POOL_DEBUG(SAT_DEBUG_RESULT, "some dependency problem\n");
      return;
    case SOLVER_RULE_RPM_NOT_INSTALLABLE:
      POOL_DEBUG(SAT_DEBUG_RESULT, "package %s is not installable\n", solvid2str(pool, source));
      return;
    case SOLVER_RULE_RPM_NOTHING_PROVIDES_DEP:
      POOL_DEBUG(SAT_DEBUG_RESULT, "nothing provides %s needed by %s\n", dep2str(pool, dep), solvid2str(pool, source));
      return;
    case SOLVER_RULE_RPM_SAME_NAME:
      POOL_DEBUG(SAT_DEBUG_RESULT, "cannot install both %s and %s\n", solvid2str(pool, source), solvid2str(pool, target));
      return;
    case SOLVER_RULE_RPM_PACKAGE_CONFLICT:
      POOL_DEBUG(SAT_DEBUG_RESULT, "package %s conflicts with %s provided by %s\n", solvid2str(pool, source), dep2str(pool, dep), solvid2str(pool, target));
      return;
    case SOLVER_RULE_RPM_PACKAGE_OBSOLETES:
      POOL_DEBUG(SAT_DEBUG_RESULT, "package %s obsoletes %s provided by %s\n", solvid2str(pool, source), dep2str(pool, dep), solvid2str(pool, target));
      return;
    case SOLVER_RULE_RPM_IMPLICIT_OBSOLETES:
      POOL_DEBUG(SAT_DEBUG_RESULT, "package %s implicitely obsoletes %s provided by %s\n", solvid2str(pool, source), dep2str(pool, dep), solvid2str(pool, target));
      return;
    case SOLVER_RULE_RPM_PACKAGE_REQUIRES:
      POOL_DEBUG(SAT_DEBUG_RESULT, "package %s requires %s, but none of the providers can be installed\n", solvid2str(pool, source), dep2str(pool, dep));
      return;
    case SOLVER_RULE_RPM_SELF_CONFLICT:
      POOL_DEBUG(SAT_DEBUG_RESULT, "package %s conflicts with %s provided by itself\n", solvid2str(pool, source), dep2str(pool, dep));
      return;
    case SOLVER_RULE_UNKNOWN:
    case SOLVER_RULE_FEATURE:
    case SOLVER_RULE_LEARNT:
      POOL_DEBUG(SAT_DEBUG_RESULT, "bad rule type\n");
      return;
    }
}

void
solver_printsolution(Solver *solv, Id problem, Id solution)
{
  Pool *pool = solv->pool;
  Id p, rp, element, how, what, select;
  Solvable *s, *sd;

  element = 0;
  while ((element = solver_next_solutionelement(solv, problem, solution, element, &p, &rp)) != 0)
    {
      if (p == SOLVER_SOLUTION_JOB)
	{
	  /* job, rp is index into job queue */
	  how = solv->job.elements[rp - 1];
	  what = solv->job.elements[rp];
	  select = how & SOLVER_SELECTMASK;
	  switch (how & SOLVER_JOBMASK)
	    {
	    case SOLVER_INSTALL:
	      if (select == SOLVER_SOLVABLE && solv->installed && pool->solvables[what].repo == solv->installed)
		POOL_DEBUG(SAT_DEBUG_RESULT, "  - do not keep %s installed\n", solvid2str(pool, what));
	      else if (select == SOLVER_SOLVABLE_PROVIDES)
		POOL_DEBUG(SAT_DEBUG_RESULT, "  - do not install a solvable %s\n", solver_select2str(solv, select, what));
	      else
		POOL_DEBUG(SAT_DEBUG_RESULT, "  - do not install %s\n", solver_select2str(solv, select, what));
	      break;
	    case SOLVER_ERASE:
	      if (select == SOLVER_SOLVABLE && !(solv->installed && pool->solvables[what].repo == solv->installed))
		POOL_DEBUG(SAT_DEBUG_RESULT, "  - do not forbid installation of %s\n", solvid2str(pool, what));
	      else if (select == SOLVER_SOLVABLE_PROVIDES)
		POOL_DEBUG(SAT_DEBUG_RESULT, "  - do not deinstall all solvables %s\n", solver_select2str(solv, select, what));
	      else
		POOL_DEBUG(SAT_DEBUG_RESULT, "  - do not deinstall %s\n", solver_select2str(solv, select, what));
	      break;
	    case SOLVER_UPDATE:
	      POOL_DEBUG(SAT_DEBUG_RESULT, "  - do not install most recent version of %s\n", solver_select2str(solv, select, what));
	      break;
	    case SOLVER_LOCK:
	      POOL_DEBUG(SAT_DEBUG_RESULT, "  - do not lock %s\n", solver_select2str(solv, select, what));
	      break;
	    default:
	      POOL_DEBUG(SAT_DEBUG_RESULT, "  - do something different\n");
	      break;
	    }
	}
      else if (p == SOLVER_SOLUTION_INFARCH)
	{
	  s = pool->solvables + rp;
	  if (solv->installed && s->repo == solv->installed)
	    POOL_DEBUG(SAT_DEBUG_RESULT, "  - keep %s despite the inferior architecture\n", solvable2str(pool, s));
	  else
	    POOL_DEBUG(SAT_DEBUG_RESULT, "  - install %s despite the inferior architecture\n", solvable2str(pool, s));
	}
      else if (p == SOLVER_SOLUTION_DISTUPGRADE)
	{
	  s = pool->solvables + rp;
	  if (solv->installed && s->repo == solv->installed)
	    POOL_DEBUG(SAT_DEBUG_RESULT, "  - keep obsolete %s\n", solvable2str(pool, s));
	  else
	    POOL_DEBUG(SAT_DEBUG_RESULT, "  - install %s from excluded repository\n", solvable2str(pool, s));
	}
      else
	{
	  /* policy, replace p with rp */
	  s = pool->solvables + p;
	  sd = rp ? pool->solvables + rp : 0;
	  if (s == sd && solv->distupgrade)
	    {
	      POOL_DEBUG(SAT_DEBUG_RESULT, "  - keep obsolete %s\n", solvable2str(pool, s));
	    }
	  else if (sd)
	    {
	      int gotone = 0;
	      if (!solv->allowdowngrade && evrcmp(pool, s->evr, sd->evr, EVRCMP_MATCH_RELEASE) > 0)
		{
		  POOL_DEBUG(SAT_DEBUG_RESULT, "  - allow downgrade of %s to %s\n", solvable2str(pool, s), solvable2str(pool, sd));
		  gotone = 1;
		}
	      if (!solv->allowarchchange && s->name == sd->name && s->arch != sd->arch && policy_illegal_archchange(solv, s, sd))
		{
		  POOL_DEBUG(SAT_DEBUG_RESULT, "  - allow architecture change of %s to %s\n", solvable2str(pool, s), solvable2str(pool, sd));
		  gotone = 1;
		}
	      if (!solv->allowvendorchange && s->name == sd->name && s->vendor != sd->vendor && policy_illegal_vendorchange(solv, s, sd))
		{
		  if (sd->vendor)
		    POOL_DEBUG(SAT_DEBUG_RESULT, "  - allow vendor change from '%s' (%s) to '%s' (%s)\n", id2str(pool, s->vendor), solvable2str(pool, s), id2str(pool, sd->vendor), solvable2str(pool, sd));
		  else
		    POOL_DEBUG(SAT_DEBUG_RESULT, "  - allow vendor change from '%s' (%s) to no vendor (%s)\n", id2str(pool, s->vendor), solvable2str(pool, s), solvable2str(pool, sd));
		  gotone = 1;
		}
	      if (!gotone)
		POOL_DEBUG(SAT_DEBUG_RESULT, "  - allow replacement of %s with %s\n", solvable2str(pool, s), solvable2str(pool, sd));
	    }
	  else
	    {
	      POOL_DEBUG(SAT_DEBUG_RESULT, "  - allow deinstallation of %s\n", solvable2str(pool, s));
	    }

	}
    }
}

void
solver_printallsolutions(Solver *solv)
{
  Pool *pool = solv->pool;
  int pcnt;
  Id problem, solution;

  POOL_DEBUG(SAT_DEBUG_RESULT, "Encountered problems! Here are the solutions:\n\n");
  pcnt = 0;
  problem = 0;
  while ((problem = solver_next_problem(solv, problem)) != 0)
    {
      pcnt++;
      POOL_DEBUG(SAT_DEBUG_RESULT, "Problem %d:\n", pcnt);
      POOL_DEBUG(SAT_DEBUG_RESULT, "====================================\n");
      solver_printprobleminfo(solv, problem);
      POOL_DEBUG(SAT_DEBUG_RESULT, "\n");
      solution = 0;
      while ((solution = solver_next_solution(solv, problem, solution)) != 0)
        {
	  solver_printsolution(solv, problem, solution);
          POOL_DEBUG(SAT_DEBUG_RESULT, "\n");
        }
    }
}

void
solver_printtrivial(Solver *solv)
{
  Pool *pool = solv->pool;
  Queue in, out;
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
  queue_init(&out);
  solver_trivial_installable(solv, &in, &out);
  POOL_DEBUG(SAT_DEBUG_RESULT, "trivial installable status:\n");
  for (i = 0; i < in.count; i++)
    POOL_DEBUG(SAT_DEBUG_RESULT, "  %s: %d\n", solvid2str(pool, in.elements[i]), out.elements[i]);
  POOL_DEBUG(SAT_DEBUG_RESULT, "\n");
  queue_free(&in);
  queue_free(&out);
}

const char *
solver_select2str(Solver *solv, Id select, Id what)
{
  Pool *pool = solv->pool;
  const char *s;
  char *b;
  if (select == SOLVER_SOLVABLE)
    return solvid2str(pool, what);
  if (select == SOLVER_SOLVABLE_NAME)
    return dep2str(pool, what);
  if (select == SOLVER_SOLVABLE_PROVIDES)
    {
      s = dep2str(pool, what);
      b = pool_alloctmpspace(pool, 11 + strlen(s));
      sprintf(b, "providing %s", s);
      return b;
    }
  if (select == SOLVER_SOLVABLE_ONE_OF)
    {
      Id p;
      char *b2;
      b = "";
      while ((p = pool->whatprovidesdata[what++]) != 0)
	{
	  s = solvid2str(pool, p);
	  b2 = pool_alloctmpspace(pool, strlen(b) + strlen(s) + 3);
	  sprintf(b2, "%s, %s", b, s);
	  b = b2;
	}
      return *b ? b + 2 : "nothing";
    }
  return "unknown job select";
}
