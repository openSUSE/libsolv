/*
 * Copyright (c) 2007, Novell Inc.
 *
 * This program is licensed under the BSD license, read LICENSE.BSD
 * for further information
 */

/*
 * solver.c
 *
 * SAT based dependency solver
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

#define RULES_BLOCK 63
#define REGARD_RECOMMENDS_OF_INSTALLED_ITEMS 0


int
solver_dep_installed(Solver *solv, Id dep)
{
  /* disable for now, splitprovides don't work anyway and it breaks
     a testcase */
#if 0
  Pool *pool = solv->pool;
  Id p, *pp;

  if (ISRELDEP(dep))
    {
      Reldep *rd = GETRELDEP(pool, dep);
      if (rd->flags == REL_AND)
	{
	  if (!solver_dep_installed(solv, rd->name))
	    return 0;
	  return solver_dep_installed(solv, rd->evr);
	}
      if (rd->flags == REL_NAMESPACE && rd->name == NAMESPACE_INSTALLED)
	return solver_dep_installed(solv, rd->evr);
    }
  FOR_PROVIDES(p, pp, dep)
    {
      if (p == SYSTEMSOLVABLE || (solv->installed && pool->solvables[p].repo == solv->installed))
	return 1;
    }
#endif
  return 0;
}


/* this mirrors solver_dep_fulfilled but uses map m instead of the decisionmap */
static inline int
dep_possible(Solver *solv, Id dep, Map *m)
{
  Pool *pool = solv->pool;
  Id p, *pp;

  if (ISRELDEP(dep))
    {
      Reldep *rd = GETRELDEP(pool, dep);
      if (rd->flags == REL_AND)
	{
	  if (!dep_possible(solv, rd->name, m))
	    return 0;
	  return dep_possible(solv, rd->evr, m);
	}
      if (rd->flags == REL_NAMESPACE && rd->name == NAMESPACE_INSTALLED)
	return solver_dep_installed(solv, rd->evr);
    }
  FOR_PROVIDES(p, pp, dep)
    {
      if (MAPTST(m, p))
	return 1;
    }
  return 0;
}

/*-----------------------------------------------------------------*/

/*
 * print rules
 */

static void
printruleelement(Solver *solv, int type, Rule *r, Id v)
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

static void
printrule(Solver *solv, int type, Rule *r)
{
  Pool *pool = solv->pool;
  int i;
  Id v;

  if (r >= solv->rules && r < solv->rules + solv->nrules)   /* r is a solver rule */
    POOL_DEBUG(type, "Rule #%d:", (int)(r - solv->rules));
  else
    POOL_DEBUG(type, "Rule:");		       /* r is any rule */
  if (r && r->w1 == 0)
    POOL_DEBUG(type, " (disabled)");
  POOL_DEBUG(type, "\n");
  for (i = 0; ; i++)
    {
      if (i == 0)
	  /* print direct literal */ 
	v = r->p;
      else if (r->d == ID_NULL)
	{
	  if (i == 2)
	    break;
	  /* binary rule --> print w2 as second literal */
	  v = r->w2;
	}
      else
	  /* every other which is in d */
	v = solv->pool->whatprovidesdata[r->d + i - 1];
      if (v == ID_NULL)
	break;
      printruleelement(solv, type, r, v);
    }
  POOL_DEBUG(type, "    next rules: %d %d\n", r->n1, r->n2);
}

static void
printruleclass(Solver *solv, int type, Rule *r)
{
  Pool *pool = solv->pool;
  if (r - solv->rules >= solv->learntrules)
    POOL_DEBUG(type, "LEARNT ");
  else if (r - solv->rules >= solv->weakrules)
    POOL_DEBUG(type, "WEAK ");
  else if (r - solv->rules >= solv->systemrules)
    POOL_DEBUG(type, "SYSTEM ");
  else if (r - solv->rules >= solv->jobrules)
    POOL_DEBUG(type, "JOB ");
  printrule(solv, type, r);
}


/*-----------------------------------------------------------------*/

/*
 * Rule handling
 */

static Pool *unifyrules_sortcmp_data;

/*
 * compare rules for unification sort
 */

static int
unifyrules_sortcmp(const void *ap, const void *bp)
{
  Pool *pool = unifyrules_sortcmp_data;
  Rule *a = (Rule *)ap;
  Rule *b = (Rule *)bp;
  Id *ad, *bd;
  int x;
  
  x = a->p - b->p;
  if (x)
    return x;			       /* p differs */

  /* identical p */
  if (a->d == 0 && b->d == 0)
    return a->w2 - b->w2;	       /* assertion: return w2 diff */

  if (a->d == 0)		       /* a is assertion, b not */
    {
      x = a->w2 - pool->whatprovidesdata[b->d];
      return x ? x : -1;
    }

  if (b->d == 0)		       /* b is assertion, a not */
    {
      x = pool->whatprovidesdata[a->d] - b->w2;
      return x ? x : 1;
    }

  /* compare whatprovidesdata */
  ad = pool->whatprovidesdata + a->d;
  bd = pool->whatprovidesdata + b->d;
  while (*bd)
    if ((x = *ad++ - *bd++) != 0)
      return x;
  return *ad;
}


/*
 * unify rules
 */

static void
unifyrules(Solver *solv)
{
  Pool *pool = solv->pool;
  int i, j;
  Rule *ir, *jr;

  if (solv->nrules <= 1)	       /* nothing to unify */
    return;

  POOL_DEBUG(SAT_DEBUG_SCHUBI, "----- unifyrules -----\n");

  /* sort rules first */
  unifyrules_sortcmp_data = solv->pool;
  qsort(solv->rules + 1, solv->nrules - 1, sizeof(Rule), unifyrules_sortcmp);

  /* prune rules
   * i = unpruned
   * j = pruned
   */
  jr = 0;
  for (i = j = 1, ir = solv->rules + 1; i < solv->nrules; i++, ir++)
    {
      if (jr && !unifyrules_sortcmp(ir, jr))
	continue;		       /* prune! */
      jr = solv->rules + j++;	       /* keep! */
      if (ir != jr)
        *jr = *ir;
    }

  /* reduced count from nrules to j rules */
  POOL_DEBUG(SAT_DEBUG_STATS, "pruned rules from %d to %d\n", solv->nrules, j);

  /* adapt rule buffer */
  solv->nrules = j;
  solv->rules = (Rule *)sat_realloc(solv->rules, ((solv->nrules + RULES_BLOCK) & ~RULES_BLOCK) * sizeof(Rule));
  IF_POOLDEBUG (SAT_DEBUG_STATS)
    {
      int binr = 0;
      int lits = 0;
      Id *dp;
      Rule *r;

      for (i = 1; i < solv->nrules; i++)
	{
	  r = solv->rules + i;
	  if (r->d == 0)
	    binr++;
	  else
	    {
	      dp = solv->pool->whatprovidesdata + r->d;
	      while (*dp++)
		lits++;
	    }
	}
      POOL_DEBUG(SAT_DEBUG_STATS, "  binary: %d\n", binr);
      POOL_DEBUG(SAT_DEBUG_STATS, "  normal: %d, %d literals\n", solv->nrules - 1 - binr, lits);
    }
  POOL_DEBUG(SAT_DEBUG_SCHUBI, "----- unifyrules end -----\n");  
}

#if 0

/*
 * hash rule
 */

static Hashval
hashrule(Solver *solv, Id p, Id d, int n)
{
  unsigned int x = (unsigned int)p;
  int *dp;

  if (n <= 1)
    return (x * 37) ^ (unsigned int)d; 
  dp = solv->pool->whatprovidesdata + d;
  while (*dp)
    x = (x * 37) ^ (unsigned int)*dp++;
  return x;
}
#endif


/*
 * add rule
 *  p = direct literal; always < 0 for installed rpm rules
 *  d, if < 0 direct literal, if > 0 offset into whatprovides, if == 0 rule is assertion (look at p only)
 *
 *
 * A requires b, b provided by B1,B2,B3 => (-A|B1|B2|B3)
 * 
 * p < 0 : pkg id of A
 * d > 0 : Offset in whatprovidesdata (list of providers of b)
 * 
 * A conflicts b, b provided by B1,B2,B3 => (-A|-B1), (-A|-B2), (-A|-B3)
 * p < 0 : pkg id of A
 * d < 0 : Id of solvable (e.g. B1)
 * 
 * d == 0: unary rule, assertion => (A) or (-A)
 * 
 *   Install:    p > 0, d = 0   (A)             user requested install
 *   Remove:     p < 0, d = 0   (-A)            user requested remove
 *   Requires:   p < 0, d > 0   (-A|B1|B2|...)  d: <list of providers for requirement of p>
 *   Updates:    p > 0, d > 0   (A|B1|B2|...)   d: <list of updates for solvable p>
 *   Conflicts:  p < 0, d < 0   (-A|-B)         either p (conflict issuer) or d (conflict provider) (binary rule)
 *   ?           p > 0, d < 0   (A|-B)
 *   No-op ?:    p = 0, d = 0   (null)          (used as policy rule placeholder)
 *
 *   resulting watches:
 *   ------------------
 *   Direct assertion (no watch needed)( if d <0 ) --> d = 0, w1 = p, w2 = 0
 *   Binary rule: p = first literal, d = 0, w2 = second literal, w1 = p
 *   every other : w1 = p, w2 = whatprovidesdata[d];
 *   Disabled rule: w1 = 0
 *
 *   always returns a rule for non-rpm rules
 */

static Rule *
addrule(Solver *solv, Id p, Id d)
{
  Pool *pool = solv->pool;
  Rule *r = 0;
  Id *dp = 0;

  int n = 0;			       /* number of literals in rule - 1
					  0 = direct assertion (single literal)
					  1 = binary rule
					*/

  /* it often happenes that requires lead to adding the same rpm rule
   * multiple times, so we prune those duplicates right away to make
   * the work for unifyrules a bit easier */

  if (solv->nrules && !solv->jobrules)
    {
      r = solv->rules + solv->nrules - 1;   /* get the last added rule */
      if (r->p == p && r->d == d && d != 0)   /* identical and not user requested */
	return r;
    }

  if (d < 0)
    {
      /* always a binary rule */
      if (p == d)
	return 0;		       /* ignore self conflict */
      n = 1;
    }
  else if (d > 0)
    {
      for (dp = pool->whatprovidesdata + d; *dp; dp++, n++)
	if (*dp == -p)
	  return 0;			/* rule is self-fulfilling */
      if (n == 1)
	d = dp[-1];                     /* take single literal */
    }

  if (n == 0 && !solv->jobrules)
    {
      /* this is a rpm rule assertion, we do not have to allocate it */
      /* it can be identified by a level of 1 and a zero reason */
      /* we must not drop those rules from the decisionq when rewinding! */
      assert(p < 0);
      assert(solv->decisionmap[-p] == 0 || solv->decisionmap[-p] == -1);
      if (solv->decisionmap[-p])
	return 0;	/* already got that one */
      queue_push(&solv->decisionq, p);
      queue_push(&solv->decisionq_why, 0);
      solv->decisionmap[-p] = -1;
      return 0;
    }
  if (n == 1 && p > d)
    {
      /* smallest literal first so we can find dups */
      n = p;
      p = d;
      d = n;
      n = 1;			       /* re-set n, was used as temp var */
    }

  /* check if the last added rule is exactly the same as what we're looking for. */
  if (r && n == 1 && !r->d && r->p == p && r->w2 == d)
    return r;  /* binary rule */

  if (r && n > 1 && r->d && r->p == p)
    {
      /* Rule where d is an offset in whatprovidesdata */
      Id *dp2;
      if (d == r->d)
	return r;
      dp2 = pool->whatprovidesdata + r->d;
      for (dp = pool->whatprovidesdata + d; *dp; dp++, dp2++)
	if (*dp != *dp2)
	  break;
      if (*dp == *dp2)
	return r;
   }
  
  /*
   * allocate new rule
   */

  /* check and extend rule buffer */
  if ((solv->nrules & RULES_BLOCK) == 0)
    {
      solv->rules = (Rule *)sat_realloc(solv->rules, (solv->nrules + (RULES_BLOCK + 1)) * sizeof(Rule));
    }

  r = solv->rules + solv->nrules++;    /* point to rule space */

  r->p = p;
  if (n == 0)
    {
      /* direct assertion, no watch needed */
      r->d = 0;
      r->w1 = p;
      r->w2 = 0;
    }
  else if (n == 1)
    {
      /* binary rule */
      r->d = 0;
      r->w1 = p;
      r->w2 = d;
    }
  else
    {
      r->d = d;
      r->w1 = p;
      r->w2 = pool->whatprovidesdata[d];
    }
  r->n1 = 0;
  r->n2 = 0;

  IF_POOLDEBUG (SAT_DEBUG_RULE_CREATION)
    {
      POOL_DEBUG(SAT_DEBUG_RULE_CREATION, "  Add rule: ");
      printrule(solv, SAT_DEBUG_RULE_CREATION, r);
    }
  
  return r;
}

static inline void
disablerule(Solver *solv, Rule *r)
{
  r->w1 = 0;
}

static inline void
enablerule(Solver *solv, Rule *r)
{
  if (r->d == 0 || r->w2 != r->p)
    r->w1 = r->p;
  else
    r->w1 = solv->pool->whatprovidesdata[r->d];
}


/**********************************************************************************/

/* a problem is an item on the solver's problem list. It can either be >0, in that
 * case it is a system (upgrade) rule, or it can be <0, which makes it refer to a job
 * consisting of multiple job rules.
 */

static void 
disableproblem(Solver *solv, Id v)
{
  Rule *r;
  int i;
  Id *jp;

  if (v > 0)
    {
      disablerule(solv, solv->rules + v);
      return;
    }
  v = -(v + 1);
  jp = solv->ruletojob.elements;
  for (i = solv->jobrules, r = solv->rules + i; i < solv->systemrules; i++, r++, jp++)
    if (*jp == v)
      disablerule(solv, r);
}

static void 
enableproblem(Solver *solv, Id v)
{
  Rule *r;
  int i;
  Id *jp;

  if (v > 0)
    {
      enablerule(solv, solv->rules + v);
      return;
    }
  v = -(v + 1);
  jp = solv->ruletojob.elements;
  for (i = solv->jobrules, r = solv->rules + i; i < solv->systemrules; i++, r++, jp++)
    if (*jp == v)
      enablerule(solv, r);
}

static void
printproblem(Solver *solv, Id v)
{
  Pool *pool = solv->pool;
  int i;
  Rule *r;
  Id *jp;

  if (v > 0)
    printrule(solv, SAT_DEBUG_SOLUTIONS, solv->rules + v);
  else
    {
      v = -(v + 1);
      POOL_DEBUG(SAT_DEBUG_SOLUTIONS, "JOB %d\n", v);
      jp = solv->ruletojob.elements;
      for (i = solv->jobrules, r = solv->rules + i; i < solv->systemrules; i++, r++, jp++)
	if (*jp == v)
	  {
	    POOL_DEBUG(SAT_DEBUG_SOLUTIONS, "- ");
	    printrule(solv, SAT_DEBUG_SOLUTIONS, r);
	  }
      POOL_DEBUG(SAT_DEBUG_SOLUTIONS, "ENDJOB\n");
    }
}



/************************************************************************/

/* go through system and job rules and add direct assertions
 * to the decisionqueue. If we find a conflict, disable rules and
 * add them to problem queue.
 */
static void
makeruledecisions(Solver *solv)
{
  Pool *pool = solv->pool;
  int i, ri;
  Rule *r, *rr;
  Id v, vv;
  int decisionstart;

  POOL_DEBUG(SAT_DEBUG_SCHUBI, "----- makeruledecisions ; size decisionq: %d -----\n",solv->decisionq.count);
  
  decisionstart = solv->decisionq.count;
  /* rpm rules don't have assertions, so we can start with the job
   * rules */
  for (ri = solv->jobrules, r = solv->rules + ri; ri < solv->nrules; ri++, r++)
    {
      if (!r->w1 || r->w2)	/* disabled or no assertion */
	continue;
      v = r->p;
      vv = v > 0 ? v : -v;
      if (solv->decisionmap[vv] == 0)
	{
	  queue_push(&solv->decisionq, v);
	  queue_push(&solv->decisionq_why, r - solv->rules);
	  solv->decisionmap[vv] = v > 0 ? 1 : -1;
	  IF_POOLDEBUG (SAT_DEBUG_PROPAGATE)
	    {
	      Solvable *s = solv->pool->solvables + vv;
	      if (v < 0)
		POOL_DEBUG(SAT_DEBUG_PROPAGATE, "conflicting %s (assertion)\n", solvable2str(solv->pool, s));
	      else
		POOL_DEBUG(SAT_DEBUG_PROPAGATE, "installing  %s (assertion)\n", solvable2str(solv->pool, s));
	    }
	  continue;
	}
      if (v > 0 && solv->decisionmap[vv] > 0)
	continue;
      if (v < 0 && solv->decisionmap[vv] < 0)
	continue;
      /* found a conflict! */
      /* ri >= learntrules cannot happen, as this would mean that the
       * problem was not solvable, so we wouldn't have created the
       * learnt rule at all */
      assert(ri < solv->learntrules);
      /* if we are weak, just disable ourself */
      if (ri >= solv->weakrules)
	{
	  POOL_DEBUG(SAT_DEBUG_UNSOLVABLE, "assertion conflict, but I am weak, disabling ");
	  printrule(solv, SAT_DEBUG_UNSOLVABLE, r);
	  disablerule(solv, r);
	  continue;
	}

      /* only job and system rules left in the decisionq*/
      /* find the decision which is the "opposite" of the jobrule */
      for (i = 0; i < solv->decisionq.count; i++)
	if (solv->decisionq.elements[i] == -v)
	  break;
      assert(i < solv->decisionq.count);
      if (solv->decisionq_why.elements[i] == 0)
	{
	  /* conflict with rpm rule, need only disable our rule */
	  assert(v > 0 || v == -SYSTEMSOLVABLE);
	  /* record proof */
	  queue_push(&solv->problems, solv->learnt_pool.count);
	  queue_push(&solv->learnt_pool, ri);
	  queue_push(&solv->learnt_pool, v != -SYSTEMSOLVABLE ? -v : v);
	  queue_push(&solv->learnt_pool, 0);
	  POOL_DEBUG(SAT_DEBUG_UNSOLVABLE, "conflict with rpm rule, disabling rule #%d\n", ri);
	  if (ri < solv->systemrules)
	    v = -(solv->ruletojob.elements[ri - solv->jobrules] + 1);
	  else
	    v = ri;
	  queue_push(&solv->problems, v);
	  queue_push(&solv->problems, 0);
	  disableproblem(solv, v);
	  continue;
	}

      /* conflict with another job or system rule */
      /* record proof */
      queue_push(&solv->problems, solv->learnt_pool.count);
      queue_push(&solv->learnt_pool, ri);
      queue_push(&solv->learnt_pool, solv->decisionq_why.elements[i]);
      queue_push(&solv->learnt_pool, 0);

      POOL_DEBUG(SAT_DEBUG_UNSOLVABLE, "conflicting system/job assertions over literal %d\n", vv);
      /* push all of our rules asserting this literal on the problem stack */
      for (i = solv->jobrules, rr = solv->rules + i; i < solv->nrules; i++, rr++)
	{
	  if (!rr->w1 || rr->w2)
	    continue;
	  if (rr->p != vv && rr->p != -vv)
	    continue;
	  POOL_DEBUG(SAT_DEBUG_UNSOLVABLE, " - disabling rule #%d\n", i);
	  v = i;
	  if (i < solv->systemrules)
	    v = -(solv->ruletojob.elements[i - solv->jobrules] + 1);
	  queue_push(&solv->problems, v);
	  disableproblem(solv, v);
	}
      queue_push(&solv->problems, 0);

      /* start over */
      while (solv->decisionq.count > decisionstart)
	{
	  v = solv->decisionq.elements[--solv->decisionq.count];
	  --solv->decisionq_why.count;
	  vv = v > 0 ? v : -v;
	  solv->decisionmap[vv] = 0;
	}
      ri = solv->jobrules - 1;
      r = solv->rules + ri;
    }
  
    POOL_DEBUG(SAT_DEBUG_SCHUBI, "----- makeruledecisions end; size decisionq: %d -----\n",solv->decisionq.count);
}

/*
 * we have enabled or disabled some of our rules. We now reenable all
 * of our learnt rules but the ones that were learnt from rules that
 * are now disabled.
 */
static void
enabledisablelearntrules(Solver *solv)
{
  Pool *pool = solv->pool;
  Rule *r;
  Id why, *whyp;
  int i;

  POOL_DEBUG(SAT_DEBUG_SOLUTIONS, "enabledisablelearntrules called\n");
  for (i = solv->learntrules, r = solv->rules + i; i < solv->nrules; i++, r++)
    {
      whyp = solv->learnt_pool.elements + solv->learnt_why.elements[i - solv->learntrules];
      while ((why = *whyp++) != 0)
	{
	  if (why < 0)
	    continue;		/* rpm assertion */
	  assert(why < i);
	  if (!solv->rules[why].w1)
	    break;
	}
      /* why != 0: we found a disabled rule, disable the learnt rule */
      if (why && r->w1)
	{
	  IF_POOLDEBUG (SAT_DEBUG_SOLUTIONS)
	    {
	      POOL_DEBUG(SAT_DEBUG_SOLUTIONS, "disabling learnt ");
	      printrule(solv, SAT_DEBUG_SOLUTIONS, r);
	    }
          disablerule(solv, r);
	}
      else if (!why && !r->w1)
	{
	  IF_POOLDEBUG (SAT_DEBUG_SOLUTIONS)
	    {
	      POOL_DEBUG(SAT_DEBUG_SOLUTIONS, "re-enabling learnt ");
	      printrule(solv, SAT_DEBUG_SOLUTIONS, r);
	    }
          enablerule(solv, r);
	}
    }
}


/* FIXME: bad code ahead, replace as soon as possible */
static void
disableupdaterules(Solver *solv, Queue *job, int jobidx)
{
  Pool *pool = solv->pool;
  int i, j;
  Id how, what, p, *pp;
  Solvable *s;
  Repo *installed;
  Rule *r;
  Id lastjob = -1;

  installed = solv->installed;
  if (!installed)
    return;

  if (jobidx != -1)
    {
      how = job->elements[jobidx];
      switch(how)
	{
	case SOLVER_INSTALL_SOLVABLE:
	case SOLVER_ERASE_SOLVABLE:
	case SOLVER_ERASE_SOLVABLE_NAME:
	case SOLVER_ERASE_SOLVABLE_PROVIDES:
	  break;
	default:
	  return;
	}
    }
  /* go through all enabled job rules */
  MAPZERO(&solv->noupdate);
  for (i = solv->jobrules; i < solv->systemrules; i++)
    {
      r = solv->rules + i;
      if (!r->w1)	/* disabled? */
	continue;
      j = solv->ruletojob.elements[i - solv->jobrules];
      if (j == lastjob)
	continue;
      lastjob = j;
      how = job->elements[j];
      what = job->elements[j + 1];
      switch(how)
	{
	case SOLVER_INSTALL_SOLVABLE:			/* install specific solvable */
	  s = pool->solvables + what;
	  FOR_PROVIDES(p, pp, s->name)
	    {
	      if (pool->solvables[p].name != s->name)
		continue;
	      if (pool->solvables[p].repo == installed)
	        MAPSET(&solv->noupdate, p - installed->start);
	    }
	  break;
	case SOLVER_ERASE_SOLVABLE:
	  s = pool->solvables + what;
	  if (s->repo == installed)
	    MAPSET(&solv->noupdate, what - installed->start);
	  break;
	case SOLVER_ERASE_SOLVABLE_NAME:                  /* remove by capability */
	case SOLVER_ERASE_SOLVABLE_PROVIDES:
	  FOR_PROVIDES(p, pp, what)
	    {
	      if (how == SOLVER_ERASE_SOLVABLE_NAME && pool->solvables[p].name != what)
	        continue;
	      if (pool->solvables[p].repo == installed)
	        MAPSET(&solv->noupdate, p - installed->start);
	    }
	  break;
	default:
	  break;
	}
    }

  /* fixup update rule status */
  if (solv->allowuninstall)
    return;		/* no update rules at all */

  if (jobidx != -1)
    {
      /* we just disabled job #jobidx. enable all update rules
       * that aren't disabled by the remaining job rules */
      how = job->elements[jobidx];
      what = job->elements[jobidx + 1];
      switch(how)
	{
	case SOLVER_INSTALL_SOLVABLE:
	  s = pool->solvables + what;
	  FOR_PROVIDES(p, pp, s->name)
	    {
	      if (pool->solvables[p].name != s->name)
		continue;
	      if (pool->solvables[p].repo != installed)
		continue;
	      if (MAPTST(&solv->noupdate, p - installed->start))
		continue;
	      r = solv->rules + solv->systemrules + (p - installed->start);
	      if (r->w1)
		continue;
	      enablerule(solv, r);
	      IF_POOLDEBUG (SAT_DEBUG_SOLUTIONS)
		{
		  POOL_DEBUG(SAT_DEBUG_SOLUTIONS, "@@@ re-enabling ");
		  printrule(solv, SAT_DEBUG_SOLUTIONS, r);
		}
	    }
	  break;
	case SOLVER_ERASE_SOLVABLE:
	  s = pool->solvables + what;
	  if (s->repo != installed)
	    break;
	  if (MAPTST(&solv->noupdate, what - installed->start))
	    break;
	  r = solv->rules + solv->systemrules + (what - installed->start);
	  if (r->w1)
	    break;
	  enablerule(solv, r);
	  IF_POOLDEBUG (SAT_DEBUG_SOLUTIONS)
	    {
	      POOL_DEBUG(SAT_DEBUG_SOLUTIONS, "@@@ re-enabling ");
	      printrule(solv, SAT_DEBUG_SOLUTIONS, r);
	    }
	  break;
	case SOLVER_ERASE_SOLVABLE_NAME:                  /* remove by capability */
	case SOLVER_ERASE_SOLVABLE_PROVIDES:
	  FOR_PROVIDES(p, pp, what)
	    {
	      if (how == SOLVER_ERASE_SOLVABLE_NAME && pool->solvables[p].name != what)
	        continue;
	      if (pool->solvables[p].repo != installed)
		continue;
	      if (MAPTST(&solv->noupdate, p - installed->start))
		continue;
	      r = solv->rules + solv->systemrules + (p - installed->start);
	      if (r->w1)
		continue;
	      enablerule(solv, r);
	      IF_POOLDEBUG (SAT_DEBUG_SOLUTIONS)
		{
		  POOL_DEBUG(SAT_DEBUG_SOLUTIONS, "@@@ re-enabling ");
		  printrule(solv, SAT_DEBUG_SOLUTIONS, r);
		}
	    }
	  break;
	default:
	  break;
	}
      return;
    }

  for (i = 0; i < installed->nsolvables; i++)
    {
      r = solv->rules + solv->systemrules + i;
      if (r->w1 && MAPTST(&solv->noupdate, i))
        disablerule(solv, r);	/* was enabled, need to disable */
    }
}


/*
 * add (install) rules for solvable
 * for unfulfilled requirements, conflicts, obsoletes,....
 * add a negative assertion for solvables that are not installable
 */

static void
addrpmrulesforsolvable(Solver *solv, Solvable *s, Map *m)
{
  Pool *pool = solv->pool;
  Repo *installed = solv->installed;
  Queue q;
  Id qbuf[64];
  int i;
  int dontfix;
  Id req, *reqp;
  Id con, *conp;
  Id obs, *obsp;
  Id rec, *recp;
  Id sug, *sugp;
  Id p, *pp;
  Id *dp;
  Id n;

  POOL_DEBUG(SAT_DEBUG_SCHUBI, "----- addrpmrulesforsolvable -----\n");
  
  queue_init_buffer(&q, qbuf, sizeof(qbuf)/sizeof(*qbuf));
  queue_push(&q, s - pool->solvables);	/* push solvable Id */

  while (q.count)
    {
      /*
       * n: Id of solvable
       * s: Pointer to solvable
       */
      
      n = queue_shift(&q);
      if (MAPTST(m, n))		       /* continue if already done */
	continue;

      MAPSET(m, n);
      s = pool->solvables + n;	       /* s = Solvable in question */

      dontfix = 0;
      if (installed			/* Installed system available */
	  && !solv->fixsystem		/* NOT repair errors in rpm dependency graph */
	  && s->repo == installed)	/* solvable is installed? */
      {
	dontfix = 1;		       /* dont care about broken rpm deps */
      }

      if (!dontfix && s->arch != ARCH_SRC && s->arch != ARCH_NOSRC && !pool_installable(pool, s))
	{
	  POOL_DEBUG(SAT_DEBUG_RULE_CREATION, "package %s [%d] is not installable\n", solvable2str(pool, s), (Id)(s - pool->solvables));
	  addrule(solv, -n, 0);		/* uninstallable */
	}

      /*-----------------------------------------
       * check requires of s
       */
      
      if (s->requires)
	{
	  reqp = s->repo->idarraydata + s->requires;
	  while ((req = *reqp++) != 0) /* go throw all requires */
	    {
	      if (req == SOLVABLE_PREREQMARKER)   /* skip the marker */
		continue;

	      dp = pool_whatprovides(pool, req);

	      if (*dp == SYSTEMSOLVABLE)	/* always installed */
		continue;

	      if (dontfix)
		{
		  /* the strategy here is to not insist on dependencies
                   * that are already broken. so if we find one provider
                   * that was already installed, we know that the
                   * dependency was not broken before so we enforce it */
		  for (i = 0; (p = dp[i]) != 0; i++)	/* for all providers */
		    {
		      if (pool->solvables[p].repo == installed)
			break;		/* provider was installed */
		    }
		  if (!p)		/* previously broken dependency */
		    {
		      POOL_DEBUG(SAT_DEBUG_RULE_CREATION, "ignoring broken requires %s of installed package %s\n", dep2str(pool, req), solvable2str(pool, s));
		      continue;
		    }
		}

	      if (!*dp)
		{
		  /* nothing provides req! */
		  POOL_DEBUG(SAT_DEBUG_RULE_CREATION, "package %s [%d] is not installable (%s)\n", solvable2str(pool, s), (Id)(s - pool->solvables), dep2str(pool, req));
		  addrule(solv, -n, 0); /* mark requestor as uninstallable */
		  continue;
		}

	      IF_POOLDEBUG (SAT_DEBUG_RULE_CREATION)
	        {
		  POOL_DEBUG(SAT_DEBUG_RULE_CREATION,"  %s requires %s\n", solvable2str(pool, s), dep2str(pool, req));
		  for (i = 0; dp[i]; i++)
		    POOL_DEBUG(SAT_DEBUG_RULE_CREATION, "   provided by %s\n", solvable2str(pool, pool->solvables + dp[i]));
	        }

	      /* add 'requires' dependency */
              /* rule: (-requestor|provider1|provider2|...|providerN) */
	      addrule(solv, -n, dp - pool->whatprovidesdata);

	      /* descend the dependency tree */
	      for (; *dp; dp++)   /* loop through all providers */
		{
		  if (!MAPTST(m, *dp))
		    queue_push(&q, *dp);
		}

	    } /* while, requirements of n */

	} /* if, requirements */

      /* that's all we check for src packages */
      if (s->arch == ARCH_SRC || s->arch == ARCH_NOSRC)
	continue;
      
      /*-----------------------------------------
       * check conflicts of s
       */
      
      if (s->conflicts)
	{
	  conp = s->repo->idarraydata + s->conflicts;
	  while ((con = *conp++) != 0)
	    {
	      FOR_PROVIDES(p, pp, con)
		{
		   /* dontfix: dont care about conflicts with already installed packs */
		  if (dontfix && pool->solvables[p].repo == installed)
		    continue;
                 /* rule: -n|-p: either solvable _or_ provider of conflict */
		  addrule(solv, -n, -p);
		}
	    }
	}

      /*-----------------------------------------
       * check obsoletes if not installed
       */
      if (!installed || pool->solvables[n].repo != installed)
	{			       /* not installed */
	  if (s->obsoletes)
	    {
	      obsp = s->repo->idarraydata + s->obsoletes;
	      while ((obs = *obsp++) != 0)
		{
		  FOR_PROVIDES(p, pp, obs)
		    addrule(solv, -n, -p);
		}
	    }
	  FOR_PROVIDES(p, pp, s->name)
	    {
	      if (s->name == pool->solvables[p].name)
		addrule(solv, -n, -p);
	    }
	}

      /*-----------------------------------------
       * add recommends to the rule list
       */
      if (s->recommends)
	{
	  recp = s->repo->idarraydata + s->recommends;
	  while ((rec = *recp++) != 0)
	    {
	      FOR_PROVIDES(p, pp, rec)
		if (!MAPTST(m, p))
		  queue_push(&q, p);
	    }
	}
      if (s->suggests)
	{
	  sugp = s->repo->idarraydata + s->suggests;
	  while ((sug = *sugp++) != 0)
	    {
	      FOR_PROVIDES(p, pp, sug)
		if (!MAPTST(m, p))
		  queue_push(&q, p);
	    }
	}
    }
  queue_free(&q);
  POOL_DEBUG(SAT_DEBUG_SCHUBI, "----- addrpmrulesforsolvable end -----\n");
}

static void
addrpmrulesforweak(Solver *solv, Map *m)
{
  Pool *pool = solv->pool;
  Solvable *s;
  Id sup, *supp;
  int i, n;

  POOL_DEBUG(SAT_DEBUG_SCHUBI, "----- addrpmrulesforweak -----\n");
  for (i = n = 1; n < pool->nsolvables; i++, n++)
    {
      if (i == pool->nsolvables)
	i = 1;
      if (MAPTST(m, i))
	continue;
      s = pool->solvables + i;
      if (!pool_installable(pool, s))
	continue;
      sup = 0;
      if (s->supplements)
	{
	  supp = s->repo->idarraydata + s->supplements;
	  while ((sup = *supp++) != ID_NULL)
	    if (dep_possible(solv, sup, m))
	      break;
	}
      if (!sup && s->freshens)
	{
	  supp = s->repo->idarraydata + s->freshens;
	  while ((sup = *supp++) != ID_NULL)
	    if (dep_possible(solv, sup, m))
	      break;
	}
      if (!sup && s->enhances)
	{
	  supp = s->repo->idarraydata + s->enhances;
	  while ((sup = *supp++) != ID_NULL)
	    if (dep_possible(solv, sup, m))
	      break;
	}
      if (!sup)
	continue;
      addrpmrulesforsolvable(solv, s, m);
      n = 0;
    }
  POOL_DEBUG(SAT_DEBUG_SCHUBI, "----- addrpmrulesforweak end -----\n");  
}

static void
addrpmrulesforupdaters(Solver *solv, Solvable *s, Map *m, int allowall)
{
  Pool *pool = solv->pool;
  int i;
  Queue qs;
  Id qsbuf[64];

  POOL_DEBUG(SAT_DEBUG_SCHUBI, "----- addrpmrulesforupdaters -----\n");

  queue_init_buffer(&qs, qsbuf, sizeof(qsbuf)/sizeof(*qsbuf));
  policy_findupdatepackages(solv, s, &qs, allowall);
  if (!MAPTST(m, s - pool->solvables))	/* add rule for s if not already done */
    addrpmrulesforsolvable(solv, s, m); 
  for (i = 0; i < qs.count; i++)
    if (!MAPTST(m, qs.elements[i]))
      addrpmrulesforsolvable(solv, pool->solvables + qs.elements[i], m);
  queue_free(&qs);
  
  POOL_DEBUG(SAT_DEBUG_SCHUBI, "----- addrpmrulesforupdaters -----\n");
}

/*
 * add rule for update
 *   (A|A1|A2|A3...)  An = update candidates for A
 * 
 * s = (installed) solvable
 */

static void
addupdaterule(Solver *solv, Solvable *s, int allowall)
{
  /* installed packages get a special upgrade allowed rule */
  Pool *pool = solv->pool;
  Id d;
  Queue qs;
  Id qsbuf[64];

  POOL_DEBUG(SAT_DEBUG_SCHUBI, "-----  addupdaterule -----\n");

  queue_init_buffer(&qs, qsbuf, sizeof(qsbuf)/sizeof(*qsbuf));
  policy_findupdatepackages(solv, s, &qs, allowall);
  if (qs.count == 0)		       /* no updaters found */
    d = 0;
  else
    d = pool_queuetowhatprovides(pool, &qs);	/* intern computed queue */
  queue_free(&qs);
  addrule(solv, s - pool->solvables, d);	/* allow update of s */
  POOL_DEBUG(SAT_DEBUG_SCHUBI, "-----  addupdaterule end -----\n");  
}


/*-----------------------------------------------------------------*/
/* watches */

/*
 * print watches
 *
 */

static void
printWatches(Solver *solv, int type)
{
  Pool *pool = solv->pool;
  int counter;
    
  POOL_DEBUG(type, "Watches: \n");

  for (counter = -(pool->nsolvables); counter <= pool->nsolvables; counter ++)
     {
	 POOL_DEBUG(type, "    solvable [%d] -- rule [%d]\n", counter, solv->watches[counter+pool->nsolvables]);
     }
}

/*
 * makewatches
 * 
 * initial setup for all watches
 */

static void
makewatches(Solver *solv)
{
  Rule *r;
  int i;
  int nsolvables = solv->pool->nsolvables;

  sat_free(solv->watches);
				       /* lower half for removals, upper half for installs */
  solv->watches = sat_calloc(2 * nsolvables, sizeof(Id));
#if 1
  /* do it reverse so rpm rules get triggered first */
  for (i = 1, r = solv->rules + solv->nrules - 1; i < solv->nrules; i++, r--)
#else
  for (i = 1, r = solv->rules + 1; i < solv->nrules; i++, r++)
#endif
    {
      if (!r->w1	               /* rule is disabled */
	  || !r->w2)		       /* rule is assertion */
	continue;

      /* see addwatches(solv, r) */
      r->n1 = solv->watches[nsolvables + r->w1];
      solv->watches[nsolvables + r->w1] = r - solv->rules;

      r->n2 = solv->watches[nsolvables + r->w2];
      solv->watches[nsolvables + r->w2] = r - solv->rules;
    }
}


/*
 * add watches (for rule)
 */

static void
addwatches(Solver *solv, Rule *r)
{
  int nsolvables = solv->pool->nsolvables;

  r->n1 = solv->watches[nsolvables + r->w1];
  solv->watches[nsolvables + r->w1] = r - solv->rules;

  r->n2 = solv->watches[nsolvables + r->w2];
  solv->watches[nsolvables + r->w2] = r - solv->rules;
}


/*-----------------------------------------------------------------*/
/* rule propagation */

#define DECISIONMAP_TRUE(p) ((p) > 0 ? (decisionmap[p] > 0) : (decisionmap[-p] < 0))

/*
 * propagate
 * 
 * propagate decision to all rules
 * return : 0 = everything is OK
 *          watched rule = there is a conflict
 */

static Rule *
propagate(Solver *solv, int level)
{
  Pool *pool = solv->pool;
  Id *rp, *nrp;
  Rule *r;
  Id p, pkg, ow;
  Id *dp;
  Id *decisionmap = solv->decisionmap;
  Id *watches = solv->watches + pool->nsolvables;

  POOL_DEBUG(SAT_DEBUG_PROPAGATE, "----- propagate -----\n");

  while (solv->propagate_index < solv->decisionq.count)
    {
      /* negate because our watches trigger if literal goes FALSE */
      pkg = -solv->decisionq.elements[solv->propagate_index++];
      IF_POOLDEBUG (SAT_DEBUG_PROPAGATE)
        {
	  POOL_DEBUG(SAT_DEBUG_PROPAGATE, "popagate for decision %d level %d\n", -pkg, level);
	  printruleelement(solv, SAT_DEBUG_PROPAGATE, 0, -pkg);
	  printWatches(solv, SAT_DEBUG_SCHUBI);
        }

      for (rp = watches + pkg; *rp; rp = nrp)
	{
	  r = solv->rules + *rp;
	  
	  IF_POOLDEBUG (SAT_DEBUG_PROPAGATE)
	    {
	      POOL_DEBUG(SAT_DEBUG_PROPAGATE,"  watch triggered ");
	      printrule(solv, SAT_DEBUG_PROPAGATE, r);
	    }
	  
	  if (pkg == r->w1)
	    {
	      ow = r->w2; /* regard the second watchpoint to come to a solution */
	      nrp = &r->n1;
	    }
	  else
	    {
	      ow = r->w1; /* regard the first watchpoint to come to a solution */
	      nrp = &r->n2;
	    }
	  /* if clause is TRUE, nothing to do */
	  if (DECISIONMAP_TRUE(ow))
	    continue;

          if (r->d)
	    {
	      /* not a binary clause, check if we need to move our watch */
	      /* search for a literal that is not ow and not false */
	      /* (true is also ok, in that case the rule is fulfilled) */
	      if (r->p && r->p != ow && !DECISIONMAP_TRUE(-r->p))
		p = r->p;
	      else
		for (dp = pool->whatprovidesdata + r->d; (p = *dp++) != 0;)
		  if (p != ow && !DECISIONMAP_TRUE(-p))
		    break;
	      if (p)
		{
		  /* p is free to watch, move watch to p */
		  IF_POOLDEBUG (SAT_DEBUG_PROPAGATE)
		    {
		      if (p > 0)
			POOL_DEBUG(SAT_DEBUG_PROPAGATE, "    -> move w%d to %s\n", (pkg == r->w1 ? 1 : 2), solvable2str(pool, pool->solvables + p));
		      else
			POOL_DEBUG(SAT_DEBUG_PROPAGATE,"    -> move w%d to !%s\n", (pkg == r->w1 ? 1 : 2), solvable2str(pool, pool->solvables - p));
		    }
		  *rp = *nrp;
		  nrp = rp;
		  if (pkg == r->w1)
		    {
		      r->w1 = p;
		      r->n1 = watches[p];
		    }
		  else
		    {
		      r->w2 = p;
		      r->n2 = watches[p];
		    }
		  watches[p] = r - solv->rules;
		  continue;
		}
	    }
          /* unit clause found, set other watch to TRUE */
	  if (DECISIONMAP_TRUE(-ow))
	    return r;		/* eek, a conflict! */
	  IF_POOLDEBUG (SAT_DEBUG_PROPAGATE)
	    {
	      POOL_DEBUG(SAT_DEBUG_PROPAGATE, "   unit ");
	      printrule(solv, SAT_DEBUG_PROPAGATE, r);
	    }
	  if (ow > 0)
            decisionmap[ow] = level;
	  else
            decisionmap[-ow] = -level;
	  queue_push(&solv->decisionq, ow);
	  queue_push(&solv->decisionq_why, r - solv->rules);
	  IF_POOLDEBUG (SAT_DEBUG_PROPAGATE)
	    {
	      Solvable *s = pool->solvables + (ow > 0 ? ow : -ow);
	      if (ow > 0)
		POOL_DEBUG(SAT_DEBUG_PROPAGATE, "    -> decided to install %s\n", solvable2str(pool, s));
	      else
		POOL_DEBUG(SAT_DEBUG_PROPAGATE, "    -> decided to conflict %s\n", solvable2str(pool, s));
	    }
	}
    }
  POOL_DEBUG(SAT_DEBUG_PROPAGATE, "----- propagate end-----\n");
  
  return 0;	/* all is well */
}


/*-----------------------------------------------------------------*/
/* Analysis */

/*
 * analyze
 *   and learn
 */

static int
analyze(Solver *solv, int level, Rule *c, int *pr, int *dr, int *whyp)
{
  Pool *pool = solv->pool;
  Queue r;
  int rlevel = 1;
  Map seen;		/* global? */
  Id v, vv, *dp, why;
  int l, i, idx;
  int num = 0, l1num = 0;
  int learnt_why = solv->learnt_pool.count;
  Id *decisionmap = solv->decisionmap;
 
  queue_init(&r);

  POOL_DEBUG(SAT_DEBUG_ANALYZE, "ANALYZE at %d ----------------------\n", level);
  map_init(&seen, pool->nsolvables);
  idx = solv->decisionq.count;
  for (;;)
    {
      IF_POOLDEBUG (SAT_DEBUG_ANALYZE)
	printruleclass(solv, SAT_DEBUG_ANALYZE, c);
      queue_push(&solv->learnt_pool, c - solv->rules);
      dp = c->d ? pool->whatprovidesdata + c->d : 0;
      /* go through all literals of the rule */
      for (i = -1; ; i++)
	{
	  if (i == -1)
	    v = c->p;
	  else if (c->d == 0)
	    v = i ? 0 : c->w2;
	  else
	    v = *dp++;
	  if (v == 0)
	    break;

	  if (DECISIONMAP_TRUE(v))	/* the one true literal */
	    continue;
	  vv = v > 0 ? v : -v;
	  if (MAPTST(&seen, vv))
	    continue;
	  l = solv->decisionmap[vv];
	  if (l < 0)
	    l = -l;
	  if (l == 1)
	    {
	      /* a level 1 literal, mark it for later */
	      MAPSET(&seen, vv);	/* mark for scanning in level 1 phase */
	      l1num++;
	      continue;
	    }
	  MAPSET(&seen, vv);
	  if (l == level)
	    num++;			/* need to do this one as well */
	  else
	    {
	      queue_push(&r, v);
	      if (l > rlevel)
		rlevel = l;
	    }
	}
      assert(num > 0);
      for (;;)
	{
	  v = solv->decisionq.elements[--idx];
	  vv = v > 0 ? v : -v;
	  if (MAPTST(&seen, vv))
	    break;
	}
      c = solv->rules + solv->decisionq_why.elements[idx];
      MAPCLR(&seen, vv);
      if (--num == 0)
	break;
    }
  *pr = -v;	/* so that v doesn't get lost */

  /* add involved level 1 rules */
  if (l1num)
    {
      POOL_DEBUG(SAT_DEBUG_ANALYZE, "got %d involved level 1 decisions\n", l1num);
      idx++;
      while (idx)
	{
	  v = solv->decisionq.elements[--idx];
	  vv = v > 0 ? v : -v;
	  if (!MAPTST(&seen, vv))
	    continue;
	  why = solv->decisionq_why.elements[idx];
	  if (!why)
	    {
	      queue_push(&solv->learnt_pool, -vv); 
	      IF_POOLDEBUG (SAT_DEBUG_ANALYZE)
		{
		  POOL_DEBUG(SAT_DEBUG_ANALYZE, "RPM ASSERT Rule:\n");
		  printruleelement(solv, SAT_DEBUG_ANALYZE, 0, v);
		}
	      continue;
	    }
	  queue_push(&solv->learnt_pool, why);
	  c = solv->rules + why;
	  IF_POOLDEBUG (SAT_DEBUG_ANALYZE)
	    printruleclass(solv, SAT_DEBUG_ANALYZE, c);
	  for (i = -1; ; i++)
	    {
	      if (i == -1)
		v = c->p;
	      else if (c->d == 0)
		v = i ? 0 : c->w2;
	      else
		v = *dp++;
	      if (v == 0)
		break;
	      if (DECISIONMAP_TRUE(v))	/* the one true literal */
		continue;
	      vv = v > 0 ? v : -v;
	      l = solv->decisionmap[vv];
	      if (l != 1 && l != -1)
		continue;
	      MAPSET(&seen, vv);
	    }
	}
    }
  map_free(&seen);

  if (r.count == 0)
    *dr = 0;
  else if (r.count == 1 && r.elements[0] < 0)
    *dr = r.elements[0];
  else
    *dr = pool_queuetowhatprovides(pool, &r);
  IF_POOLDEBUG (SAT_DEBUG_ANALYZE)
    {
      POOL_DEBUG(SAT_DEBUG_ANALYZE, "learned rule for level %d (am %d)\n", rlevel, level);
      printruleelement(solv, SAT_DEBUG_ANALYZE, 0, *pr);
      for (i = 0; i < r.count; i++)
        printruleelement(solv, SAT_DEBUG_ANALYZE, 0, r.elements[i]);
    }
  /* push end marker on learnt reasons stack */
  queue_push(&solv->learnt_pool, 0);
  if (whyp)
    *whyp = learnt_why;
  return rlevel;
}


/*
 * reset_solver
 * reset the solver decisions to right after the rpm rules.
 * called after rules have been enabled/disabled
 */

static void
reset_solver(Solver *solv)
{
  Pool *pool = solv->pool;
  int i;
  Id v;

  /* rewind decisions to direct rpm rule assertions */
  for (i = solv->decisionq.count - 1; i >= solv->directdecisions; i--)
    {
      v = solv->decisionq.elements[i];
      solv->decisionmap[v > 0 ? v : -v] = 0;
    }

  POOL_DEBUG(SAT_DEBUG_UNSOLVABLE, "decisions done reduced from %d to %d\n", solv->decisionq.count, solv->directdecisions);

  solv->decisionq_why.count = solv->directdecisions;
  solv->decisionq.count = solv->directdecisions;
  solv->recommends_index = -1;
  solv->propagate_index = 0;

  /* adapt learnt rule status to new set of enabled/disabled rules */
  enabledisablelearntrules(solv);

  /* redo all job/system decisions */
  makeruledecisions(solv);
  POOL_DEBUG(SAT_DEBUG_UNSOLVABLE, "decisions so far: %d\n", solv->decisionq.count);

  /* recreate watch chains */
  makewatches(solv);
}


/*
 * analyze_unsolvable_rule
 */

static void
analyze_unsolvable_rule(Solver *solv, Rule *r)
{
  Pool *pool = solv->pool;
  int i;
  Id why = r - solv->rules;
  IF_POOLDEBUG (SAT_DEBUG_UNSOLVABLE)
    printruleclass(solv, SAT_DEBUG_UNSOLVABLE, r);
  if (solv->learntrules && why >= solv->learntrules)
    {
      for (i = solv->learnt_why.elements[why - solv->learntrules]; solv->learnt_pool.elements[i]; i++)
	if (solv->learnt_pool.elements[i] > 0)
	  analyze_unsolvable_rule(solv, solv->rules + solv->learnt_pool.elements[i]);
      return;
    }
  /* do not add rpm rules to problem */
  if (why < solv->jobrules)
    return;
  /* turn rule into problem */
  if (why >= solv->jobrules && why < solv->systemrules)
    why = -(solv->ruletojob.elements[why - solv->jobrules] + 1);
  /* return if problem already countains our rule */
  if (solv->problems.count)
    {
      for (i = solv->problems.count - 1; i >= 0; i--)
	if (solv->problems.elements[i] == 0)	/* end of last problem reached? */
	  break;
	else if (solv->problems.elements[i] == why)
	  return;
    }
  queue_push(&solv->problems, why);
}


/*
 * analyze_unsolvable
 *
 * return: 1 - disabled some rules, try again
 *         0 - hopeless
 */

static int
analyze_unsolvable(Solver *solv, Rule *cr, int disablerules)
{
  Pool *pool = solv->pool;
  Rule *r;
  Map seen;		/* global to speed things up? */
  Id v, vv, *dp, why;
  int l, i, idx;
  Id *decisionmap = solv->decisionmap;
  int oldproblemcount;
  int oldlearntpoolcount;
  int lastweak;

  POOL_DEBUG(SAT_DEBUG_UNSOLVABLE, "ANALYZE UNSOLVABLE ----------------------\n");
  oldproblemcount = solv->problems.count;
  oldlearntpoolcount = solv->learnt_pool.count;

  /* make room for proof index */
  /* must update it later, as analyze_unsolvable_rule would confuse
   * it with a rule index if we put the real value in already */
  queue_push(&solv->problems, 0);

  r = cr;
  map_init(&seen, pool->nsolvables);
  queue_push(&solv->learnt_pool, r - solv->rules);
  analyze_unsolvable_rule(solv, r);
  dp = r->d ? pool->whatprovidesdata + r->d : 0;
  for (i = -1; ; i++)
    {
      if (i == -1)
	v = r->p;
      else if (r->d == 0)
	v = i ? 0 : r->w2;
      else
	v = *dp++;
      if (v == 0)
	break;
      if (DECISIONMAP_TRUE(v))	/* the one true literal */
	  continue;
      vv = v > 0 ? v : -v;
      l = solv->decisionmap[vv];
      if (l < 0)
	l = -l;
      MAPSET(&seen, vv);
    }
  idx = solv->decisionq.count;
  while (idx > 0)
    {
      v = solv->decisionq.elements[--idx];
      vv = v > 0 ? v : -v;
      if (!MAPTST(&seen, vv))
	continue;
      why = solv->decisionq_why.elements[idx];
      if (!why)
	{
	  /* level 1 and no why, must be an rpm assertion */
	  IF_POOLDEBUG (SAT_DEBUG_UNSOLVABLE)
	    {
	      POOL_DEBUG(SAT_DEBUG_UNSOLVABLE, "RPM ");
	      printruleelement(solv, SAT_DEBUG_UNSOLVABLE, 0, v);
	    }
	  /* this is the only positive rpm assertion */
	  if (v == SYSTEMSOLVABLE)
	    v = -v;
	  assert(v < 0);
	  queue_push(&solv->learnt_pool, v);
	  continue;
	}
      queue_push(&solv->learnt_pool, why);
      r = solv->rules + why;
      analyze_unsolvable_rule(solv, r);
      dp = r->d ? pool->whatprovidesdata + r->d : 0;
      for (i = -1; ; i++)
	{
	  if (i == -1)
	    v = r->p;
	  else if (r->d == 0)
	    v = i ? 0 : r->w2;
	  else
	    v = *dp++;
	  if (v == 0)
	    break;
	  if (DECISIONMAP_TRUE(v))	/* the one true literal */
	      continue;
	  vv = v > 0 ? v : -v;
	  l = solv->decisionmap[vv];
	  if (l < 0)
	    l = -l;
	  MAPSET(&seen, vv);
	}
    }
  map_free(&seen);
  queue_push(&solv->problems, 0);	/* mark end of this problem */

  lastweak = 0;
  if (solv->weakrules != solv->learntrules)
    {
      for (i = oldproblemcount + 1; i < solv->problems.count - 1; i++)
	{
	  why = solv->problems.elements[i];
	  if (why < solv->weakrules || why >= solv->learntrules)
	    continue;
	  if (!lastweak || lastweak < why)
	    lastweak = why;
	}
    }
  if (lastweak)
    {
      /* disable last weak rule */
      solv->problems.count = oldproblemcount;
      solv->learnt_pool.count = oldlearntpoolcount;
      r = solv->rules + lastweak;
      POOL_DEBUG(SAT_DEBUG_UNSOLVABLE, "disabling weak ");
      printrule(solv, SAT_DEBUG_UNSOLVABLE, r);
      disablerule(solv, r);
      reset_solver(solv);
      return 1;
    }

  /* finish proof */
  queue_push(&solv->learnt_pool, 0);
  solv->problems.elements[oldproblemcount] = oldlearntpoolcount;

  if (disablerules)
    {
      for (i = oldproblemcount + 1; i < solv->problems.count - 1; i++)
        disableproblem(solv, solv->problems.elements[i]);
      reset_solver(solv);
      return 1;
    }
  POOL_DEBUG(SAT_DEBUG_UNSOLVABLE, "UNSOLVABLE\n");
  return 0;
}


/*-----------------------------------------------------------------*/
/* Decision revert */

/*
 * revert
 * revert decision at level
 */

static void
revert(Solver *solv, int level)
{
  Pool *pool = solv->pool;
  Id v, vv;
  while (solv->decisionq.count)
    {
      v = solv->decisionq.elements[solv->decisionq.count - 1];
      vv = v > 0 ? v : -v;
      if (solv->decisionmap[vv] <= level && solv->decisionmap[vv] >= -level)
        break;
      POOL_DEBUG(SAT_DEBUG_PROPAGATE, "reverting decision %d at %d\n", v, solv->decisionmap[vv]);
      solv->decisionmap[vv] = 0;
      solv->decisionq.count--;
      solv->decisionq_why.count--;
      solv->propagate_index = solv->decisionq.count;
    }
  while (solv->branches.count && solv->branches.elements[solv->branches.count - 1] <= -level)
    {
      solv->branches.count--;
      while (solv->branches.count && solv->branches.elements[solv->branches.count - 1] >= 0)
	solv->branches.count--;
    }
  solv->recommends_index = -1;
}


/*
 * watch2onhighest - put watch2 on literal with highest level
 */

static inline void
watch2onhighest(Solver *solv, Rule *r)
{
  int l, wl = 0;
  Id v, *dp;

  if (!r->d)
    return;	/* binary rule, both watches are set */
  dp = solv->pool->whatprovidesdata + r->d;
  while ((v = *dp++) != 0)
    {
      l = solv->decisionmap[v < 0 ? -v : v];
      if (l < 0)
	l = -l;
      if (l > wl)
	{
	  r->w2 = dp[-1];
	  wl = l;
	}
    }
}


/*
 * setpropagatelearn
 *
 * add free decision to decisionq, increase level and
 * propagate decision, return if no conflict.
 * in conflict case, analyze conflict rule, add resulting
 * rule to learnt rule set, make decision from learnt
 * rule (always unit) and re-propagate.
 *
 * returns the new solver level or 0 if unsolvable
 *
 */

static int
setpropagatelearn(Solver *solv, int level, Id decision, int disablerules)
{
  Pool *pool = solv->pool;
  Rule *r;
  Id p, d;
  int l, why;

  if (decision)
    {
      level++;
      if (decision > 0)
        solv->decisionmap[decision] = level;
      else
        solv->decisionmap[-decision] = -level;
      queue_push(&solv->decisionq, decision);
      queue_push(&solv->decisionq_why, 0);
    }
  for (;;)
    {
      r = propagate(solv, level);
      if (!r)
	break;
      if (level == 1)
	return analyze_unsolvable(solv, r, disablerules);
      POOL_DEBUG(SAT_DEBUG_ANALYZE, "conflict with rule #%d\n", (int)(r - solv->rules));
      l = analyze(solv, level, r, &p, &d, &why);	/* learnt rule in p and d */
      assert(l > 0 && l < level);
      POOL_DEBUG(SAT_DEBUG_ANALYZE, "reverting decisions (level %d -> %d)\n", level, l);
      level = l;
      revert(solv, level);
      r = addrule(solv, p, d);       /* p requires d */
      assert(r);
      assert(solv->learnt_why.count == (r - solv->rules) - solv->learntrules);
      queue_push(&solv->learnt_why, why);
      if (d)
	{
	  /* at least 2 literals, needs watches */
	  watch2onhighest(solv, r);
	  addwatches(solv, r);
	}
      solv->decisionmap[p > 0 ? p : -p] = p > 0 ? level : -level;
      queue_push(&solv->decisionq, p);
      queue_push(&solv->decisionq_why, r - solv->rules);
      IF_POOLDEBUG (SAT_DEBUG_ANALYZE)
	{
	  POOL_DEBUG(SAT_DEBUG_ANALYZE, "decision: ");
	  printruleelement(solv, SAT_DEBUG_ANALYZE, 0, p);
	  POOL_DEBUG(SAT_DEBUG_ANALYZE, "new rule: ");
	  printrule(solv, SAT_DEBUG_ANALYZE, r);
	}
    }
  return level;
}


/*
 * install best package from the queue. We add an extra package, inst, if
 * provided. See comment in weak install section.
 *
 * returns the new solver level or 0 if unsolvable
 *
 */
static int
selectandinstall(Solver *solv, int level, Queue *dq, Id inst, int disablerules)
{
  Pool *pool = solv->pool;
  Id p;
  int i;

  if (dq->count > 1 || inst)
    policy_filter_unwanted(solv, dq, inst, POLICY_MODE_CHOOSE);

  i = 0;
  if (dq->count > 1)
    {
      /* choose the supplemented one */
      for (i = 0; i < dq->count; i++)
	if (solver_is_supplementing(solv, pool->solvables + dq->elements[i]))
	  break;
      if (i == dq->count)
	{
	  for (i = 1; i < dq->count; i++)
	    queue_push(&solv->branches, dq->elements[i]);
	  queue_push(&solv->branches, -level);
	  i = 0;
	}
    }
  p = dq->elements[i];

  POOL_DEBUG(SAT_DEBUG_POLICY, "installing %s\n", solvable2str(pool, pool->solvables + p));

  return setpropagatelearn(solv, level, p, disablerules);
}


/*-----------------------------------------------------------------*/
/* Main solver interface */


/*
 * solver_create
 * create solver structure
 *
 * pool: all available solvables
 * installed: installed Solvables
 *
 *
 * Upon solving, rules are created to flag the Solvables
 * of the 'installed' Repo as installed.
 */

Solver *
solver_create(Pool *pool, Repo *installed)
{
  Solver *solv;
  solv = (Solver *)sat_calloc(1, sizeof(Solver));
  solv->pool = pool;
  solv->installed = installed;

  queue_init(&solv->ruletojob);
  queue_init(&solv->decisionq);
  queue_init(&solv->decisionq_why);
  queue_init(&solv->problems);
  queue_init(&solv->suggestions);
  queue_init(&solv->learnt_why);
  queue_init(&solv->learnt_pool);
  queue_init(&solv->branches);

  map_init(&solv->recommendsmap, pool->nsolvables);
  map_init(&solv->suggestsmap, pool->nsolvables);
  map_init(&solv->noupdate, installed ? installed->end - installed->start : 0);
  solv->recommends_index = 0;

  solv->decisionmap = (Id *)sat_calloc(pool->nsolvables, sizeof(Id));
  solv->rules = (Rule *)sat_malloc((solv->nrules + (RULES_BLOCK + 1)) * sizeof(Rule));
  memset(solv->rules, 0, sizeof(Rule));
  solv->nrules = 1;

  return solv;
}


/*
 * solver_free
 */

void
solver_free(Solver *solv)
{
  queue_free(&solv->ruletojob);
  queue_free(&solv->decisionq);
  queue_free(&solv->decisionq_why);
  queue_free(&solv->learnt_why);
  queue_free(&solv->learnt_pool);
  queue_free(&solv->problems);
  queue_free(&solv->suggestions);
  queue_free(&solv->branches);

  map_free(&solv->recommendsmap);
  map_free(&solv->suggestsmap);
  map_free(&solv->noupdate);

  sat_free(solv->decisionmap);
  sat_free(solv->rules);
  sat_free(solv->watches);
  sat_free(solv->weaksystemrules);
  sat_free(solv->obsoletes);
  sat_free(solv->obsoletes_data);
  sat_free(solv);
}


/*-------------------------------------------------------*/

/*
 * run_solver
 * 
 * all rules have been set up, now actually run the solver
 *
 */

static void
run_solver(Solver *solv, int disablerules, int doweak)
{
  Queue dq;
  int systemlevel;
  int level, olevel;
  Rule *r;
  int i, j, n;
  Solvable *s;
  Pool *pool = solv->pool;
  Id p, *dp;

  IF_POOLDEBUG (SAT_DEBUG_RULE_CREATION)
    {
      POOL_DEBUG (SAT_DEBUG_RULE_CREATION, "number of rules: %d\n", solv->nrules);
      for (i = 0; i < solv->nrules; i++)
	printrule(solv, SAT_DEBUG_RULE_CREATION, solv->rules + i);
    }

  POOL_DEBUG(SAT_DEBUG_STATS, "initial decisions: %d\n", solv->decisionq.count);
  
  IF_POOLDEBUG (SAT_DEBUG_SCHUBI)
    printdecisions(solv);

  /* start SAT algorithm */
  level = 1;
  systemlevel = level + 1;
  POOL_DEBUG(SAT_DEBUG_STATS, "solving...\n");

  queue_init(&dq);
  for (;;)
    {
      /*
       * propagate
       */
      
      if (level == 1)
	{
	  POOL_DEBUG(SAT_DEBUG_PROPAGATE, "propagating (propagate_index: %d;  size decisionq: %d)...\n", solv->propagate_index, solv->decisionq.count);
	  if ((r = propagate(solv, level)) != 0)
	    {
	      if (analyze_unsolvable(solv, r, disablerules))
		continue;
	      queue_free(&dq);
	      return;
	    }
	}

      /*
       * installed packages
       */
      
      if (level < systemlevel && solv->installed && solv->installed->nsolvables)
	{
	  if (!solv->updatesystem)
	    {
	      /* try to keep as many packages as possible */
	      POOL_DEBUG(SAT_DEBUG_STATS, "installing system packages\n");
	      for (i = solv->installed->start, n = 0; ; i++)
		{
		  if (n == solv->installed->nsolvables)
		    break;
		  if (i == solv->installed->end)
		    i = solv->installed->start;
		  s = pool->solvables + i;
		  if (s->repo != solv->installed)
		    continue;
		  n++;
		  if (solv->decisionmap[i] != 0)
		    continue;
		  POOL_DEBUG(SAT_DEBUG_PROPAGATE, "keeping %s\n", solvable2str(pool, s));
		  olevel = level;
		  level = setpropagatelearn(solv, level, i, disablerules);
		  if (level == 0)
		    {
		      queue_free(&dq);
		      return;
		    }
		  if (level <= olevel)
		    n = 0;
		}
	    }
	  if (solv->weaksystemrules)
	    {
	      POOL_DEBUG(SAT_DEBUG_STATS, "installing weak system packages\n");
	      for (i = solv->installed->start; i < solv->installed->end; i++)
		{
		  if (pool->solvables[i].repo != solv->installed)
		    continue;
		  if (solv->decisionmap[i] > 0 || (solv->decisionmap[i] < 0 && solv->weaksystemrules[i - solv->installed->start] == 0))
		    continue;
		  /* noupdate is set if a job is erasing the installed solvable or installing a specific version */
		  if (MAPTST(&solv->noupdate, i - solv->installed->start))
		    continue;
		  queue_empty(&dq);
		  if (solv->weaksystemrules[i - solv->installed->start])
		    {
		      dp = pool->whatprovidesdata + solv->weaksystemrules[i - solv->installed->start];
		      while ((p = *dp++) != 0)
			{
			  if (solv->decisionmap[p] > 0)
			    break;
			  if (solv->decisionmap[p] == 0)
			    queue_push(&dq, p);
			}
		      if (p)
			continue;	/* update package already installed */
		    }
		  if (!dq.count && solv->decisionmap[i] != 0)
		    continue;
		  olevel = level;
		  /* FIXME: i is handled a bit different because we do not want
		   * to have it pruned just because it is not recommened.
		   * we should not prune installed packages instead */
		  level = selectandinstall(solv, level, &dq, (solv->decisionmap[i] ? 0 : i), disablerules);
		  if (level == 0)
		    {
		      queue_free(&dq);
		      return;
		    }
		  if (level <= olevel)
		    break;
		}
	      if (i < solv->installed->end)
		continue;
	    }
	  systemlevel = level;
	}

      /*
       * decide
       */
      
      POOL_DEBUG(SAT_DEBUG_STATS, "deciding unresolved rules\n");
      for (i = 1, n = 1; ; i++, n++)
	{
	  if (n == solv->nrules)
	    break;
	  if (i == solv->nrules)
	    i = 1;
	  r = solv->rules + i;
	  if (!r->w1)	/* ignore disabled rules */
	    continue;
	  queue_empty(&dq);
	  if (r->d == 0)
	    {
	      /* binary or unary rule */
	      /* need two positive undecided literals */
	      if (r->p < 0 || r->w2 <= 0)
		continue;
	      if (solv->decisionmap[r->p] || solv->decisionmap[r->w2])
		continue;
	      queue_push(&dq, r->p);
	      queue_push(&dq, r->w2);
	    }
	  else
	    {
	      /* make sure that
               * all negative literals are installed
               * no positive literal is installed
	       * i.e. the rule is not fulfilled and we
               * just need to decide on the positive literals
               */
	      if (r->p < 0)
		{
		  if (solv->decisionmap[-r->p] <= 0)
		    continue;
		}
	      else
		{
		  if (solv->decisionmap[r->p] > 0)
		    continue;
		  if (solv->decisionmap[r->p] == 0)
		    queue_push(&dq, r->p);
		}
	      dp = pool->whatprovidesdata + r->d;
	      while ((p = *dp++) != 0)
		{
		  if (p < 0)
		    {
		      if (solv->decisionmap[-p] <= 0)
			break;
		    }
		  else
		    {
		      if (solv->decisionmap[p] > 0)
			break;
		      if (solv->decisionmap[p] == 0)
			queue_push(&dq, p);
		    }
		}
	      if (p)
		continue;
	    }
	  /* dq.count < 2 cannot happen as this means that
	   * the rule is unit */
	  assert(dq.count > 1);
	  IF_POOLDEBUG (SAT_DEBUG_PROPAGATE)
	    {
	      POOL_DEBUG(SAT_DEBUG_PROPAGATE, "unfulfilled ");
	      printrule(solv, SAT_DEBUG_PROPAGATE, r);
	    }

	  olevel = level;
	  level = selectandinstall(solv, level, &dq, 0, disablerules);
	  if (level == 0)
	    {
	      queue_free(&dq);
	      return;
	    }
	  if (level < systemlevel)
	    break;
	  n = 0;
	} /* for(), decide */

      if (n != solv->nrules)	/* continue if level < systemlevel */
	continue;
      
      if (doweak && !solv->problems.count)
	{
	  int qcount;

	  POOL_DEBUG(SAT_DEBUG_STATS, "installing recommended packages\n");
	  if (!REGARD_RECOMMENDS_OF_INSTALLED_ITEMS)
	    {
	      for (i = 0; i < solv->decisionq.count; i++)
		{
		  p = solv->decisionq.elements[i];
		  if (p > 0 && pool->solvables[p].repo == solv->installed)
		    solv->decisionmap[p] = -solv->decisionmap[p];
		}
	    }
	  queue_empty(&dq);
	  for (i = 1; i < pool->nsolvables; i++)
	    {
	      if (solv->decisionmap[i] < 0)
		continue;
	      if (solv->decisionmap[i] > 0)
		{
		  Id *recp, rec, *pp, p;
		  s = pool->solvables + i;
		  /* installed, check for recommends */
		  /* XXX need to special case AND ? */
		  if (s->recommends)
		    {
		      recp = s->repo->idarraydata + s->recommends;
		      while ((rec = *recp++) != 0)
			{
			  qcount = dq.count;
			  FOR_PROVIDES(p, pp, rec)
			    {
			      if (solv->decisionmap[p] > 0)
				{
				  dq.count = qcount;
				  break;
				}
			      else if (solv->decisionmap[p] == 0)
				{
				  queue_pushunique(&dq, p);
				}
			    }
			}
		    }
		}
	      else
		{
		  s = pool->solvables + i;
		  if (!s->supplements && !s->freshens)
		    continue;
		  if (!pool_installable(pool, s))
		    continue;
		  if (solver_is_supplementing(solv, s))
		    queue_pushunique(&dq, i);
		}
	    }
	  if (!REGARD_RECOMMENDS_OF_INSTALLED_ITEMS)
	    {
	      for (i = 0; i < solv->decisionq.count; i++)
		{
		  p = solv->decisionq.elements[i];
		  if (p > 0 && pool->solvables[p].repo == solv->installed)
		    solv->decisionmap[p] = -solv->decisionmap[p];
		}
	    }
	  if (dq.count)
	    {
	      if (dq.count > 1)
	        policy_filter_unwanted(solv, &dq, 0, POLICY_MODE_RECOMMEND);
	      p = dq.elements[0];
	      POOL_DEBUG(SAT_DEBUG_STATS, "installing recommended %s\n", solvable2str(pool, pool->solvables + p));
	      level = setpropagatelearn(solv, level, p, 0);
	      continue;
	    }
	}

     if (solv->solution_callback)
	{
	  solv->solution_callback(solv, solv->solution_callback_data);
	  if (solv->branches.count)
	    {
	      int i = solv->branches.count - 1;
	      int l = -solv->branches.elements[i];
	      for (; i > 0; i--)
		if (solv->branches.elements[i - 1] < 0)
		  break;
	      p = solv->branches.elements[i];
	      POOL_DEBUG(SAT_DEBUG_STATS, "branching with %s\n", solvable2str(pool, pool->solvables + p));
	      queue_empty(&dq);
	      for (j = i + 1; j < solv->branches.count; j++)
		queue_push(&dq, solv->branches.elements[j]);
	      solv->branches.count = i;
	      level = l;
	      revert(solv, level);
	      if (dq.count > 1)
	        for (j = 0; j < dq.count; j++)
		  queue_push(&solv->branches, dq.elements[j]);
	      olevel = level;
	      level = setpropagatelearn(solv, level, p, disablerules);
	      if (level == 0)
		{
		  queue_free(&dq);
		  return;
		}
	      continue;
	    }
	  /* all branches done, we're finally finished */
	  break;
	}

      /* minimization step */
     if (solv->branches.count)
	{
	  int l = 0, lasti = -1, lastl = -1;
	  p = 0;
	  for (i = solv->branches.count - 1; i >= 0; i--)
	    {
	      p = solv->branches.elements[i];
	      if (p < 0)
		l = -p;
	      else if (p > 0 && solv->decisionmap[p] > l + 1)
		{
		  lasti = i;
		  lastl = l;
		}
	    }
	  if (lasti >= 0)
	    {
	      /* kill old solvable so that we do not loop */
	      p = solv->branches.elements[lasti];
	      solv->branches.elements[lasti] = 0;
	      POOL_DEBUG(SAT_DEBUG_STATS, "minimizing %d -> %d with %s\n", solv->decisionmap[p], l, solvable2str(pool, pool->solvables + p));

	      level = lastl;
	      revert(solv, level);
	      olevel = level;
	      level = setpropagatelearn(solv, level, p, disablerules);
	      if (level == 0)
		{
		  queue_free(&dq);
		  return;
		}
	      continue;
	    }
	}
      break;
    }
  queue_free(&dq);
}

  
/*
 * refine_suggestion
 * at this point, all rules that led to conflicts are disabled.
 * we re-enable all rules of a problem set but rule "sug", then
 * continue to disable more rules until there as again a solution.
 */
  
/* FIXME: think about conflicting assertions */

static void
refine_suggestion(Solver *solv, Queue *job, Id *problem, Id sug, Queue *refined)
{
  Pool *pool = solv->pool;
  Rule *r;
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
	  printproblem(solv, problem[i]);
	}
    }
  queue_init(&disabled);
  queue_empty(refined);
  queue_push(refined, sug);

  /* re-enable all problem rules with the exception of "sug" */
  revert(solv, 1);
  reset_solver(solv);

  for (i = 0; problem[i]; i++)
    if (problem[i] != sug)
      enableproblem(solv, problem[i]);

  if (sug < 0)
    disableupdaterules(solv, job, -(sug + 1));

  for (;;)
    {
      /* re-enable as many weak rules as possible */
      for (i = solv->weakrules; i < solv->learntrules; i++)
	{
	  r = solv->rules + i;
	  if (!r->w1)
	    enablerule(solv, r);
	}

      queue_empty(&solv->problems);
      revert(solv, 1);		/* XXX move to reset_solver? */
      reset_solver(solv);

      if (!solv->problems.count)
        run_solver(solv, 0, 0);

      if (!solv->problems.count)
	{
	  POOL_DEBUG(SAT_DEBUG_SOLUTIONS, "no more problems!\n");
	  IF_POOLDEBUG (SAT_DEBUG_SCHUBI)
	    printdecisions(solv);
	  break;		/* great, no more problems */
	}
      disabledcnt = disabled.count;
      /* start with 1 to skip over proof index */
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
	  queue_push(&disabled, v);
	}
      if (disabled.count == disabledcnt)
	{
	  /* no solution found, this was an invalid suggestion! */
	  POOL_DEBUG(SAT_DEBUG_SOLUTIONS, "no solution found!\n");
	  refined->count = 0;
	  break;
	}
      if (disabled.count == disabledcnt + 1)
	{
	  /* just one suggestion, add it to refined list */
	  v = disabled.elements[disabledcnt];
	  queue_push(refined, v);
	  disableproblem(solv, v);
	  if (v < 0)
	    disableupdaterules(solv, job, -(v + 1));
	}
      else
	{
	  /* more than one solution, disable all */
	  /* do not push anything on refine list */
	  IF_POOLDEBUG (SAT_DEBUG_SOLUTIONS)
	    {
	      POOL_DEBUG(SAT_DEBUG_SOLUTIONS, "more than one solution found:\n");
	      for (i = disabledcnt; i < disabled.count; i++)
		printproblem(solv, disabled.elements[i]);
	    }
	  for (i = disabledcnt; i < disabled.count; i++)
	    disableproblem(solv, disabled.elements[i]);
	}
    }
  /* all done, get us back into the same state as before */
  /* enable refined rules again */
  for (i = 0; i < disabled.count; i++)
    enableproblem(solv, disabled.elements[i]);
  /* disable problem rules again */
  for (i = 0; problem[i]; i++)
    disableproblem(solv, problem[i]);
  disableupdaterules(solv, job, -1);
  POOL_DEBUG(SAT_DEBUG_SOLUTIONS, "refine_suggestion end\n");
}

static void
problems_to_solutions(Solver *solv, Queue *job)
{
  Pool *pool = solv->pool;
  Queue problems;
  Queue solution;
  Queue solutions;
  Id *problem;
  Id why;
  int i, j;

  if (!solv->problems.count)
    return;
  queue_clone(&problems, &solv->problems);
  queue_init(&solution);
  queue_init(&solutions);
  /* copy over proof index */
  queue_push(&solutions, problems.elements[0]);
  problem = problems.elements + 1;
  for (i = 1; i < problems.count; i++)
    {
      Id v = problems.elements[i];
      if (v == 0)
	{
	  /* mark end of this problem */
	  queue_push(&solutions, 0);
	  queue_push(&solutions, 0);
	  if (i + 1 == problems.count)
	    break;
	  /* copy over proof of next problem */
          queue_push(&solutions, problems.elements[i + 1]);
	  i++;
	  problem = problems.elements + i + 1;
	  continue;
	}
      refine_suggestion(solv, job, problem, v, &solution);
      if (!solution.count)
	continue;	/* this solution didn't work out */

      for (j = 0; j < solution.count; j++)
	{
	  why = solution.elements[j];
	  /* must be either job descriptor or system rule */
	  assert(why < 0 || (why >= solv->systemrules && why < solv->weakrules));
#if 0
	  printproblem(solv, why);
#endif
	  if (why < 0)
	    {
	      /* job descriptor */
	      queue_push(&solutions, 0);
	      queue_push(&solutions, -why);
	    }
	  else
	    {
	      /* system rule, find replacement package */
	      Id p, rp = 0;
	      p = solv->installed->start + (why - solv->systemrules);
	      if (solv->weaksystemrules && solv->weaksystemrules[why - solv->systemrules])
		{
		  Id *dp = pool->whatprovidesdata + solv->weaksystemrules[why - solv->systemrules];
		  for (; *dp; dp++)
		    {
		      if (*dp >= solv->installed->start && *dp < solv->installed->start + solv->installed->nsolvables)
			continue;
		      if (solv->decisionmap[*dp] > 0)
			{
			  rp = *dp;
			  break;
			}
		    }
		}
	      queue_push(&solutions, p);
	      queue_push(&solutions, rp);
	    }
	}
      /* mark end of this solution */
      queue_push(&solutions, 0);
      queue_push(&solutions, 0);
    }
  queue_free(&solution);
  queue_free(&problems);
  /* copy queue over to solutions */
  queue_free(&solv->problems);
  queue_clone(&solv->problems, &solutions);

  /* bring solver back into problem state */
  revert(solv, 1);		/* XXX move to reset_solver? */
  reset_solver(solv);

  assert(solv->problems.count == solutions.count);
  queue_free(&solutions);
}

Id
solver_next_problem(Solver *solv, Id problem)
{
  Id *pp;
  if (problem == 0)
    return solv->problems.count ? 1 : 0;
  pp = solv->problems.elements + problem;
  while (pp[0] || pp[1])
    {
      /* solution */
      pp += 2;
      while (pp[0] || pp[1])
        pp += 2;
      pp += 2;
    }
  pp += 2;
  problem = pp - solv->problems.elements;
  if (problem >= solv->problems.count)
    return 0;
  return problem + 1;
}

Id
solver_next_solution(Solver *solv, Id problem, Id solution)
{
  Id *pp;
  if (solution == 0)
    {
      solution = problem;
      pp = solv->problems.elements + solution;
      return pp[0] || pp[1] ? solution : 0;
    }
  pp = solv->problems.elements + solution;
  while (pp[0] || pp[1])
    pp += 2;
  pp += 2;
  solution = pp - solv->problems.elements;
  return pp[0] || pp[1] ? solution : 0;
}

Id
solver_next_solutionelement(Solver *solv, Id problem, Id solution, Id element, Id *p, Id *rp)
{
  Id *pp;
  element = element ? element + 2 : solution;
  pp = solv->problems.elements + element;
  if (!(pp[0] || pp[1]))
    return 0;
  *p = pp[0];
  *rp = pp[1];
  return element;
}


/*
 * create obsoletesmap from solver decisions
 * required for decision handling
 */

Id *
create_obsoletesmap(Solver *solv)
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

	  n = solv->decisionq.elements[i];
	  if (n < 0)
	    continue;
	  if (n == SYSTEMSOLVABLE)
	    continue;
	  s = pool->solvables + n;
	  if (s->repo == installed)		/* obsoletes don't count for already installed packages */
	    continue;
	  FOR_PROVIDES(p, pp, s->name)
	    if (s->name == pool->solvables[p].name)
	      {
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
	  obsp = s->repo->idarraydata + s->obsoletes;
	  while ((obs = *obsp++) != 0)
	    FOR_PROVIDES(p, pp, obs)
	      {
		if (pool->solvables[p].repo == installed && !obsoletesmap[p])
		  {
		    obsoletesmap[p] = n;
		    obsoletesmap[n]++;
		  }
	      }
	}
    }
  return obsoletesmap;
}

/*
 * printdecisions
 */
  

void
printdecisions(Solver *solv)
{
  Pool *pool = solv->pool;
  Repo *installed = solv->installed;
  Id p, *obsoletesmap = create_obsoletesmap( solv );
  int i;
  Solvable *s;

  POOL_DEBUG(SAT_DEBUG_SCHUBI, "----- Decisions -----\n");  

  /* print solvables to be erased */

  if (installed)
    {
      FOR_REPO_SOLVABLES(installed, p, s)
	{
	  if (solv->decisionmap[p] >= 0)
	    continue;
	  if (obsoletesmap[p])
	    continue;
	  POOL_DEBUG(SAT_DEBUG_RESULT, "erase   %s\n", solvable2str(pool, s));
	}
    }

  /* print solvables to be installed */

  for (i = 0; i < solv->decisionq.count; i++)
    {
      int j;
      p = solv->decisionq.elements[i];
      if (p < 0)
        {
	    IF_POOLDEBUG (SAT_DEBUG_SCHUBI)
	    {	
	      p = -p;
	      s = pool->solvables + p;	    
	      POOL_DEBUG(SAT_DEBUG_SCHUBI, "level of %s is %d\n", solvable2str(pool, s), p);
	    }
	    continue;
	}
      if (p == SYSTEMSOLVABLE)
        {
	    POOL_DEBUG(SAT_DEBUG_SCHUBI, "SYSTEMSOLVABLE\n");
	    continue;
	}
      s = pool->solvables + p;
      if (installed && s->repo == installed)
	continue;

      if (!obsoletesmap[p])
        {
          POOL_DEBUG(SAT_DEBUG_RESULT, "install %s", solvable2str(pool, s));
        }
      else
	{
	  POOL_DEBUG(SAT_DEBUG_RESULT, "update  %s", solvable2str(pool, s));
          POOL_DEBUG(SAT_DEBUG_RESULT, "  (obsoletes");
	  for (j = installed->start; j < installed->end; j++)
	    if (obsoletesmap[j] == p)
	      POOL_DEBUG(SAT_DEBUG_RESULT, " %s", solvable2str(pool, pool->solvables + j));
	  POOL_DEBUG(SAT_DEBUG_RESULT, ")");
	}
      POOL_DEBUG(SAT_DEBUG_RESULT, "\n");
    }

  sat_free(obsoletesmap);

  if (solv->suggestions.count)
    {
      POOL_DEBUG(SAT_DEBUG_RESULT, "\nsuggested packages:\n");
      for (i = 0; i < solv->suggestions.count; i++)
	{
	  s = pool->solvables + solv->suggestions.elements[i];
	  POOL_DEBUG(SAT_DEBUG_RESULT, "- %s\n", solvable2str(pool, s));
	}
    }
  POOL_DEBUG(SAT_DEBUG_SCHUBI, "----- Decisions end -----\n");    
}

/* this is basically the reverse of addrpmrulesforsolvable */
SolverProbleminfo
solver_problemruleinfo(Solver *solv, Queue *job, Id rid, Id *depp, Id *sourcep, Id *targetp)
{
  Pool *pool = solv->pool;
  Repo *installed = solv->installed;
  Rule *r;
  Solvable *s;
  int dontfix = 0;
  Id p, *pp, req, *reqp, con, *conp, obs, *obsp, *dp;
  
  assert(rid < solv->weakrules);
  if (rid >= solv->systemrules)
    {
      *depp = 0;
      *sourcep = solv->installed->start + (rid - solv->systemrules);
      *targetp = 0;
      return SOLVER_PROBLEM_UPDATE_RULE;
    }
  if (rid >= solv->jobrules)
    {
     
      r = solv->rules + rid;
      p = solv->ruletojob.elements[rid - solv->jobrules];
      *depp = job->elements[p + 1];
      *sourcep = p;
      *targetp = job->elements[p];
      if (r->d == 0 && r->w2 == 0 && r->p == -SYSTEMSOLVABLE)
	return SOLVER_PROBLEM_JOB_NOTHING_PROVIDES_DEP;
      return SOLVER_PROBLEM_JOB_RULE;
    }
  if (rid < 0)
    {
      /* a rpm rule assertion */
      assert(rid != -SYSTEMSOLVABLE);
      s = pool->solvables - rid;
      if (installed && !solv->fixsystem && s->repo == installed)
	dontfix = 1;
      assert(!dontfix);	/* dontfix packages never have a neg assertion */
      /* see why the package is not installable */
      if (s->arch != ARCH_SRC && s->arch != ARCH_NOSRC && !pool_installable(pool, s))
	return SOLVER_PROBLEM_NOT_INSTALLABLE;
      /* check requires */
      assert(s->requires);
      reqp = s->repo->idarraydata + s->requires;
      while ((req = *reqp++) != 0)
	{
	  if (req == SOLVABLE_PREREQMARKER)
	    continue;
	  dp = pool_whatprovides(pool, req);
	  if (*dp == 0)
	    break;
	}
      assert(req);
      *depp = req;
      *sourcep = -rid;
      *targetp = 0;
      return SOLVER_PROBLEM_NOTHING_PROVIDES_DEP;
    }
  r = solv->rules + rid;
  assert(r->p < 0);
  if (r->d == 0 && r->w2 == 0)
    {
      /* an assertion. we don't store them as rpm rules, so
       * can't happen */
      assert(0);
    }
  s = pool->solvables - r->p;
  if (installed && !solv->fixsystem && s->repo == installed)
    dontfix = 1;
  if (r->d == 0 && r->w2 < 0)
    {
      /* a package conflict */
      Solvable *s2 = pool->solvables - r->w2;
      int dontfix2 = 0;

      if (installed && !solv->fixsystem && s2->repo == installed)
	dontfix2 = 1;

      /* if both packages have the same name and at least one of them
       * is not installed, they conflict */
      if (s->name == s2->name && (!installed || (s->repo != installed || s2->repo != installed)))
	{
	  *depp = 0;
	  *sourcep = -r->p;
	  *targetp = -r->w2;
	  return SOLVER_PROBLEM_SAME_NAME;
	}

      /* check conflicts in both directions */
      if (s->conflicts)
	{
	  conp = s->repo->idarraydata + s->conflicts;
	  while ((con = *conp++) != 0)
            {
              FOR_PROVIDES(p, pp, con) 
		{
		  if (dontfix && pool->solvables[p].repo == installed)
		    continue;
		  if (p != -r->w2)
		    continue;
		  *depp = con;
		  *sourcep = -r->p;
		  *targetp = p;
		  return SOLVER_PROBLEM_PACKAGE_CONFLICT;
		}
	    }
	}
      if (s2->conflicts)
	{
	  conp = s2->repo->idarraydata + s2->conflicts;
	  while ((con = *conp++) != 0)
            {
              FOR_PROVIDES(p, pp, con) 
		{
		  if (dontfix2 && pool->solvables[p].repo == installed)
		    continue;
		  if (p != -r->p)
		    continue;
		  *depp = con;
		  *sourcep = -r->w2;
		  *targetp = p;
		  return SOLVER_PROBLEM_PACKAGE_CONFLICT;
		}
	    }
	}
      /* check obsoletes in both directions */
      if ((!installed || s->repo != installed) && s->obsoletes)
	{
	  obsp = s->repo->idarraydata + s->obsoletes;
	  while ((obs = *obsp++) != 0)
	    {
	      FOR_PROVIDES(p, pp, obs)
		{
		  if (p != -r->w2)
		    continue;
		  *depp = obs;
		  *sourcep = -r->p;
		  *targetp = p;
		  return SOLVER_PROBLEM_PACKAGE_OBSOLETES;
		}
	    }
	}
      if ((!installed || s2->repo != installed) && s2->obsoletes)
	{
	  obsp = s2->repo->idarraydata + s2->obsoletes;
	  while ((obs = *obsp++) != 0)
	    {
	      FOR_PROVIDES(p, pp, obs)
		{
		  if (p != -r->p)
		    continue;
		  *depp = obs;
		  *sourcep = -r->w2;
		  *targetp = p;
		  return SOLVER_PROBLEM_PACKAGE_OBSOLETES;
		}
	    }
	}
      /* all cases checked, can't happen */
      assert(0);
    }
  /* simple requires */
  assert(s->requires);
  reqp = s->repo->idarraydata + s->requires;
  while ((req = *reqp++) != 0)
    {
      if (req == SOLVABLE_PREREQMARKER)
	continue;
      dp = pool_whatprovides(pool, req);
      if (r->d == 0)
	{
	  if (*dp == r->w2 && dp[1] == 0)
	    break;
	}
      else if (dp - pool->whatprovidesdata == r->d)
	break;
    }
  assert(req);
  *depp = req;
  *sourcep = -r->p;
  *targetp = 0;
  return SOLVER_PROBLEM_DEP_PROVIDERS_NOT_INSTALLABLE;
}

static void
findproblemrule_internal(Solver *solv, Id idx, Id *reqrp, Id *conrp, Id *sysrp, Id *jobrp)
{
  Id rid;
  Id lreqr, lconr, lsysr, ljobr;
  Rule *r;
  int reqassert = 0;

  lreqr = lconr = lsysr = ljobr = 0;
  while ((rid = solv->learnt_pool.elements[idx++]) != 0)
    {
      if (rid >= solv->learntrules)
	findproblemrule_internal(solv, solv->learnt_why.elements[rid - solv->learntrules], &lreqr, &lconr, &lsysr, &ljobr);
      else if (rid >= solv->systemrules)
	{
	  if (!*sysrp)
	    *sysrp = rid;
	}
      else if (rid >= solv->jobrules)
	{
	  if (!*jobrp)
	    *jobrp = rid;
	}
      else if (rid >= 0)
	{
	  r = solv->rules + rid;
	  if (!r->d && r->w2 < 0)
	    {
	      if (!*conrp)
		*conrp = rid;
	    }
	  else
	    {
	      if (!*reqrp)
		*reqrp = rid;
	    }
	}
      else
	{
	  /* assertion, counts as require rule */
	  /* ignore system solvable as we need useful info */
	  if (rid == -SYSTEMSOLVABLE)
	    continue;
	  if (!*reqrp || !reqassert)
	    {
	      *reqrp = rid;
	      reqassert = 1;
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

Id
solver_findproblemrule(Solver *solv, Id problem)
{
  Id reqr, conr, sysr, jobr;
  Id idx = solv->problems.elements[problem - 1];
  reqr = conr = sysr = jobr = 0;
  findproblemrule_internal(solv, idx, &reqr, &conr, &sysr, &jobr);
  if (reqr)
    return reqr;
  if (conr)
    return conr;
  if (sysr)
    return sysr;
  if (jobr)
    return jobr;
  assert(0);
}

void
printprobleminfo(Solver *solv, Queue *job, Id problem)
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
    }
}

void
printsolutions(Solver *solv, Queue *job)
{
  Pool *pool = solv->pool;
  int pcnt;
  Id p, rp, what;
  Id problem, solution, element;
  Solvable *s, *sd;

  POOL_DEBUG(SAT_DEBUG_RESULT, "Encountered problems! Here are the solutions:\n\n");
  pcnt = 1;
  problem = 0;
  while ((problem = solver_next_problem(solv, problem)) != 0)
    {
      POOL_DEBUG(SAT_DEBUG_RESULT, "Problem %d:\n", pcnt++);
      POOL_DEBUG(SAT_DEBUG_RESULT, "====================================\n");
      printprobleminfo(solv, job, problem);
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
		  what = job->elements[rp];
		  switch (job->elements[rp - 1])
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
		      POOL_DEBUG(SAT_DEBUG_RESULT, "- do not install %s\n", id2str(pool, what));
		      break;
		    case SOLVER_ERASE_SOLVABLE_NAME:
		      POOL_DEBUG(SAT_DEBUG_RESULT, "- do not deinstall %s\n", id2str(pool, what));
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
		      if (!solv->allowarchchange && s->name == sd->name && s->arch != sd->arch && policy_illegal_archchange(pool, s, sd))
			{
			  POOL_DEBUG(SAT_DEBUG_RESULT, "- allow architecture change of %s to %s\n", solvable2str(pool, s), solvable2str(pool, sd));
			  gotone = 1;
			}
		      if (!solv->allowvendorchange && s->name == sd->name && s->vendor != sd->vendor && policy_illegal_vendorchange(pool, s, sd))
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


/* for each installed solvable find which packages with *different* names
 * obsolete the solvable.
 * this index is used in policy_findupdatepackages if noupdateprovide is set.
 */

static void
create_obsolete_index(Solver *solv)
{
  Pool *pool = solv->pool;
  Solvable *s;
  Repo *installed = solv->installed;
  Id p, *pp, obs, *obsp, *obsoletes, *obsoletes_data;
  int i, n;

  if (!installed || !installed->nsolvables)
    return;
  /* create reverse obsoletes map for installed solvables */
  solv->obsoletes = obsoletes = sat_calloc(installed->end - installed->start, sizeof(Id));
  for (i = 1; i < pool->nsolvables; i++)
    {
      s = pool->solvables + i;
      if (s->repo == installed)
	continue;
      if (!s->obsoletes)
	continue;
      if (!pool_installable(pool, s))
	continue;
      obsp = s->repo->idarraydata + s->obsoletes;
      while ((obs = *obsp++) != 0)
        FOR_PROVIDES(p, pp, obs)
	  {
	    if (pool->solvables[p].repo != installed)
	      continue;
	    if (pool->solvables[p].name == s->name)
	      continue;
	    obsoletes[p - installed->start]++;
	  }
    }
  n = 0;
  for (i = 0; i < installed->nsolvables; i++)
    if (obsoletes[i])
      {
        n += obsoletes[i] + 1;
        obsoletes[i] = n;
      }
  solv->obsoletes_data = obsoletes_data = sat_calloc(n + 1, sizeof(Id));
  POOL_DEBUG(SAT_DEBUG_STATS, "obsoletes data: %d entries\n", n + 1);
  for (i = pool->nsolvables - 1; i > 0; i--)
    {
      s = pool->solvables + i;
      if (s->repo == installed)
	continue;
      if (!s->obsoletes)
	continue;
      if (!pool_installable(pool, s))
	continue;
      obsp = s->repo->idarraydata + s->obsoletes;
      while ((obs = *obsp++) != 0)
        FOR_PROVIDES(p, pp, obs)
	  {
	    if (pool->solvables[p].repo != installed)
	      continue;
	    if (pool->solvables[p].name == s->name)
	      continue;
	    p -= installed->start;
	    if (obsoletes_data[obsoletes[p]] != i)
	      obsoletes_data[--obsoletes[p]] = i;
	  }
    }
}


/*-----------------------------------------------------------------*/
/* main() */

/*
 *
 * solve job queue
 *
 */

void
solver_solve(Solver *solv, Queue *job)
{
  Pool *pool = solv->pool;
  Repo *installed = solv->installed;
  int i;
  int oldnrules;
  Map addedmap;		       /* '1' == have rpm-rules for solvable */
  Id how, what, p, *pp, d;
  Queue q;
  Solvable *s;

  /* create whatprovides if not already there */
  if (!pool->whatprovides)
    pool_createwhatprovides(pool);

  /* create obsolete index if needed */
  if (solv->noupdateprovide)
    create_obsolete_index(solv);

  /*
   * create basic rule set of all involved packages
   * use addedmap bitmap to make sure we don't create rules twice
   * 
   */

  map_init(&addedmap, pool->nsolvables);
  queue_init(&q);
  
  /*
   * always install our system solvable
   */
  MAPSET(&addedmap, SYSTEMSOLVABLE);
  queue_push(&solv->decisionq, SYSTEMSOLVABLE);
  queue_push(&solv->decisionq_why, 0);
  solv->decisionmap[SYSTEMSOLVABLE] = 1;

  /*
   * create rules for all package that could be involved with the solving
   * so called: rpm rules
   * 
   */
  if (installed)
    {
      oldnrules = solv->nrules;
      POOL_DEBUG(SAT_DEBUG_SCHUBI, "*** create rpm rules for installed solvables ***\n");
      FOR_REPO_SOLVABLES(installed, p, s)
	addrpmrulesforsolvable(solv, s, &addedmap);
      POOL_DEBUG(SAT_DEBUG_STATS, "added %d rpm rules for installed solvables\n", solv->nrules - oldnrules);
      POOL_DEBUG(SAT_DEBUG_SCHUBI, "*** create rpm rules for updaters of installed solvables ***\n");
      oldnrules = solv->nrules;
      FOR_REPO_SOLVABLES(installed, p, s)
	addrpmrulesforupdaters(solv, s, &addedmap, 1);
      POOL_DEBUG(SAT_DEBUG_STATS, "added %d rpm rules for updaters of installed solvables\n", solv->nrules - oldnrules);
    }

  POOL_DEBUG(SAT_DEBUG_SCHUBI, "*** create rpm rules for packages involved with a job ***\n");
  oldnrules = solv->nrules;
  for (i = 0; i < job->count; i += 2)
    {
      how = job->elements[i];
      what = job->elements[i + 1];

      switch(how)
	{
	case SOLVER_INSTALL_SOLVABLE:
	  addrpmrulesforsolvable(solv, pool->solvables + what, &addedmap);
	  break;
	case SOLVER_INSTALL_SOLVABLE_NAME:
	case SOLVER_INSTALL_SOLVABLE_PROVIDES:
	  FOR_PROVIDES(p, pp, what)
	    {
	      /* if by name, ensure that the name matches */
	      if (how == SOLVER_INSTALL_SOLVABLE_NAME && pool->solvables[p].name != what)
		continue;
	      addrpmrulesforsolvable(solv, pool->solvables + p, &addedmap);
	    }
	  break;
	case SOLVER_INSTALL_SOLVABLE_UPDATE:
	  /* dont allow downgrade */
	  addrpmrulesforupdaters(solv, pool->solvables + what, &addedmap, 0);
	  break;
	}
    }
  POOL_DEBUG(SAT_DEBUG_STATS, "added %d rpm rules for packages involved in a job\n", solv->nrules - oldnrules);

  POOL_DEBUG(SAT_DEBUG_SCHUBI, "*** create rpm rules for recommended/suggested packages ***\n");

  oldnrules = solv->nrules;
  addrpmrulesforweak(solv, &addedmap);
  POOL_DEBUG(SAT_DEBUG_STATS, "added %d rpm rules because of weak dependencies\n", solv->nrules - oldnrules);

  IF_POOLDEBUG (SAT_DEBUG_STATS)
    {
      int possible = 0, installable = 0;
      for (i = 1; i < pool->nsolvables; i++)
	{
	  if (pool_installable(pool, pool->solvables + i))
	    installable++;
	  if (MAPTST(&addedmap, i))
	    possible++;
	}
      POOL_DEBUG(SAT_DEBUG_STATS, "%d of %d installable solvables considered for solving (rules has been generated for)\n", possible, installable);
    }

  /*
   * first pass done, we now have all the rpm rules we need.
   * unify existing rules before going over all job rules and
   * policy rules.
   * at this point the system is always solvable,
   * as an empty system (remove all packages) is a valid solution
   */
  
  unifyrules(solv);	/* remove duplicate rpm rules */
  solv->directdecisions = solv->decisionq.count;

  POOL_DEBUG(SAT_DEBUG_STATS, "decisions so far: %d\n", solv->decisionq.count);
  IF_POOLDEBUG (SAT_DEBUG_SCHUBI) 
    printdecisions (solv);

  /*
   * now add all job rules
   */

  POOL_DEBUG(SAT_DEBUG_SCHUBI, "*** Add JOB rules ***\n");  
  
  solv->jobrules = solv->nrules;

  for (i = 0; i < job->count; i += 2)
    {
      oldnrules = solv->nrules;

      how = job->elements[i];
      what = job->elements[i + 1];
      switch(how)
	{
	case SOLVER_INSTALL_SOLVABLE:			/* install specific solvable */
	  s = pool->solvables + what;
	  POOL_DEBUG(SAT_DEBUG_JOB, "job: install solvable %s\n", solvable2str(pool, s));
          addrule(solv, what, 0);			/* install by Id */
	  queue_push(&solv->ruletojob, i);
	  break;
	case SOLVER_ERASE_SOLVABLE:
	  s = pool->solvables + what;
	  POOL_DEBUG(SAT_DEBUG_JOB, "job: erase solvable %s\n", solvable2str(pool, s));
          addrule(solv, -what, 0);			/* remove by Id */
	  queue_push(&solv->ruletojob, i);
	  break;
	case SOLVER_INSTALL_SOLVABLE_NAME:		/* install by capability */
	case SOLVER_INSTALL_SOLVABLE_PROVIDES:
	  if (how == SOLVER_INSTALL_SOLVABLE_NAME)
	    POOL_DEBUG(SAT_DEBUG_JOB, "job: install name %s\n", id2str(pool, what));
	  if (how == SOLVER_INSTALL_SOLVABLE_PROVIDES)
	    POOL_DEBUG(SAT_DEBUG_JOB, "job: install provides %s\n", dep2str(pool, what));
	  queue_empty(&q);
	  FOR_PROVIDES(p, pp, what)
	    {
              /* if by name, ensure that the name matches */
	      if (how == SOLVER_INSTALL_SOLVABLE_NAME && pool->solvables[p].name != what)
		continue;
	      queue_push(&q, p);
	    }
	  if (!q.count)	
	    {
	      /* no provider, make this an impossible rule */
	      queue_push(&q, -SYSTEMSOLVABLE);
	    }

	  p = queue_shift(&q);	       /* get first provider */
	  if (!q.count)
	    d = 0;		       /* single provider ? -> make assertion */
	  else
	    d = pool_queuetowhatprovides(pool, &q);   /* get all providers */
	  addrule(solv, p, d);	       /* add 'requires' rule */
	  queue_push(&solv->ruletojob, i);
	  break;
	case SOLVER_ERASE_SOLVABLE_NAME:                  /* remove by capability */
	case SOLVER_ERASE_SOLVABLE_PROVIDES:
	  if (how == SOLVER_ERASE_SOLVABLE_NAME)
	    POOL_DEBUG(SAT_DEBUG_JOB, "job: erase name %s\n", id2str(pool, what));
	  if (how == SOLVER_ERASE_SOLVABLE_PROVIDES)
	    POOL_DEBUG(SAT_DEBUG_JOB, "job: erase provides %s\n", dep2str(pool, what));
	  FOR_PROVIDES(p, pp, what)
	    {
	      /* if by name, ensure that the name matches */
	      if (how == SOLVER_ERASE_SOLVABLE_NAME && pool->solvables[p].name != what)
	        continue;
	      addrule(solv, -p, 0);  /* add 'remove' rule */
	      queue_push(&solv->ruletojob, i);
	    }
	  break;
	case SOLVER_INSTALL_SOLVABLE_UPDATE:              /* find update for solvable */
	  s = pool->solvables + what;
	  POOL_DEBUG(SAT_DEBUG_JOB, "job: update %s\n", solvable2str(pool, s));
	  addupdaterule(solv, s, 0);
	  queue_push(&solv->ruletojob, i);
	  break;
	}
      IF_POOLDEBUG (SAT_DEBUG_JOB)
	{
	  int j;
	  if (solv->nrules == oldnrules)
	    POOL_DEBUG(SAT_DEBUG_JOB, " - no rule created");
	  for (j = oldnrules; j < solv->nrules; j++)
	    {
	      POOL_DEBUG(SAT_DEBUG_JOB, " - job ");
	      printrule(solv, SAT_DEBUG_JOB, solv->rules + j);
	    }
	}
    }
  assert(solv->ruletojob.count == solv->nrules - solv->jobrules);

  /*
   * now add system rules
   * 
   */

  POOL_DEBUG(SAT_DEBUG_SCHUBI, "*** Add system rules ***\n");
  
  
  solv->systemrules = solv->nrules;

  /*
   * create rules for updating installed solvables
   * 
   */
  
  if (installed && !solv->allowuninstall)
    {				       /* loop over all installed solvables */
      /* we create all update rules, but disable some later on depending on the job */
      for (i = installed->start, s = pool->solvables + i; i < installed->end; i++, s++)
	if (s->repo == installed)
	  addupdaterule(solv, s, 0); /* allowall = 0 */
	else
	  addupdaterule(solv, 0, 0);	/* create dummy rule;  allowall = 0  */
      /* consistency check: we added a rule for _every_ system solvable */
      assert(solv->nrules - solv->systemrules == installed->end - installed->start);
    }

  /* create special weak system rules */
  /* those are used later on to keep a version of the installed packages in
     best effort mode */
  if (installed && installed->nsolvables)
    {
      solv->weaksystemrules = sat_calloc(installed->end - installed->start, sizeof(Id));
      FOR_REPO_SOLVABLES(installed, p, s)
	{
	  policy_findupdatepackages(solv, s, &q, 1);
	  if (q.count)
	    solv->weaksystemrules[p - installed->start] = pool_queuetowhatprovides(pool, &q);
	}
    }

  /* free unneeded memory */
  map_free(&addedmap);
  queue_free(&q);

  solv->weakrules = solv->nrules;

  /* try real hard to keep packages installed */
  if (0)
    {
      FOR_REPO_SOLVABLES(installed, p, s)
        {
	  /* FIXME: can't work with refine_suggestion! */
	  /* need to always add the rule but disable it */
	  if (MAPTST(&solv->noupdate, p - installed->start))
 	    continue;
	  d = solv->weaksystemrules[p - installed->start];
	  addrule(solv, p, d);
	}
    }

  /* all new rules are learnt after this point */
  solv->learntrules = solv->nrules;

  /* disable system rules that conflict with our job */
  disableupdaterules(solv, job, -1);

  /* make decisions based on job/system assertions */
  makeruledecisions(solv);

  /* create watches chains */
  makewatches(solv);

  POOL_DEBUG(SAT_DEBUG_STATS, "problems so far: %d\n", solv->problems.count);

  /* solve! */
  run_solver(solv, 1, 1);

  /* find suggested packages */
  if (!solv->problems.count)
    {
      Id sug, *sugp, p, *pp;

      /* create map of all suggests that are still open */
      solv->recommends_index = -1;
      MAPZERO(&solv->suggestsmap);
      for (i = 0; i < solv->decisionq.count; i++)
	{
	  p = solv->decisionq.elements[i];
	  if (p < 0)
	    continue;
	  s = pool->solvables + p;
	  if (s->suggests)
	    {
	      sugp = s->repo->idarraydata + s->suggests;
	      while ((sug = *sugp++) != 0)
		{
		  FOR_PROVIDES(p, pp, sug)
		    if (solv->decisionmap[p] > 0)
		      break;
		  if (p)
		    continue;	/* already fulfilled */
		  FOR_PROVIDES(p, pp, sug)
		    MAPSET(&solv->suggestsmap, p);
		}
	    }
	}
      for (i = 1; i < pool->nsolvables; i++)
	{
	  if (solv->decisionmap[i] != 0)
	    continue;
	  s = pool->solvables + i;
	  if (!MAPTST(&solv->suggestsmap, i))
	    {
	      if (!s->enhances)
		continue;
	      if (!pool_installable(pool, s))
		continue;
	      if (!solver_is_enhancing(solv, s))
		continue;
	    }
	  queue_push(&solv->suggestions, i);
	}
      policy_filter_unwanted(solv, &solv->suggestions, 0, POLICY_MODE_SUGGEST);
    }

  if (solv->problems.count)
    problems_to_solutions(solv, job);
}

