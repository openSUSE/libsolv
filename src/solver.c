/*
 * Copyright (c) 2007-2008, Novell Inc.
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
#include "solverdebug.h"

#define RULES_BLOCK 63

static void reenablepolicyrules(Solver *solv, Queue *job, int jobidx);
static void addrpmruleinfo(Solver *solv, Id p, Id d, int type, Id dep);

/********************************************************************
 *
 * dependency check helpers
 *
 */

/*-------------------------------------------------------------------
 * handle split provides
 */

int
solver_splitprovides(Solver *solv, Id dep)
{
  Pool *pool = solv->pool;
  Id p, pp;
  Reldep *rd;
  Solvable *s;

  if (!solv->dosplitprovides || !solv->installed)
    return 0;
  if (!ISRELDEP(dep))
    return 0;
  rd = GETRELDEP(pool, dep);
  if (rd->flags != REL_WITH)
    return 0;
  FOR_PROVIDES(p, pp, dep)
    {
      s = pool->solvables + p;
      if (s->repo == solv->installed && s->name == rd->name)
	return 1;
    }
  return 0;
}


/*-------------------------------------------------------------------
 * solver_dep_installed
 */

int
solver_dep_installed(Solver *solv, Id dep)
{
#if 0
  Pool *pool = solv->pool;
  Id p, pp;

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


/*-------------------------------------------------------------------
 * Check if dependenc is possible
 * 
 * this mirrors solver_dep_fulfilled
 * but uses map m instead of the decisionmap
 */

static inline int
dep_possible(Solver *solv, Id dep, Map *m)
{
  Pool *pool = solv->pool;
  Id p, pp;

  if (ISRELDEP(dep))
    {
      Reldep *rd = GETRELDEP(pool, dep);
      if (rd->flags == REL_AND)
	{
	  if (!dep_possible(solv, rd->name, m))
	    return 0;
	  return dep_possible(solv, rd->evr, m);
	}
      if (rd->flags == REL_NAMESPACE && rd->name == NAMESPACE_SPLITPROVIDES)
	return solver_splitprovides(solv, rd->evr);
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

/********************************************************************
 *
 * Rule handling
 *
 * - unify rules, remove duplicates
 */

/*-------------------------------------------------------------------
 *
 * compare rules for unification sort
 *
 */

static int
unifyrules_sortcmp(const void *ap, const void *bp, void *dp)
{
  Pool *pool = dp;
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


/*-------------------------------------------------------------------
 *
 * unify rules
 * go over all rules and remove duplicates
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
  sat_sort(solv->rules + 1, solv->nrules - 1, sizeof(Rule), unifyrules_sortcmp, solv->pool);

  /* prune rules
   * i = unpruned
   * j = pruned
   */
  jr = 0;
  for (i = j = 1, ir = solv->rules + i; i < solv->nrules; i++, ir++)
    {
      if (jr && !unifyrules_sortcmp(ir, jr, pool))
	continue;		       /* prune! */
      jr = solv->rules + j++;	       /* keep! */
      if (ir != jr)
        *jr = *ir;
    }

  /* reduced count from nrules to j rules */
  POOL_DEBUG(SAT_DEBUG_STATS, "pruned rules from %d to %d\n", solv->nrules, j);

  /* adapt rule buffer */
  solv->nrules = j;
  solv->rules = sat_extend_resize(solv->rules, solv->nrules, sizeof(Rule), RULES_BLOCK);
    /*
     * debug: statistics
     */
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


/*-------------------------------------------------------------------
 * 
 */

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
 *   Remove:     p < 0, d = 0   (-A)            user requested remove (also: uninstallable)
 *   Requires:   p < 0, d > 0   (-A|B1|B2|...)  d: <list of providers for requirement of p>
 *   Updates:    p > 0, d > 0   (A|B1|B2|...)   d: <list of updates for solvable p>
 *   Conflicts:  p < 0, d < 0   (-A|-B)         either p (conflict issuer) or d (conflict provider) (binary rule)
 *                                              also used for obsoletes
 *   ?:          p > 0, d < 0   (A|-B)          
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
					  >1 = 
					*/

  /* it often happenes that requires lead to adding the same rpm rule
   * multiple times, so we prune those duplicates right away to make
   * the work for unifyrules a bit easier */

  if (solv->nrules                      /* we already have rules */
      && !solv->rpmrules_end)           /* but are not done with rpm rules */
    {
      r = solv->rules + solv->nrules - 1;   /* get the last added rule */
      if (r->p == p && r->d == d && d != 0)   /* identical and not user requested */
	return r;
    }

    /*
     * compute number of literals (n) in rule
     */
    
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
	
      if (n == 1)   /* have single provider */
	d = dp[-1];                     /* take single literal */
    }

  if (n == 1 && p > d && !solv->rpmrules_end)
    {
      /* smallest literal first so we can find dups */
      n = p; p = d; d = n;             /* p <-> d */
      n = 1;			       /* re-set n, was used as temp var */
    }

  /*
   * check for duplicate
   */
    
  /* check if the last added rule (r) is exactly the same as what we're looking for. */
  if (r && n == 1 && !r->d && r->p == p && r->w2 == d)
    return r;  /* binary rule */

    /* have n-ary rule with same first literal, check other literals */
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

  /* extend rule buffer */
  solv->rules = sat_extend(solv->rules, solv->nrules, 1, sizeof(Rule), RULES_BLOCK);
  r = solv->rules + solv->nrules++;    /* point to rule space */

    /*
     * r = new rule
     */
    
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
      solver_printrule(solv, SAT_DEBUG_RULE_CREATION, r);
    }

  return r;
}

/*-------------------------------------------------------------------
 * disable rule
 */

static inline void
disablerule(Solver *solv, Rule *r)
{
  if (r->d >= 0)
    r->d = -r->d - 1;
}

/*-------------------------------------------------------------------
 * enable rule
 */

static inline void
enablerule(Solver *solv, Rule *r)
{
  if (r->d < 0)
    r->d = -r->d - 1;
}


/**********************************************************************************/

/* a problem is an item on the solver's problem list. It can either be >0, in that
 * case it is a update rule, or it can be <0, which makes it refer to a job
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
      if (v >= solv->infarchrules && v < solv->infarchrules_end)
	{
	  Pool *pool = solv->pool;
	  Id name = pool->solvables[-solv->rules[v].p].name;
	  while (v > solv->infarchrules && pool->solvables[-solv->rules[v - 1].p].name == name)
	    v--;
	  for (; v < solv->infarchrules_end && pool->solvables[-solv->rules[v].p].name == name; v++)
	    disablerule(solv, solv->rules + v);
	  return;
	}
      if (v >= solv->duprules && v < solv->duprules_end)
	{
	  Pool *pool = solv->pool;
	  Id name = pool->solvables[-solv->rules[v].p].name;
	  while (v > solv->duprules && pool->solvables[-solv->rules[v - 1].p].name == name)
	    v--;
	  for (; v < solv->duprules_end && pool->solvables[-solv->rules[v].p].name == name; v++)
	    disablerule(solv, solv->rules + v);
	  return;
	}
      disablerule(solv, solv->rules + v);
      return;
    }
  v = -(v + 1);
  jp = solv->ruletojob.elements;
  for (i = solv->jobrules, r = solv->rules + i; i < solv->jobrules_end; i++, r++, jp++)
    if (*jp == v)
      disablerule(solv, r);
}

/*-------------------------------------------------------------------
 * enableproblem
 */

static void
enableproblem(Solver *solv, Id v)
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
	    enablerule(solv, solv->rules + v);
	  return;
	}
      if (v >= solv->duprules && v < solv->duprules_end)
	{
	  Pool *pool = solv->pool;
	  Id name = pool->solvables[-solv->rules[v].p].name;
	  while (v > solv->duprules && pool->solvables[-solv->rules[v - 1].p].name == name)
	    v--;
	  for (; v < solv->duprules_end && pool->solvables[-solv->rules[v].p].name == name; v++)
	    enablerule(solv, solv->rules + v);
	  return;
	}
      if (v >= solv->featurerules && v < solv->featurerules_end)
	{
	  /* do not enable feature rule if update rule is enabled */
	  r = solv->rules + (v - solv->featurerules + solv->updaterules);
	  if (r->d >= 0)
	    return;
	}
      enablerule(solv, solv->rules + v);
      if (v >= solv->updaterules && v < solv->updaterules_end)
	{
	  /* disable feature rule when enabling update rule */
	  r = solv->rules + (v - solv->updaterules + solv->featurerules);
	  if (r->p)
	    disablerule(solv, r);
	}
      return;
    }
  v = -(v + 1);
  jp = solv->ruletojob.elements;
  for (i = solv->jobrules, r = solv->rules + i; i < solv->jobrules_end; i++, r++, jp++)
    if (*jp == v)
      enablerule(solv, r);
}


/************************************************************************/

/*
 * make assertion rules into decisions
 * 
 * Go through rules and add direct assertions to the decisionqueue.
 * If we find a conflict, disable rules and add them to problem queue.
 */

static void
makeruledecisions(Solver *solv)
{
  Pool *pool = solv->pool;
  int i, ri, ii;
  Rule *r, *rr;
  Id v, vv;
  int decisionstart;

  POOL_DEBUG(SAT_DEBUG_SCHUBI, "----- makeruledecisions ; size decisionq: %d -----\n",solv->decisionq.count);

  /* The system solvable is always installed first */
  assert(solv->decisionq.count == 0);
  queue_push(&solv->decisionq, SYSTEMSOLVABLE);
  queue_push(&solv->decisionq_why, 0);
  solv->decisionmap[SYSTEMSOLVABLE] = 1;	/* installed at level '1' */

  decisionstart = solv->decisionq.count;
  for (ii = 0; ii < solv->ruleassertions.count; ii++)
    {
      ri = solv->ruleassertions.elements[ii];
      r = solv->rules + ri;
	
      if (r->d < 0 || !r->p || r->w2)	/* disabled, dummy or no assertion */
	continue;
      /* do weak rules in phase 2 */
      if (ri < solv->learntrules && MAPTST(&solv->weakrulemap, ri))
	continue;
	
      v = r->p;
      vv = v > 0 ? v : -v;
	
      if (!solv->decisionmap[vv])          /* if not yet decided */
	{
	    /*
	     * decide !
	     */
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
	/*
	 * check previous decision: is it sane ?
	 */
	
      if (v > 0 && solv->decisionmap[vv] > 0)    /* ok to install */
	continue;
      if (v < 0 && solv->decisionmap[vv] < 0)    /* ok to remove */
	continue;
	
        /*
	 * found a conflict!
	 * 
	 * The rule (r) we're currently processing says something
	 * different (v = r->p) than a previous decision (decisionmap[abs(v)])
	 * on this literal
	 */
	
      if (ri >= solv->learntrules)
	{
	  /* conflict with a learnt rule */
	  /* can happen when packages cannot be installed for
           * multiple reasons. */
          /* we disable the learnt rule in this case */
	  disablerule(solv, r);
	  continue;
	}
	
        /*
	 * find the decision which is the "opposite" of the rule
	 */
	
      for (i = 0; i < solv->decisionq.count; i++)
	if (solv->decisionq.elements[i] == -v)
	  break;
      assert(i < solv->decisionq.count);         /* assert that we found it */
	
      /*
       * conflict with system solvable ?
       */
	
      if (v == -SYSTEMSOLVABLE) {
	/* conflict with system solvable */
	queue_push(&solv->problems, solv->learnt_pool.count);
        queue_push(&solv->learnt_pool, ri);
	queue_push(&solv->learnt_pool, 0);
	POOL_DEBUG(SAT_DEBUG_UNSOLVABLE, "conflict with system solvable, disabling rule #%d\n", ri);
	if  (ri >= solv->jobrules && ri < solv->jobrules_end)
	  v = -(solv->ruletojob.elements[ri - solv->jobrules] + 1);
	else
	  v = ri;
	queue_push(&solv->problems, v);
	queue_push(&solv->problems, 0);
	disableproblem(solv, v);
	continue;
      }

      assert(solv->decisionq_why.elements[i] > 0);
	
      /*
       * conflict with an rpm rule ?
       */
	
      if (solv->decisionq_why.elements[i] < solv->rpmrules_end)
	{
	  /* conflict with rpm rule assertion */
	  queue_push(&solv->problems, solv->learnt_pool.count);
	  queue_push(&solv->learnt_pool, ri);
	  queue_push(&solv->learnt_pool, solv->decisionq_why.elements[i]);
	  queue_push(&solv->learnt_pool, 0);
	  assert(v > 0 || v == -SYSTEMSOLVABLE);
	  POOL_DEBUG(SAT_DEBUG_UNSOLVABLE, "conflict with rpm rule, disabling rule #%d\n", ri);
	  if (ri >= solv->jobrules && ri < solv->jobrules_end)
	    v = -(solv->ruletojob.elements[ri - solv->jobrules] + 1);
	  else
	    v = ri;
	  queue_push(&solv->problems, v);
	  queue_push(&solv->problems, 0);
	  disableproblem(solv, v);
	  continue;
	}

      /*
       * conflict with another job or update/feature rule
       */
	
      /* record proof */
      queue_push(&solv->problems, solv->learnt_pool.count);
      queue_push(&solv->learnt_pool, ri);
      queue_push(&solv->learnt_pool, solv->decisionq_why.elements[i]);
      queue_push(&solv->learnt_pool, 0);

      POOL_DEBUG(SAT_DEBUG_UNSOLVABLE, "conflicting update/job assertions over literal %d\n", vv);

        /*
	 * push all of our rules (can only be feature or job rules)
	 * asserting this literal on the problem stack
	 */
	
      for (i = solv->featurerules, rr = solv->rules + i; i < solv->learntrules; i++, rr++)
	{
	  if (rr->d < 0                          /* disabled */
	      || rr->w2)                         /*  or no assertion */
	    continue;
	  if (rr->p != vv                        /* not affecting the literal */
	      && rr->p != -vv)
	    continue;
	  if (MAPTST(&solv->weakrulemap, i))     /* weak: silently ignore */
	    continue;
	    
	  POOL_DEBUG(SAT_DEBUG_UNSOLVABLE, " - disabling rule #%d\n", i);
	    
          solver_printruleclass(solv, SAT_DEBUG_UNSOLVABLE, solv->rules + i);
	    
	  v = i;
	    /* is is a job rule ? */
	  if (i >= solv->jobrules && i < solv->jobrules_end)
	    v = -(solv->ruletojob.elements[i - solv->jobrules] + 1);
	    
	  queue_push(&solv->problems, v);
	  disableproblem(solv, v);
	}
      queue_push(&solv->problems, 0);

      /*
       * start over
       * (back up from decisions)
       */
      while (solv->decisionq.count > decisionstart)
	{
	  v = solv->decisionq.elements[--solv->decisionq.count];
	  --solv->decisionq_why.count;
	  vv = v > 0 ? v : -v;
	  solv->decisionmap[vv] = 0;
	}
      ii = -1; /* restarts loop at 0 */
    }

  /*
   * phase 2: now do the weak assertions
   */
  for (ii = 0; ii < solv->ruleassertions.count; ii++)
    {
      ri = solv->ruleassertions.elements[ii];
      r = solv->rules + ri;
      if (r->d < 0 || r->w2)	                 /* disabled or no assertion */
	continue;
      if (ri >= solv->learntrules || !MAPTST(&solv->weakrulemap, ri))       /* skip non-weak */
	continue;
      v = r->p;
      vv = v > 0 ? v : -v;
      /*
       * decide !
       * (if not yet decided)
       */
      if (!solv->decisionmap[vv])
	{
	  queue_push(&solv->decisionq, v);
	  queue_push(&solv->decisionq_why, r - solv->rules);
	  solv->decisionmap[vv] = v > 0 ? 1 : -1;
	  IF_POOLDEBUG (SAT_DEBUG_PROPAGATE)
	    {
	      Solvable *s = solv->pool->solvables + vv;
	      if (v < 0)
		POOL_DEBUG(SAT_DEBUG_PROPAGATE, "conflicting %s (weak assertion)\n", solvable2str(solv->pool, s));
	      else
		POOL_DEBUG(SAT_DEBUG_PROPAGATE, "installing  %s (weak assertion)\n", solvable2str(solv->pool, s));
	    }
	  continue;
	}
      /*
       * previously decided, sane ?
       */
      if (v > 0 && solv->decisionmap[vv] > 0)
	continue;
      if (v < 0 && solv->decisionmap[vv] < 0)
	continue;
	
      POOL_DEBUG(SAT_DEBUG_UNSOLVABLE, "assertion conflict, but I am weak, disabling ");
      solver_printrule(solv, SAT_DEBUG_UNSOLVABLE, r);

      if (ri >= solv->jobrules && ri < solv->jobrules_end)
	v = -(solv->ruletojob.elements[ri - solv->jobrules] + 1);
      else
	v = ri;
      disableproblem(solv, v);
      if (v < 0)
	reenablepolicyrules(solv, &solv->job, -(v + 1));
    }
  
  POOL_DEBUG(SAT_DEBUG_SCHUBI, "----- makeruledecisions end; size decisionq: %d -----\n",solv->decisionq.count);
}


/*-------------------------------------------------------------------
 * enable/disable learnt rules 
 *
 * we have enabled or disabled some of our rules. We now reenable all
 * of our learnt rules except the ones that were learnt from rules that
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
	  assert(why > 0 && why < i);
	  if (solv->rules[why].d < 0)
	    break;
	}
      /* why != 0: we found a disabled rule, disable the learnt rule */
      if (why && r->d >= 0)
	{
	  IF_POOLDEBUG (SAT_DEBUG_SOLUTIONS)
	    {
	      POOL_DEBUG(SAT_DEBUG_SOLUTIONS, "disabling ");
	      solver_printruleclass(solv, SAT_DEBUG_SOLUTIONS, r);
	    }
          disablerule(solv, r);
	}
      else if (!why && r->d < 0)
	{
	  IF_POOLDEBUG (SAT_DEBUG_SOLUTIONS)
	    {
	      POOL_DEBUG(SAT_DEBUG_SOLUTIONS, "re-enabling ");
	      solver_printruleclass(solv, SAT_DEBUG_SOLUTIONS, r);
	    }
          enablerule(solv, r);
	}
    }
}


/*-------------------------------------------------------------------
 * enable weak rules
 * 
 * Enable all rules, except learnt rules, which are
 * - disabled and weak (set in weakrulemap)
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
      enablerule(solv, r);
    }
}


/*-------------------------------------------------------------------
 * policy rule enabling/disabling
 *
 * we need to disable policy rules that conflict with our job list, and
 * also reenable such rules with the job was changed due to solution generation
 *
 */

static inline void
disableinfarchrule(Solver *solv, Id name)
{
  Pool *pool = solv->pool;
  Rule *r;
  int i;
  for (i = solv->infarchrules, r = solv->rules + i; i < solv->infarchrules_end; i++, r++)
    {
      if (r->p < 0 && r->d >= 0 && pool->solvables[-r->p].name == name)
	disablerule(solv, r);
    }
}

static inline void
reenableinfarchrule(Solver *solv, Id name)
{
  Pool *pool = solv->pool;
  Rule *r;
  int i;
  for (i = solv->infarchrules, r = solv->rules + i; i < solv->infarchrules_end; i++, r++)
    {
      if (r->p < 0 && r->d < 0 && pool->solvables[-r->p].name == name)
	{
	  enablerule(solv, r);
	  IF_POOLDEBUG (SAT_DEBUG_SOLUTIONS)
	    {
	      POOL_DEBUG(SAT_DEBUG_SOLUTIONS, "@@@ re-enabling ");
	      solver_printruleclass(solv, SAT_DEBUG_SOLUTIONS, r);
	    }
	}
    }
}

static inline void
disableduprule(Solver *solv, Id name)
{
  Pool *pool = solv->pool;
  Rule *r;
  int i;
  for (i = solv->duprules, r = solv->rules + i; i < solv->duprules_end; i++, r++)
    {
      if (r->p < 0 && r->d >= 0 && pool->solvables[-r->p].name == name)
	disablerule(solv, r);
    }
}

static inline void
reenableduprule(Solver *solv, Id name)
{
  Pool *pool = solv->pool;
  Rule *r;
  int i;
  for (i = solv->duprules, r = solv->rules + i; i < solv->duprules_end; i++, r++)
    {
      if (r->p < 0 && r->d < 0 && pool->solvables[-r->p].name == name)
	{
	  enablerule(solv, r);
	  IF_POOLDEBUG (SAT_DEBUG_SOLUTIONS)
	    {
	      POOL_DEBUG(SAT_DEBUG_SOLUTIONS, "@@@ re-enabling ");
	      solver_printruleclass(solv, SAT_DEBUG_SOLUTIONS, r);
	    }
	}
    }
}

static inline void
disableupdaterule(Solver *solv, Id p)
{
  Rule *r;

  MAPSET(&solv->noupdate, p - solv->installed->start);
  r = solv->rules + solv->updaterules + (p - solv->installed->start);
  if (r->p && r->d >= 0)
    disablerule(solv, r);
  r = solv->rules + solv->featurerules + (p - solv->installed->start);
  if (r->p && r->d >= 0)
    disablerule(solv, r);
}

static inline void
reenableupdaterule(Solver *solv, Id p)
{
  Pool *pool = solv->pool;
  Rule *r;

  MAPCLR(&solv->noupdate, p - solv->installed->start);
  r = solv->rules + solv->updaterules + (p - solv->installed->start);
  if (r->p)
    {
      if (r->d >= 0)
	return;
      enablerule(solv, r);
      IF_POOLDEBUG (SAT_DEBUG_SOLUTIONS)
	{
	  POOL_DEBUG(SAT_DEBUG_SOLUTIONS, "@@@ re-enabling ");
	  solver_printruleclass(solv, SAT_DEBUG_SOLUTIONS, r);
	}
      return;
    }
  r = solv->rules + solv->featurerules + (p - solv->installed->start);
  if (r->p && r->d < 0)
    {
      enablerule(solv, r);
      IF_POOLDEBUG (SAT_DEBUG_SOLUTIONS)
	{
	  POOL_DEBUG(SAT_DEBUG_SOLUTIONS, "@@@ re-enabling ");
	  solver_printruleclass(solv, SAT_DEBUG_SOLUTIONS, r);
	}
    }
}

#define DISABLE_UPDATE	1
#define DISABLE_INFARCH	2
#define DISABLE_DUP	3

static void
jobtodisablelist(Solver *solv, Id how, Id what, Queue *q)
{
  Pool *pool = solv->pool;
  Id select, p, pp;
  Repo *installed;
  Solvable *s;
  int i;

  installed = solv->installed;
  select = how & SOLVER_SELECTMASK;
  switch (how & SOLVER_JOBMASK)
    {
    case SOLVER_INSTALL:
      if ((select == SOLVER_SOLVABLE_NAME || select == SOLVER_SOLVABLE_PROVIDES) && solv->infarchrules != solv->infarchrules_end && ISRELDEP(what))
	{
	  Reldep *rd = GETRELDEP(pool, what);
	  if (rd->flags == REL_ARCH)
	    {
	      int qcnt = q->count;
	      FOR_JOB_SELECT(p, pp, select, what)
		{
		  s = pool->solvables + p;
		  /* unify names */
		  for (i = qcnt; i < q->count; i += 2)
		    if (q->elements[i + 1] == s->name)
		      break;
		  if (i < q->count)
		    continue;
		  queue_push(q, DISABLE_INFARCH);
		  queue_push(q, s->name);
		}
	    }
	}
      if (select != SOLVER_SOLVABLE)
	break;
      s = pool->solvables + what;
      if (solv->infarchrules != solv->infarchrules_end)
	{
	  queue_push(q, DISABLE_INFARCH);
	  queue_push(q, s->name);
	}
      if (solv->duprules != solv->duprules_end)
	{
	  queue_push(q, DISABLE_DUP);
	  queue_push(q, s->name);
	}
      if (!installed)
	return;
      if (solv->noobsoletes.size && MAPTST(&solv->noobsoletes, what))
	return;
      if (s->repo == installed)
	{
	  queue_push(q, DISABLE_UPDATE);
	  queue_push(q, what);
	  return;
	}
      if (s->obsoletes)
	{
	  Id obs, *obsp;
	  obsp = s->repo->idarraydata + s->obsoletes;
	  while ((obs = *obsp++) != 0)
	    FOR_PROVIDES(p, pp, obs)
	      {
		Solvable *ps = pool->solvables + p;
		if (ps->repo != installed)
		  continue;
		if (!solv->obsoleteusesprovides && !pool_match_nevr(pool, ps, obs))
		  continue;
		queue_push(q, DISABLE_UPDATE);
		queue_push(q, p);
	      }
	}
      FOR_PROVIDES(p, pp, s->name)
	{
	  Solvable *ps = pool->solvables + p;
	  if (ps->repo != installed)
	    continue;
	  if (!solv->implicitobsoleteusesprovides && ps->name != s->name)
	    continue;
	  queue_push(q, DISABLE_UPDATE);
	  queue_push(q, p);
	}
      return;
    case SOLVER_ERASE:
      if (!installed)
	break;
      FOR_JOB_SELECT(p, pp, select, what)
	if (pool->solvables[p].repo == installed)
	  {
	    queue_push(q, DISABLE_UPDATE);
	    queue_push(q, p);
	  }
      return;
    default:
      return;
    }
}

/* disable all policy rules that are in conflict with our job list */
static void
disablepolicyrules(Solver *solv, Queue *job)
{
  int i, j;
  Queue allq;
  Rule *r;
  Id lastjob = -1;
  Id allqbuf[128];

  queue_init_buffer(&allq, allqbuf, sizeof(allqbuf)/sizeof(*allqbuf));

  for (i = solv->jobrules; i < solv->jobrules_end; i++)
    {
      r = solv->rules + i;
      if (r->d < 0)	/* disabled? */
	continue;
      j = solv->ruletojob.elements[i - solv->jobrules];
      if (j == lastjob)
	continue;
      lastjob = j;
      jobtodisablelist(solv, job->elements[j], job->elements[j + 1], &allq);
    }
  MAPZERO(&solv->noupdate);
  for (i = 0; i < allq.count; i += 2)
    {
      Id type = allq.elements[i], arg = allq.elements[i + 1];
      switch(type)
	{
	case DISABLE_UPDATE:
	  disableupdaterule(solv, arg);
	  break;
	case DISABLE_INFARCH:
	  disableinfarchrule(solv, arg);
	  break;
	case DISABLE_DUP:
	  disableduprule(solv, arg);
	  break;
	default:
	  break;
	}
    }
  queue_free(&allq);
}

/* we just disabled job #jobidx, now reenable all policy rules that were
 * disabled because of this job */
static void
reenablepolicyrules(Solver *solv, Queue *job, int jobidx)
{
  int i, j;
  Queue q, allq;
  Rule *r;
  Id lastjob = -1;
  Id qbuf[32], allqbuf[128];

  queue_init_buffer(&q, qbuf, sizeof(qbuf)/sizeof(*qbuf));
  queue_init_buffer(&allq, allqbuf, sizeof(allqbuf)/sizeof(*allqbuf));
  jobtodisablelist(solv, job->elements[jobidx], job->elements[jobidx + 1], &q);
  if (!q.count)
    return;
  for (i = solv->jobrules; i < solv->jobrules_end; i++)
    {
      r = solv->rules + i;
      if (r->d < 0)	/* disabled? */
	continue;
      j = solv->ruletojob.elements[i - solv->jobrules];
      if (j == lastjob)
	continue;
      lastjob = j;
      jobtodisablelist(solv, job->elements[j], job->elements[j + 1], &allq);
    }
  for (j = 0; j < q.count; j += 2)
    {
      Id type = q.elements[j], arg = q.elements[j + 1];
      for (i = 0; i < allq.count; i += 2)
	if (allq.elements[i] == type && allq.elements[i + 1] == arg)
	  break;
      if (i < allq.count)
	continue;	/* still disabled */
      switch(type)
	{
	case DISABLE_UPDATE:
	  reenableupdaterule(solv, arg);
	  break;
	case DISABLE_INFARCH:
	  reenableinfarchrule(solv, arg);
	  break;
	case DISABLE_DUP:
	  reenableduprule(solv, arg);
	  break;
	}
    }
  queue_free(&allq);
  queue_free(&q);
}

/*-------------------------------------------------------------------
 * rule generation
 *
 */

/*
 *  special multiversion patch conflict handling:
 *  a patch conflict is also satisfied, if some other
 *  version with the same name/arch that doesn't conflict
 *  get's installed. The generated rule is thus:
 *  -patch|-cpack|opack1|opack2|...
 */
Id
makemultiversionconflict(Solver *solv, Id n, Id con)
{
  Pool *pool = solv->pool;
  Solvable *s, *sn;
  Queue q;
  Id p, pp, qbuf[64];

  sn = pool->solvables + n;
  queue_init_buffer(&q, qbuf, sizeof(qbuf)/sizeof(*qbuf));
  queue_push(&q, -n);
  FOR_PROVIDES(p, pp, sn->name)
    {
      s = pool->solvables + p;
      if (s->name != sn->name || s->arch != sn->arch)
	continue;
      if (!MAPTST(&solv->noobsoletes, p))
	continue;
      if (pool_match_nevr(pool, pool->solvables + p, con))
	continue;
      /* here we have a multiversion solvable that doesn't conflict */
      /* thus we're not in conflict if it is installed */
      queue_push(&q, p);
    }
  if (q.count == 1)
    return -n;	/* no other package found, generate normal conflict */
  return pool_queuetowhatprovides(pool, &q);
}

static inline void
addrpmrule(Solver *solv, Id p, Id d, int type, Id dep)
{
  if (!solv->ruleinfoq)
    addrule(solv, p, d);
  else
    addrpmruleinfo(solv, p, d, type, dep);
}

/*-------------------------------------------------------------------
 * 
 * add (install) rules for solvable
 * 
 * s: Solvable for which to add rules
 * m: m[s] = 1 for solvables which have rules, prevent rule duplication
 * 
 * Algorithm: 'visit all nodes of a graph'. The graph nodes are
 *  solvables, the edges their dependencies.
 *  Starting from an installed solvable, this will create all rules
 *  representing the graph created by the solvables dependencies.
 * 
 * for unfulfilled requirements, conflicts, obsoletes,....
 * add a negative assertion for solvables that are not installable
 * 
 * It will also create rules for all solvables referenced by 's'
 *  i.e. descend to all providers of requirements of 's'
 *
 */

static void
addrpmrulesforsolvable(Solver *solv, Solvable *s, Map *m)
{
  Pool *pool = solv->pool;
  Repo *installed = solv->installed;

  /* 'work' queue. keeps Ids of solvables we still have to work on.
     And buffer for it. */
  Queue workq;
  Id workqbuf[64];
    
  int i;
    /* if to add rules for broken deps ('rpm -V' functionality)
     * 0 = yes, 1 = no
     */
  int dontfix;
    /* Id var and pointer for each dependency
     * (not used in parallel)
     */
  Id req, *reqp;
  Id con, *conp;
  Id obs, *obsp;
  Id rec, *recp;
  Id sug, *sugp;
  Id p, pp;		/* whatprovides loops */
  Id *dp;		/* ptr to 'whatprovides' */
  Id n;			/* Id for current solvable 's' */

  POOL_DEBUG(SAT_DEBUG_SCHUBI, "----- addrpmrulesforsolvable -----\n");

  queue_init_buffer(&workq, workqbuf, sizeof(workqbuf)/sizeof(*workqbuf));
  queue_push(&workq, s - pool->solvables);	/* push solvable Id to work queue */

  /* loop until there's no more work left */
  while (workq.count)
    {
      /*
       * n: Id of solvable
       * s: Pointer to solvable
       */

      n = queue_shift(&workq);		/* 'pop' next solvable to work on from queue */
      if (m)
	{
	  if (MAPTST(m, n))		/* continue if already visited */
	    continue;
	  MAPSET(m, n);			/* mark as visited */
	}

      s = pool->solvables + n;		/* s = Solvable in question */

      dontfix = 0;
      if (installed			/* Installed system available */
	  && !solv->fixsystem		/* NOT repair errors in rpm dependency graph */
	  && s->repo == installed)	/* solvable is installed? */
        {
	  dontfix = 1;			/* dont care about broken rpm deps */
        }

      if (!dontfix
	  && s->arch != ARCH_SRC
	  && s->arch != ARCH_NOSRC
	  && !pool_installable(pool, s))
	{
	  POOL_DEBUG(SAT_DEBUG_RULE_CREATION, "package %s [%d] is not installable\n", solvable2str(pool, s), (Id)(s - pool->solvables));
	  addrpmrule(solv, -n, 0, SOLVER_RULE_RPM_NOT_INSTALLABLE, 0);
	}

      /* yet another SUSE hack, sigh */
      if (pool->nscallback && !strncmp("product:", id2str(pool, s->name), 8))
        {
          Id buddy = pool->nscallback(pool, pool->nscallbackdata, NAMESPACE_PRODUCTBUDDY, n);
          if (buddy > 0 && buddy != SYSTEMSOLVABLE && buddy != n && buddy < pool->nsolvables)
            {
              addrpmrule(solv, n, -buddy, SOLVER_RULE_RPM_PACKAGE_REQUIRES, solvable_selfprovidedep(pool->solvables + n));
              addrpmrule(solv, buddy, -n, SOLVER_RULE_RPM_PACKAGE_REQUIRES, solvable_selfprovidedep(pool->solvables + buddy)); 
	      if (m && !MAPTST(m, buddy))
		queue_push(&workq, buddy);
            }
        }

      /*-----------------------------------------
       * check requires of s
       */

      if (s->requires)
	{
	  reqp = s->repo->idarraydata + s->requires;
	  while ((req = *reqp++) != 0)            /* go through all requires */
	    {
	      if (req == SOLVABLE_PREREQMARKER)   /* skip the marker */
		continue;

	      /* find list of solvables providing 'req' */
	      dp = pool_whatprovides_ptr(pool, req);

	      if (*dp == SYSTEMSOLVABLE)	  /* always installed */
		continue;

	      if (dontfix)
		{
		  /* the strategy here is to not insist on dependencies
                   * that are already broken. so if we find one provider
                   * that was already installed, we know that the
                   * dependency was not broken before so we enforce it */
		 
		  /* check if any of the providers for 'req' is installed */
		  for (i = 0; (p = dp[i]) != 0; i++)
		    {
		      if (pool->solvables[p].repo == installed)
			break;		/* provider was installed */
		    }
		  /* didn't find an installed provider: previously broken dependency */
		  if (!p)
		    {
		      POOL_DEBUG(SAT_DEBUG_RULE_CREATION, "ignoring broken requires %s of installed package %s\n", dep2str(pool, req), solvable2str(pool, s));
		      continue;
		    }
		}

	      if (!*dp)
		{
		  /* nothing provides req! */
		  POOL_DEBUG(SAT_DEBUG_RULE_CREATION, "package %s [%d] is not installable (%s)\n", solvable2str(pool, s), (Id)(s - pool->solvables), dep2str(pool, req));
		  addrpmrule(solv, -n, 0, SOLVER_RULE_RPM_NOTHING_PROVIDES_DEP, req);
		  continue;
		}

	      IF_POOLDEBUG (SAT_DEBUG_RULE_CREATION)
	        {
		  POOL_DEBUG(SAT_DEBUG_RULE_CREATION,"  %s requires %s\n", solvable2str(pool, s), dep2str(pool, req));
		  for (i = 0; dp[i]; i++)
		    POOL_DEBUG(SAT_DEBUG_RULE_CREATION, "   provided by %s\n", solvid2str(pool, dp[i]));
	        }

	      /* add 'requires' dependency */
              /* rule: (-requestor|provider1|provider2|...|providerN) */
	      addrpmrule(solv, -n, dp - pool->whatprovidesdata, SOLVER_RULE_RPM_PACKAGE_REQUIRES, req);

	      /* descend the dependency tree
	         push all non-visited providers on the work queue */
	      if (m)
		{
		  for (; *dp; dp++)
		    {
		      if (!MAPTST(m, *dp))
			queue_push(&workq, *dp);
		    }
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
	  int ispatch = 0;

	  /* we treat conflicts in patches a bit differen:
	   * - nevr matching
	   * - multiversion handling
	   * XXX: we should really handle this different, looking
	   * at the name is a bad hack
	   */
	  if (!strncmp("patch:", id2str(pool, s->name), 6))
	    ispatch = 1;
	  conp = s->repo->idarraydata + s->conflicts;
	  /* foreach conflicts of 's' */
	  while ((con = *conp++) != 0)
	    {
	      /* foreach providers of a conflict of 's' */
	      FOR_PROVIDES(p, pp, con)
		{
		  if (ispatch && !pool_match_nevr(pool, pool->solvables + p, con))
		    continue;
		  /* dontfix: dont care about conflicts with already installed packs */
		  if (dontfix && pool->solvables[p].repo == installed)
		    continue;
		  /* p == n: self conflict */
		  if (p == n && !solv->allowselfconflicts)
		    {
		      if (ISRELDEP(con))
			{
			  Reldep *rd = GETRELDEP(pool, con);
			  if (rd->flags == REL_NAMESPACE && rd->name == NAMESPACE_OTHERPROVIDERS)
			    continue;
			}
		      p = 0;	/* make it a negative assertion, aka 'uninstallable' */
		    }
		  if (p && ispatch && solv->noobsoletes.size && MAPTST(&solv->noobsoletes, p) && ISRELDEP(con))
		    {
		      /* our patch conflicts with a noobsoletes (aka multiversion) package */
		      p = -makemultiversionconflict(solv, p, con);
		    }
                 /* rule: -n|-p: either solvable _or_ provider of conflict */
		  addrpmrule(solv, -n, -p, p ? SOLVER_RULE_RPM_PACKAGE_CONFLICT : SOLVER_RULE_RPM_SELF_CONFLICT, con);
		}
	    }
	}

      /*-----------------------------------------
       * check obsoletes if not installed
       * (only installation will trigger the obsoletes in rpm)
       */
      if (!installed || pool->solvables[n].repo != installed)
	{			       /* not installed */
	  int noobs = solv->noobsoletes.size && MAPTST(&solv->noobsoletes, n);
	  if (s->obsoletes && !noobs)
	    {
	      obsp = s->repo->idarraydata + s->obsoletes;
	      /* foreach obsoletes */
	      while ((obs = *obsp++) != 0)
		{
		  /* foreach provider of an obsoletes of 's' */ 
		  FOR_PROVIDES(p, pp, obs)
		    {
		      if (!solv->obsoleteusesprovides /* obsoletes are matched names, not provides */
			  && !pool_match_nevr(pool, pool->solvables + p, obs))
			continue;
		      addrpmrule(solv, -n, -p, SOLVER_RULE_RPM_PACKAGE_OBSOLETES, obs);
		    }
		}
	    }
	  FOR_PROVIDES(p, pp, s->name)
	    {
	      Solvable *ps = pool->solvables + p;
	      /* we still obsolete packages with same nevra, like rpm does */
	      /* (actually, rpm mixes those packages. yuck...) */
	      if (noobs && (s->name != ps->name || s->evr != ps->evr || s->arch != ps->arch))
		continue;
	      if (!solv->implicitobsoleteusesprovides && s->name != ps->name)
		continue;
	      if (s->name == ps->name)
	        addrpmrule(solv, -n, -p, SOLVER_RULE_RPM_SAME_NAME, 0);
	      else
	        addrpmrule(solv, -n, -p, SOLVER_RULE_RPM_IMPLICIT_OBSOLETES, s->name);
	    }
	}

      /*-----------------------------------------
       * add recommends to the work queue
       */
      if (s->recommends && m)
	{
	  recp = s->repo->idarraydata + s->recommends;
	  while ((rec = *recp++) != 0)
	    {
	      FOR_PROVIDES(p, pp, rec)
		if (!MAPTST(m, p))
		  queue_push(&workq, p);
	    }
	}
      if (s->suggests && m)
	{
	  sugp = s->repo->idarraydata + s->suggests;
	  while ((sug = *sugp++) != 0)
	    {
	      FOR_PROVIDES(p, pp, sug)
		if (!MAPTST(m, p))
		  queue_push(&workq, p);
	    }
	}
    }
  queue_free(&workq);
  POOL_DEBUG(SAT_DEBUG_SCHUBI, "----- addrpmrulesforsolvable end -----\n");
}


/*-------------------------------------------------------------------
 * 
 * Add package rules for weak rules
 *
 * m: visited solvables
 */

static void
addrpmrulesforweak(Solver *solv, Map *m)
{
  Pool *pool = solv->pool;
  Solvable *s;
  Id sup, *supp;
  int i, n;

  POOL_DEBUG(SAT_DEBUG_SCHUBI, "----- addrpmrulesforweak -----\n");
    /* foreach solvable in pool */
  for (i = n = 1; n < pool->nsolvables; i++, n++)
    {
      if (i == pool->nsolvables)		/* wrap i */
	i = 1;
      if (MAPTST(m, i))				/* been there */
	continue;

      s = pool->solvables + i;
      if (!pool_installable(pool, s))		/* only look at installable ones */
	continue;

      sup = 0;
      if (s->supplements)
	{
	  /* find possible supplements */
	  supp = s->repo->idarraydata + s->supplements;
	  while ((sup = *supp++) != ID_NULL)
	    if (dep_possible(solv, sup, m))
	      break;
	}

      /* if nothing found, check for enhances */
      if (!sup && s->enhances)
	{
	  supp = s->repo->idarraydata + s->enhances;
	  while ((sup = *supp++) != ID_NULL)
	    if (dep_possible(solv, sup, m))
	      break;
	}
      /* if nothing found, goto next solvables */
      if (!sup)
	continue;
      addrpmrulesforsolvable(solv, s, m);
      n = 0;			/* check all solvables again */
    }
  POOL_DEBUG(SAT_DEBUG_SCHUBI, "----- addrpmrulesforweak end -----\n");
}


/*-------------------------------------------------------------------
 * 
 * add package rules for possible updates
 * 
 * s: solvable
 * m: map of already visited solvables
 * allow_all: 0 = dont allow downgrades, 1 = allow all candidates
 */

static void
addrpmrulesforupdaters(Solver *solv, Solvable *s, Map *m, int allow_all)
{
  Pool *pool = solv->pool;
  int i;
    /* queue and buffer for it */
  Queue qs;
  Id qsbuf[64];

  POOL_DEBUG(SAT_DEBUG_SCHUBI, "----- addrpmrulesforupdaters -----\n");

  queue_init_buffer(&qs, qsbuf, sizeof(qsbuf)/sizeof(*qsbuf));
    /* find update candidates for 's' */
  policy_findupdatepackages(solv, s, &qs, allow_all);
    /* add rule for 's' if not already done */
  if (!MAPTST(m, s - pool->solvables))
    addrpmrulesforsolvable(solv, s, m);
    /* foreach update candidate, add rule if not already done */
  for (i = 0; i < qs.count; i++)
    if (!MAPTST(m, qs.elements[i]))
      addrpmrulesforsolvable(solv, pool->solvables + qs.elements[i], m);
  queue_free(&qs);

  POOL_DEBUG(SAT_DEBUG_SCHUBI, "----- addrpmrulesforupdaters -----\n");
}

static Id
finddistupgradepackages(Solver *solv, Solvable *s, Queue *qs, int allow_all)
{
  Pool *pool = solv->pool;
  int i;

  policy_findupdatepackages(solv, s, qs, allow_all);
  if (!qs->count)
    {
      if (allow_all)
        return 0;	/* orphaned, don't create feature rule */
      /* check if this is an orphaned package */
      policy_findupdatepackages(solv, s, qs, 1);
      if (!qs->count)
	return 0;	/* orphaned, don't create update rule */
      qs->count = 0;
      return -SYSTEMSOLVABLE;	/* supported but not installable */
    }
  if (allow_all)
    return s - pool->solvables;
  /* check if it is ok to keep the installed package */
  for (i = 0; i < qs->count; i++)
    {
      Solvable *ns = pool->solvables + qs->elements[i];
      if (s->evr == ns->evr && solvable_identical(s, ns))
        return s - pool->solvables;
    }
  /* nope, it must be some other package */
  return -SYSTEMSOLVABLE;
}

/* add packages from the dup repositories to the update candidates
 * this isn't needed for the global dup mode as all packages are
 * from dup repos in that case */
static void
addduppackages(Solver *solv, Solvable *s, Queue *qs)
{
  Queue dupqs;
  Id p, dupqsbuf[64];
  int i;
  int oldnoupdateprovide = solv->noupdateprovide;

  queue_init_buffer(&dupqs, dupqsbuf, sizeof(dupqsbuf)/sizeof(*dupqsbuf));
  solv->noupdateprovide = 1;
  policy_findupdatepackages(solv, s, &dupqs, 2);
  solv->noupdateprovide = oldnoupdateprovide;
  for (i = 0; i < dupqs.count; i++)
    {
      p = dupqs.elements[i];
      if (MAPTST(&solv->dupmap, p))
        queue_pushunique(qs, p);
    }
  queue_free(&dupqs);
}

/*-------------------------------------------------------------------
 * 
 * add rule for update
 *   (A|A1|A2|A3...)  An = update candidates for A
 *
 * s = (installed) solvable
 */

static void
addupdaterule(Solver *solv, Solvable *s, int allow_all)
{
  /* installed packages get a special upgrade allowed rule */
  Pool *pool = solv->pool;
  Id p, d;
  Queue qs;
  Id qsbuf[64];

  POOL_DEBUG(SAT_DEBUG_SCHUBI, "-----  addupdaterule -----\n");
  queue_init_buffer(&qs, qsbuf, sizeof(qsbuf)/sizeof(*qsbuf));
  p = s - pool->solvables;
  /* find update candidates for 's' */
  if (solv->distupgrade)
    p = finddistupgradepackages(solv, s, &qs, allow_all);
  else
    policy_findupdatepackages(solv, s, &qs, allow_all);
  if (!allow_all && !solv->distupgrade && solv->dupinvolvedmap.size && MAPTST(&solv->dupinvolvedmap, p))
    addduppackages(solv, s, &qs);

  if (!allow_all && qs.count && solv->noobsoletes.size)
    {
      int i, j;

      d = pool_queuetowhatprovides(pool, &qs);
      /* filter out all noobsoletes packages as they don't update */
      for (i = j = 0; i < qs.count; i++)
	{
	  if (MAPTST(&solv->noobsoletes, qs.elements[i]))
	    {
	      /* it's ok if they have same nevra */
	      Solvable *ps = pool->solvables + qs.elements[i];
	      if (ps->name != s->name || ps->evr != s->evr || ps->arch != s->arch)
	        continue;
	    }
	  qs.elements[j++] = qs.elements[i];
	}
      if (j == 0 && p == -SYSTEMSOLVABLE && solv->distupgrade)
	{
	  queue_push(&solv->orphaned, s - pool->solvables);	/* treat as orphaned */
	  j = qs.count;
	}
      if (j < qs.count)
	{
	  if (d && solv->updatesystem && solv->installed && s->repo == solv->installed)
	    {
	      if (!solv->multiversionupdaters)
		solv->multiversionupdaters = sat_calloc(solv->installed->end - solv->installed->start, sizeof(Id));
	      solv->multiversionupdaters[s - pool->solvables - solv->installed->start] = d;
	    }
	  qs.count = j;
	}
    }
  if (qs.count && p == -SYSTEMSOLVABLE)
    p = queue_shift(&qs);
  d = qs.count ? pool_queuetowhatprovides(pool, &qs) : 0;
  queue_free(&qs);
  addrule(solv, p, d);	/* allow update of s */
  POOL_DEBUG(SAT_DEBUG_SCHUBI, "-----  addupdaterule end -----\n");
}


/********************************************************************/
/* watches */


/*-------------------------------------------------------------------
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
  /* do it reverse so rpm rules get triggered first (XXX: obsolete?) */
  for (i = 1, r = solv->rules + solv->nrules - 1; i < solv->nrules; i++, r--)
#else
  for (i = 1, r = solv->rules + 1; i < solv->nrules; i++, r++)
#endif
    {
      if (!r->w2)		/* assertions do not need watches */
	continue;

      /* see addwatches_rule(solv, r) */
      r->n1 = solv->watches[nsolvables + r->w1];
      solv->watches[nsolvables + r->w1] = r - solv->rules;

      r->n2 = solv->watches[nsolvables + r->w2];
      solv->watches[nsolvables + r->w2] = r - solv->rules;
    }
}


/*-------------------------------------------------------------------
 *
 * add watches (for a new learned rule)
 * sets up watches for a single rule
 * 
 * see also makewatches() above.
 */

static inline void
addwatches_rule(Solver *solv, Rule *r)
{
  int nsolvables = solv->pool->nsolvables;

  r->n1 = solv->watches[nsolvables + r->w1];
  solv->watches[nsolvables + r->w1] = r - solv->rules;

  r->n2 = solv->watches[nsolvables + r->w2];
  solv->watches[nsolvables + r->w2] = r - solv->rules;
}


/********************************************************************/
/*
 * rule propagation
 */


/* shortcuts to check if a literal (positive or negative) assignment
 * evaluates to 'true' or 'false'
 */
#define DECISIONMAP_TRUE(p) ((p) > 0 ? (decisionmap[p] > 0) : (decisionmap[-p] < 0))
#define DECISIONMAP_FALSE(p) ((p) > 0 ? (decisionmap[p] < 0) : (decisionmap[-p] > 0))
#define DECISIONMAP_UNDEF(p) (decisionmap[(p) > 0 ? (p) : -(p)] == 0)

/*-------------------------------------------------------------------
 * 
 * propagate
 *
 * make decision and propagate to all rules
 * 
 * Evaluate each term affected by the decision (linked through watches)
 * If we find unit rules we make new decisions based on them
 * 
 * Everything's fixed there, it's just finding rules that are
 * unit.
 * 
 * return : 0 = everything is OK
 *          rule = conflict found in this rule
 */

static Rule *
propagate(Solver *solv, int level)
{
  Pool *pool = solv->pool;
  Id *rp, *next_rp;           /* rule pointer, next rule pointer in linked list */
  Rule *r;                    /* rule */
  Id p, pkg, other_watch;
  Id *dp;
  Id *decisionmap = solv->decisionmap;
    
  Id *watches = solv->watches + pool->nsolvables;   /* place ptr in middle */

  POOL_DEBUG(SAT_DEBUG_PROPAGATE, "----- propagate -----\n");

  /* foreach non-propagated decision */
  while (solv->propagate_index < solv->decisionq.count)
    {
	/*
	 * 'pkg' was just decided
	 * negate because our watches trigger if literal goes FALSE
	 */
      pkg = -solv->decisionq.elements[solv->propagate_index++];
	
      IF_POOLDEBUG (SAT_DEBUG_PROPAGATE)
        {
	  POOL_DEBUG(SAT_DEBUG_PROPAGATE, "propagate for decision %d level %d\n", -pkg, level);
	  solver_printruleelement(solv, SAT_DEBUG_PROPAGATE, 0, -pkg);
        }

      /* foreach rule where 'pkg' is now FALSE */
      for (rp = watches + pkg; *rp; rp = next_rp)
	{
	  r = solv->rules + *rp;
	  if (r->d < 0)
	    {
	      /* rule is disabled, goto next */
	      if (pkg == r->w1)
	        next_rp = &r->n1;
	      else
	        next_rp = &r->n2;
	      continue;
	    }

	  IF_POOLDEBUG (SAT_DEBUG_PROPAGATE)
	    {
	      POOL_DEBUG(SAT_DEBUG_PROPAGATE,"  watch triggered ");
	      solver_printrule(solv, SAT_DEBUG_PROPAGATE, r);
	    }

	    /* 'pkg' was just decided (was set to FALSE)
	     * 
	     *  now find other literal watch, check clause
	     *   and advance on linked list
	     */
	  if (pkg == r->w1)
	    {
	      other_watch = r->w2;
	      next_rp = &r->n1;
	    }
	  else
	    {
	      other_watch = r->w1;
	      next_rp = &r->n2;
	    }
	    
	    /* 
	     * This term is already true (through the other literal)
	     * so we have nothing to do
	     */
	  if (DECISIONMAP_TRUE(other_watch))
	    continue;

	    /*
	     * The other literal is FALSE or UNDEF
	     * 
	     */
	    
          if (r->d)
	    {
	      /* Not a binary clause, try to move our watch.
	       * 
	       * Go over all literals and find one that is
	       *   not other_watch
	       *   and not FALSE
	       * 
	       * (TRUE is also ok, in that case the rule is fulfilled)
	       */
	      if (r->p                                /* we have a 'p' */
		  && r->p != other_watch              /* which is not watched */
		  && !DECISIONMAP_FALSE(r->p))        /* and not FALSE */
		{
		  p = r->p;
		}
	      else                                    /* go find a 'd' to make 'true' */
		{
		  /* foreach p in 'd'
		     we just iterate sequentially, doing it in another order just changes the order of decisions, not the decisions itself
		   */
		  for (dp = pool->whatprovidesdata + r->d; (p = *dp++) != 0;)
		    {
		      if (p != other_watch              /* which is not watched */
		          && !DECISIONMAP_FALSE(p))     /* and not FALSE */
		        break;
		    }
		}

	      if (p)
		{
		  /*
		   * if we found some p that is UNDEF or TRUE, move
		   * watch to it
		   */
		  IF_POOLDEBUG (SAT_DEBUG_PROPAGATE)
		    {
		      if (p > 0)
			POOL_DEBUG(SAT_DEBUG_PROPAGATE, "    -> move w%d to %s\n", (pkg == r->w1 ? 1 : 2), solvid2str(pool, p));
		      else
			POOL_DEBUG(SAT_DEBUG_PROPAGATE,"    -> move w%d to !%s\n", (pkg == r->w1 ? 1 : 2), solvid2str(pool, -p));
		    }
		    
		  *rp = *next_rp;
		  next_rp = rp;
		    
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
	      /* search failed, thus all unwatched literals are FALSE */
		
	    } /* not binary */
	    
            /*
	     * unit clause found, set literal other_watch to TRUE
	     */

	  if (DECISIONMAP_FALSE(other_watch))	   /* check if literal is FALSE */
	    return r;  		                   /* eek, a conflict! */
	    
	  IF_POOLDEBUG (SAT_DEBUG_PROPAGATE)
	    {
	      POOL_DEBUG(SAT_DEBUG_PROPAGATE, "   unit ");
	      solver_printrule(solv, SAT_DEBUG_PROPAGATE, r);
	    }

	  if (other_watch > 0)
            decisionmap[other_watch] = level;    /* install! */
	  else
	    decisionmap[-other_watch] = -level;  /* remove! */
	    
	  queue_push(&solv->decisionq, other_watch);
	  queue_push(&solv->decisionq_why, r - solv->rules);

	  IF_POOLDEBUG (SAT_DEBUG_PROPAGATE)
	    {
	      if (other_watch > 0)
		POOL_DEBUG(SAT_DEBUG_PROPAGATE, "    -> decided to install %s\n", solvid2str(pool, other_watch));
	      else
		POOL_DEBUG(SAT_DEBUG_PROPAGATE, "    -> decided to conflict %s\n", solvid2str(pool, -other_watch));
	    }
	    
	} /* foreach rule involving 'pkg' */
	
    } /* while we have non-decided decisions */
    
  POOL_DEBUG(SAT_DEBUG_PROPAGATE, "----- propagate end-----\n");

  return 0;	/* all is well */
}


/********************************************************************/
/* Analysis */

/*-------------------------------------------------------------------
 * 
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
  Id d, v, vv, *dp, why;
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
	solver_printruleclass(solv, SAT_DEBUG_ANALYZE, c);
      queue_push(&solv->learnt_pool, c - solv->rules);
      d = c->d < 0 ? -c->d - 1 : c->d;
      dp = d ? pool->whatprovidesdata + d : 0;
      /* go through all literals of the rule */
      for (i = -1; ; i++)
	{
	  if (i == -1)
	    v = c->p;
	  else if (d == 0)
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
	  MAPSET(&seen, vv);
	  if (l == 1)
	    l1num++;			/* need to do this one in level1 pass */
	  else if (l == level)
	    num++;			/* need to do this one as well */
	  else
	    {
	      queue_push(&r, v);	/* not level1 or conflict level, add to new rule */
	      if (l > rlevel)
		rlevel = l;
	    }
	}
l1retry:
      if (!num && !--l1num)
	break;	/* all level 1 literals done */
      for (;;)
	{
	  assert(idx > 0);
	  v = solv->decisionq.elements[--idx];
	  vv = v > 0 ? v : -v;
	  if (MAPTST(&seen, vv))
	    break;
	}
      MAPCLR(&seen, vv);
      if (num && --num == 0)
	{
	  *pr = -v;	/* so that v doesn't get lost */
	  if (!l1num)
	    break;
	  POOL_DEBUG(SAT_DEBUG_ANALYZE, "got %d involved level 1 decisions\n", l1num);
	  for (i = 0; i < r.count; i++)
	    {
	      v = r.elements[i];
	      MAPCLR(&seen, v > 0 ? v : -v);
	    }
	  /* only level 1 marks left */
	  l1num++;
	  goto l1retry;
	}
      why = solv->decisionq_why.elements[idx];
      if (why <= 0)	/* just in case, maybe for SYSTEMSOLVABLE */
	goto l1retry;
      c = solv->rules + why;
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
      solver_printruleelement(solv, SAT_DEBUG_ANALYZE, 0, *pr);
      for (i = 0; i < r.count; i++)
        solver_printruleelement(solv, SAT_DEBUG_ANALYZE, 0, r.elements[i]);
    }
  /* push end marker on learnt reasons stack */
  queue_push(&solv->learnt_pool, 0);
  if (whyp)
    *whyp = learnt_why;
  solv->stats_learned++;
  return rlevel;
}


/*-------------------------------------------------------------------
 * 
 * reset_solver
 * 
 * reset the solver decisions to right after the rpm rules.
 * called after rules have been enabled/disabled
 */

static void
reset_solver(Solver *solv)
{
  Pool *pool = solv->pool;
  int i;
  Id v;

  /* rewind all decisions */
  for (i = solv->decisionq.count - 1; i >= 0; i--)
    {
      v = solv->decisionq.elements[i];
      solv->decisionmap[v > 0 ? v : -v] = 0;
    }
  solv->decisionq_why.count = 0;
  solv->decisionq.count = 0;
  solv->recommends_index = -1;
  solv->propagate_index = 0;

  /* adapt learnt rule status to new set of enabled/disabled rules */
  enabledisablelearntrules(solv);

  /* redo all job/update decisions */
  makeruledecisions(solv);
  POOL_DEBUG(SAT_DEBUG_UNSOLVABLE, "decisions so far: %d\n", solv->decisionq.count);
}


/*-------------------------------------------------------------------
 * 
 * analyze_unsolvable_rule
 */

static void
analyze_unsolvable_rule(Solver *solv, Rule *r, Id *lastweakp)
{
  Pool *pool = solv->pool;
  int i;
  Id why = r - solv->rules;

  IF_POOLDEBUG (SAT_DEBUG_UNSOLVABLE)
    solver_printruleclass(solv, SAT_DEBUG_UNSOLVABLE, r);
  if (solv->learntrules && why >= solv->learntrules)
    {
      for (i = solv->learnt_why.elements[why - solv->learntrules]; solv->learnt_pool.elements[i]; i++)
	if (solv->learnt_pool.elements[i] > 0)
	  analyze_unsolvable_rule(solv, solv->rules + solv->learnt_pool.elements[i], lastweakp);
      return;
    }
  if (MAPTST(&solv->weakrulemap, why))
    if (!*lastweakp || why > *lastweakp)
      *lastweakp = why;
  /* do not add rpm rules to problem */
  if (why < solv->rpmrules_end)
    return;
  /* turn rule into problem */
  if (why >= solv->jobrules && why < solv->jobrules_end)
    why = -(solv->ruletojob.elements[why - solv->jobrules] + 1);
  /* normalize dup/infarch rules */
  if (why > solv->infarchrules && why < solv->infarchrules_end)
    {
      Id name = pool->solvables[-solv->rules[why].p].name;
      while (why > solv->infarchrules && pool->solvables[-solv->rules[why - 1].p].name == name)
	why--;
    }
  if (why > solv->duprules && why < solv->duprules_end)
    {
      Id name = pool->solvables[-solv->rules[why].p].name;
      while (why > solv->duprules && pool->solvables[-solv->rules[why - 1].p].name == name)
	why--;
    }

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


/*-------------------------------------------------------------------
 * 
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
  Id d, v, vv, *dp, why;
  int l, i, idx;
  Id *decisionmap = solv->decisionmap;
  int oldproblemcount;
  int oldlearntpoolcount;
  Id lastweak;

  POOL_DEBUG(SAT_DEBUG_UNSOLVABLE, "ANALYZE UNSOLVABLE ----------------------\n");
  solv->stats_unsolvable++;
  oldproblemcount = solv->problems.count;
  oldlearntpoolcount = solv->learnt_pool.count;

  /* make room for proof index */
  /* must update it later, as analyze_unsolvable_rule would confuse
   * it with a rule index if we put the real value in already */
  queue_push(&solv->problems, 0);

  r = cr;
  map_init(&seen, pool->nsolvables);
  queue_push(&solv->learnt_pool, r - solv->rules);
  lastweak = 0;
  analyze_unsolvable_rule(solv, r, &lastweak);
  d = r->d < 0 ? -r->d - 1 : r->d;
  dp = d ? pool->whatprovidesdata + d : 0;
  for (i = -1; ; i++)
    {
      if (i == -1)
	v = r->p;
      else if (d == 0)
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
      assert(why > 0);
      queue_push(&solv->learnt_pool, why);
      r = solv->rules + why;
      analyze_unsolvable_rule(solv, r, &lastweak);
      d = r->d < 0 ? -r->d - 1 : r->d;
      dp = d ? pool->whatprovidesdata + d : 0;
      for (i = -1; ; i++)
	{
	  if (i == -1)
	    v = r->p;
	  else if (d == 0)
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

  if (lastweak)
    {
      Id v;
      /* disable last weak rule */
      solv->problems.count = oldproblemcount;
      solv->learnt_pool.count = oldlearntpoolcount;
      if (lastweak >= solv->jobrules && lastweak < solv->jobrules_end)
	v = -(solv->ruletojob.elements[lastweak - solv->jobrules] + 1);
      else
        v = lastweak;
      POOL_DEBUG(SAT_DEBUG_UNSOLVABLE, "disabling ");
      solver_printruleclass(solv, SAT_DEBUG_UNSOLVABLE, solv->rules + lastweak);
      disableproblem(solv, v);
      if (v < 0)
	reenablepolicyrules(solv, &solv->job, -(v + 1));
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
      /* XXX: might want to enable all weak rules again */
      reset_solver(solv);
      return 1;
    }
  POOL_DEBUG(SAT_DEBUG_UNSOLVABLE, "UNSOLVABLE\n");
  return 0;
}


/********************************************************************/
/* Decision revert */

/*-------------------------------------------------------------------
 * 
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
      if (v > 0 && solv->recommendations.count && v == solv->recommendations.elements[solv->recommendations.count - 1])
	solv->recommendations.count--;
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


/*-------------------------------------------------------------------
 * 
 * watch2onhighest - put watch2 on literal with highest level
 */

static inline void
watch2onhighest(Solver *solv, Rule *r)
{
  int l, wl = 0;
  Id d, v, *dp;

  d = r->d < 0 ? -r->d - 1 : r->d;
  if (!d)
    return;	/* binary rule, both watches are set */
  dp = solv->pool->whatprovidesdata + d;
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


/*-------------------------------------------------------------------
 * 
 * setpropagatelearn
 *
 * add free decision (solvable to install) to decisionq
 * increase level and propagate decision
 * return if no conflict.
 *
 * in conflict case, analyze conflict rule, add resulting
 * rule to learnt rule set, make decision from learnt
 * rule (always unit) and re-propagate.
 *
 * returns the new solver level or 0 if unsolvable
 *
 */

static int
setpropagatelearn(Solver *solv, int level, Id decision, int disablerules, Id ruleid)
{
  Pool *pool = solv->pool;
  Rule *r;
  Id p = 0, d = 0;
  int l, why;

  assert(ruleid >= 0);
  if (decision)
    {
      level++;
      if (decision > 0)
        solv->decisionmap[decision] = level;
      else
        solv->decisionmap[-decision] = -level;
      queue_push(&solv->decisionq, decision);
      queue_push(&solv->decisionq_why, -ruleid);	/* <= 0 -> free decision */
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
      r = addrule(solv, p, d);
      assert(r);
      assert(solv->learnt_why.count == (r - solv->rules) - solv->learntrules);
      queue_push(&solv->learnt_why, why);
      if (d)
	{
	  /* at least 2 literals, needs watches */
	  watch2onhighest(solv, r);
	  addwatches_rule(solv, r);
	}
      else
	{
	  /* learnt rule is an assertion */
          queue_push(&solv->ruleassertions, r - solv->rules);
	}
      /* the new rule is unit by design */
      solv->decisionmap[p > 0 ? p : -p] = p > 0 ? level : -level;
      queue_push(&solv->decisionq, p);
      queue_push(&solv->decisionq_why, r - solv->rules);
      IF_POOLDEBUG (SAT_DEBUG_ANALYZE)
	{
	  POOL_DEBUG(SAT_DEBUG_ANALYZE, "decision: ");
	  solver_printruleelement(solv, SAT_DEBUG_ANALYZE, 0, p);
	  POOL_DEBUG(SAT_DEBUG_ANALYZE, "new rule: ");
	  solver_printrule(solv, SAT_DEBUG_ANALYZE, r);
	}
    }
  return level;
}


/*-------------------------------------------------------------------
 * 
 * select and install
 * 
 * install best package from the queue. We add an extra package, inst, if
 * provided. See comment in weak install section.
 *
 * returns the new solver level or 0 if unsolvable
 *
 */

static int
selectandinstall(Solver *solv, int level, Queue *dq, int disablerules, Id ruleid)
{
  Pool *pool = solv->pool;
  Id p;
  int i;

  if (dq->count > 1)
    policy_filter_unwanted(solv, dq, POLICY_MODE_CHOOSE);
  if (dq->count > 1)
    {
      /* XXX: didn't we already do that? */
      /* XXX: shouldn't we prefer installed packages? */
      /* XXX: move to policy.c? */
      /* choose the supplemented one */
      for (i = 0; i < dq->count; i++)
	if (solver_is_supplementing(solv, pool->solvables + dq->elements[i]))
	  {
	    dq->elements[0] = dq->elements[i];
	    dq->count = 1;
	    break;
	  }
    }
  if (dq->count > 1)
    {
      /* multiple candidates, open a branch */
      for (i = 1; i < dq->count; i++)
	queue_push(&solv->branches, dq->elements[i]);
      queue_push(&solv->branches, -level);
    }
  p = dq->elements[0];

  POOL_DEBUG(SAT_DEBUG_POLICY, "installing %s\n", solvid2str(pool, p));

  return setpropagatelearn(solv, level, p, disablerules, ruleid);
}


/********************************************************************/
/* Main solver interface */


/*-------------------------------------------------------------------
 * 
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
solver_create(Pool *pool)
{
  Solver *solv;
  solv = (Solver *)sat_calloc(1, sizeof(Solver));
  solv->pool = pool;
  solv->installed = pool->installed;

  queue_init(&solv->transaction);
  queue_init(&solv->transaction_info);
  queue_init(&solv->ruletojob);
  queue_init(&solv->decisionq);
  queue_init(&solv->decisionq_why);
  queue_init(&solv->problems);
  queue_init(&solv->suggestions);
  queue_init(&solv->recommendations);
  queue_init(&solv->orphaned);
  queue_init(&solv->learnt_why);
  queue_init(&solv->learnt_pool);
  queue_init(&solv->branches);
  queue_init(&solv->covenantq);
  queue_init(&solv->weakruleq);
  queue_init(&solv->ruleassertions);

  map_init(&solv->recommendsmap, pool->nsolvables);
  map_init(&solv->suggestsmap, pool->nsolvables);
  map_init(&solv->noupdate, solv->installed ? solv->installed->end - solv->installed->start : 0);
  solv->recommends_index = 0;

  solv->decisionmap = (Id *)sat_calloc(pool->nsolvables, sizeof(Id));
  solv->nrules = 1;
  solv->rules = sat_extend_resize(solv->rules, solv->nrules, sizeof(Rule), RULES_BLOCK);
  memset(solv->rules, 0, sizeof(Rule));

  return solv;
}


/*-------------------------------------------------------------------
 * 
 * solver_free
 */

void
solver_free(Solver *solv)
{
  queue_free(&solv->transaction);
  queue_free(&solv->transaction_info);
  queue_free(&solv->job);
  queue_free(&solv->ruletojob);
  queue_free(&solv->decisionq);
  queue_free(&solv->decisionq_why);
  queue_free(&solv->learnt_why);
  queue_free(&solv->learnt_pool);
  queue_free(&solv->problems);
  queue_free(&solv->solutions);
  queue_free(&solv->suggestions);
  queue_free(&solv->recommendations);
  queue_free(&solv->orphaned);
  queue_free(&solv->branches);
  queue_free(&solv->covenantq);
  queue_free(&solv->weakruleq);
  queue_free(&solv->ruleassertions);

  map_free(&solv->recommendsmap);
  map_free(&solv->suggestsmap);
  map_free(&solv->noupdate);
  map_free(&solv->weakrulemap);
  map_free(&solv->noobsoletes);

  map_free(&solv->updatemap);
  map_free(&solv->dupmap);
  map_free(&solv->dupinvolvedmap);

  sat_free(solv->decisionmap);
  sat_free(solv->rules);
  sat_free(solv->watches);
  sat_free(solv->obsoletes);
  sat_free(solv->obsoletes_data);
  sat_free(solv->multiversionupdaters);
  sat_free(solv->transaction_installed);
  sat_free(solv);
}


/*-------------------------------------------------------------------
 * 
 * run_solver
 *
 * all rules have been set up, now actually run the solver
 *
 */

static void
run_solver(Solver *solv, int disablerules, int doweak)
{
  Queue dq;		/* local decisionqueue */
  Queue dqs;		/* local decisionqueue for supplements */
  int systemlevel;
  int level, olevel;
  Rule *r;
  int i, j, n;
  Solvable *s;
  Pool *pool = solv->pool;
  Id p, *dp;
  int minimizationsteps;

  IF_POOLDEBUG (SAT_DEBUG_RULE_CREATION)
    {
      POOL_DEBUG (SAT_DEBUG_RULE_CREATION, "number of rules: %d\n", solv->nrules);
      for (i = 1; i < solv->nrules; i++)
	solver_printruleclass(solv, SAT_DEBUG_RULE_CREATION, solv->rules + i);
    }

  POOL_DEBUG(SAT_DEBUG_STATS, "initial decisions: %d\n", solv->decisionq.count);

  IF_POOLDEBUG (SAT_DEBUG_SCHUBI)
    solver_printdecisions(solv);

  /* start SAT algorithm */
  level = 1;
  systemlevel = level + 1;
  POOL_DEBUG(SAT_DEBUG_STATS, "solving...\n");

  queue_init(&dq);
  queue_init(&dqs);

  /*
   * here's the main loop:
   * 1) propagate new decisions (only needed for level 1)
   * 2) try to keep installed packages
   * 3) fulfill all unresolved rules
   * 4) install recommended packages
   * 5) minimalize solution if we had choices
   * if we encounter a problem, we rewind to a safe level and restart
   * with step 1
   */
   
  minimizationsteps = 0;
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
	      queue_free(&dqs);
	      return;
	    }
	}

     if (level < systemlevel)
	{
	  POOL_DEBUG(SAT_DEBUG_STATS, "resolving job rules\n");
	  for (i = solv->jobrules, r = solv->rules + i; i < solv->jobrules_end; i++, r++)
	    {
	      Id l;
	      if (r->d < 0)		/* ignore disabled rules */
		continue;
	      queue_empty(&dq);
	      FOR_RULELITERALS(l, dp, r)
		{
		  if (l < 0)
		    {
		      if (solv->decisionmap[-l] <= 0)
			break;
		    }
		  else
		    {
		      if (solv->decisionmap[l] > 0)
			break;
		      if (solv->decisionmap[l] == 0)
			queue_push(&dq, l);
		    }
		}
	      if (l || !dq.count)
		continue;
	      /* prune to installed if not updating */
	      if (!solv->updatesystem && solv->installed && dq.count > 1)
		{
		  int j, k;
		  for (j = k = 0; j < dq.count; j++)
		    {
		      Solvable *s = pool->solvables + dq.elements[j];
		      if (s->repo == solv->installed)
			dq.elements[k++] = dq.elements[j];
		    }
		  if (k)
		    dq.count = k;
		}
	      olevel = level;
	      level = selectandinstall(solv, level, &dq, disablerules, i);
	      if (level == 0)
		{
		  queue_free(&dq);
		  queue_free(&dqs);
		  return;
		}
	      if (level <= olevel)
		break;
	    }
	  systemlevel = level + 1;
	  if (i < solv->jobrules_end)
	    continue;
	}


      /*
       * installed packages
       */

      if (level < systemlevel && solv->installed && solv->installed->nsolvables)
	{
	  Repo *installed = solv->installed;
	  int pass;

	  /* we use two passes if we need to update packages 
           * to create a better user experience */
	  for (pass = solv->updatemap.size ? 0 : 1; pass < 2; pass++)
	    {
	      FOR_REPO_SOLVABLES(installed, i, s)
		{
		  Rule *rr;
		  Id d;

		  /* XXX: noupdate check is probably no longer needed, as all jobs should
                   * already be satisfied */
		  if (MAPTST(&solv->noupdate, i - installed->start))
		    continue;
		  if (solv->decisionmap[i] > 0)
		    continue;
		  if (!pass && solv->updatemap.size && !MAPTST(&solv->updatemap, i - installed->start))
		    continue;		/* updates first */
		  r = solv->rules + solv->updaterules + (i - installed->start);
		  rr = r;
		  if (!rr->p || rr->d < 0)	/* disabled -> look at feature rule */
		    rr -= solv->installed->end - solv->installed->start;
		  if (!rr->p)		/* identical to update rule? */
		    rr = r;
		  if (!rr->p)
		    continue;		/* orpaned package */

		  queue_empty(&dq);
		  if (solv->decisionmap[i] < 0 || solv->updatesystem || (solv->updatemap.size && MAPTST(&solv->updatemap, i - installed->start)) || rr->p != i)
		    {
		      if (solv->noobsoletes.size && solv->multiversionupdaters
			     && (d = solv->multiversionupdaters[i - installed->start]) != 0)
			{
			  /* special multiversion handling, make sure best version is chosen */
			  queue_push(&dq, i);
			  while ((p = pool->whatprovidesdata[d++]) != 0)
			    if (solv->decisionmap[p] >= 0)
			      queue_push(&dq, p);
			  policy_filter_unwanted(solv, &dq, POLICY_MODE_CHOOSE);
			  p = dq.elements[0];
			  if (p != i && solv->decisionmap[p] == 0)
			    {
			      rr = solv->rules + solv->featurerules + (i - solv->installed->start);
			      if (!rr->p)		/* update rule == feature rule? */
				rr = rr - solv->featurerules + solv->updaterules;
			      dq.count = 1;
			    }
			  else
			    dq.count = 0;
			}
		      else
			{
			  /* update to best package */
			  FOR_RULELITERALS(p, dp, rr)
			    {
			      if (solv->decisionmap[p] > 0)
				{
				  dq.count = 0;		/* already fulfilled */
				  break;
				}
			      if (!solv->decisionmap[p])
				queue_push(&dq, p);
			    }
			}
		    }
		  /* install best version */
		  if (dq.count)
		    {
		      olevel = level;
		      level = selectandinstall(solv, level, &dq, disablerules, rr - solv->rules);
		      if (level == 0)
			{
			  queue_free(&dq);
			  queue_free(&dqs);
			  return;
			}
		      if (level <= olevel)
			break;
		    }
		  /* if still undecided keep package */
		  if (solv->decisionmap[i] == 0)
		    {
		      olevel = level;
		      POOL_DEBUG(SAT_DEBUG_POLICY, "keeping %s\n", solvid2str(pool, i));
		      level = setpropagatelearn(solv, level, i, disablerules, r - solv->rules);
		      if (level == 0)
			{
			  queue_free(&dq);
			  queue_free(&dqs);
			  return;
			}
		      if (level <= olevel)
			break;
		    }
		}
	      if (i < installed->end)
		break;
	    }
	  systemlevel = level + 1;
	  if (pass < 2)
	    continue;		/* had trouble, retry */
	}

      if (level < systemlevel)
        systemlevel = level;

      /*
       * decide
       */

      POOL_DEBUG(SAT_DEBUG_POLICY, "deciding unresolved rules\n");
      for (i = 1, n = 1; ; i++, n++)
	{
	  if (n == solv->nrules)
	    break;
	  if (i == solv->nrules)
	    i = 1;
	  r = solv->rules + i;
	  if (r->d < 0)		/* ignore disabled rules */
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
	  IF_POOLDEBUG (SAT_DEBUG_PROPAGATE)
	    {
	      POOL_DEBUG(SAT_DEBUG_PROPAGATE, "unfulfilled ");
	      solver_printruleclass(solv, SAT_DEBUG_PROPAGATE, r);
	    }
	  /* dq.count < 2 cannot happen as this means that
	   * the rule is unit */
	  assert(dq.count > 1);

	  olevel = level;
	  level = selectandinstall(solv, level, &dq, disablerules, r - solv->rules);
	  if (level == 0)
	    {
	      queue_free(&dq);
	      queue_free(&dqs);
	      return;
	    }
	  if (level < systemlevel || level == 1)
	    break;
	  n = 0;
	} /* for(), decide */

      if (n != solv->nrules)	/* continue if level < systemlevel */
	continue;

      if (doweak)
	{
	  int qcount;

	  POOL_DEBUG(SAT_DEBUG_POLICY, "installing recommended packages\n");
	  queue_empty(&dq);	/* recommended packages */
	  queue_empty(&dqs);	/* supplemented packages */
	  for (i = 1; i < pool->nsolvables; i++)
	    {
	      if (solv->decisionmap[i] < 0)
		continue;
	      if (solv->decisionmap[i] > 0)
		{
		  /* installed, check for recommends */
		  Id *recp, rec, pp, p;
		  s = pool->solvables + i;
		  if (solv->ignorealreadyrecommended && s->repo == solv->installed)
		    continue;
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
		  if (!s->supplements)
		    continue;
		  if (!pool_installable(pool, s))
		    continue;
		  if (!solver_is_supplementing(solv, s))
		    continue;
		  queue_push(&dqs, i);
		}
	    }

	  /* filter out all packages obsoleted by installed packages */
	  /* this is no longer needed if we have reverse obsoletes */
          if ((dqs.count || dq.count) && solv->installed)
	    {
	      Map obsmap;
	      Id obs, *obsp, po, ppo;

	      map_init(&obsmap, pool->nsolvables);
	      for (p = solv->installed->start; p < solv->installed->end; p++)
		{
		  s = pool->solvables + p;
		  if (s->repo != solv->installed || !s->obsoletes)
		    continue;
		  if (solv->decisionmap[p] <= 0)
		    continue;
		  if (solv->noobsoletes.size && MAPTST(&solv->noobsoletes, p))
		    continue;
		  obsp = s->repo->idarraydata + s->obsoletes;
		  /* foreach obsoletes */
		  while ((obs = *obsp++) != 0)
		    FOR_PROVIDES(po, ppo, obs)
		      MAPSET(&obsmap, po);
		}
	      for (i = j = 0; i < dqs.count; i++)
		if (!MAPTST(&obsmap, dqs.elements[i]))
		  dqs.elements[j++] = dqs.elements[i];
	      dqs.count = j;
	      for (i = j = 0; i < dq.count; i++)
		if (!MAPTST(&obsmap, dq.elements[i]))
		  dq.elements[j++] = dq.elements[i];
	      dq.count = j;
	      map_free(&obsmap);
	    }

          /* filter out all already supplemented packages if requested */
          if (solv->ignorealreadyrecommended && dqs.count)
	    {
	      /* turn off all new packages */
	      for (i = 0; i < solv->decisionq.count; i++)
		{
		  p = solv->decisionq.elements[i];
		  if (p < 0)
		    continue;
		  s = pool->solvables + p;
		  if (s->repo && s->repo != solv->installed)
		    solv->decisionmap[p] = -solv->decisionmap[p];
		}
	      /* filter out old supplements */
	      for (i = j = 0; i < dqs.count; i++)
		{
		  p = dqs.elements[i];
		  s = pool->solvables + p;
		  if (!s->supplements)
		    continue;
		  if (!solver_is_supplementing(solv, s))
		    dqs.elements[j++] = p;
		}
	      dqs.count = j;
	      /* undo turning off */
	      for (i = 0; i < solv->decisionq.count; i++)
		{
		  p = solv->decisionq.elements[i];
		  if (p < 0)
		    continue;
		  s = pool->solvables + p;
		  if (s->repo && s->repo != solv->installed)
		    solv->decisionmap[p] = -solv->decisionmap[p];
		}
	    }

	  /* multiversion doesn't mix well with supplements.
	   * filter supplemented packages where we already decided
	   * to install a different version (see bnc#501088) */
          if (dqs.count && solv->noobsoletes.size)
	    {
	      for (i = j = 0; i < dqs.count; i++)
		{
		  p = dqs.elements[i];
		  if (MAPTST(&solv->noobsoletes, p))
		    {
		      Id p2, pp2;
		      s = pool->solvables + p;
		      FOR_PROVIDES(p2, pp2, s->name)
			if (solv->decisionmap[p2] > 0 && pool->solvables[p2].name == s->name)
			  break;
		      if (p2)
			continue;	/* ignore this package */
		    }
		  dqs.elements[j++] = p;
		}
	      dqs.count = j;
	    }

          /* make dq contain both recommended and supplemented pkgs */
	  if (dqs.count)
	    {
	      for (i = 0; i < dqs.count; i++)
		queue_pushunique(&dq, dqs.elements[i]);
	    }

	  if (dq.count)
	    {
	      Map dqmap;
	      int decisioncount = solv->decisionq.count;

	      if (dq.count == 1)
		{
		  /* simple case, just one package. no need to choose  */
		  p = dq.elements[0];
		  if (dqs.count)
		    POOL_DEBUG(SAT_DEBUG_POLICY, "installing supplemented %s\n", solvid2str(pool, p));
		  else
		    POOL_DEBUG(SAT_DEBUG_POLICY, "installing recommended %s\n", solvid2str(pool, p));
		  queue_push(&solv->recommendations, p);
		  level = setpropagatelearn(solv, level, p, 0, 0);
		  continue;	/* back to main loop */
		}

	      /* filter packages, this gives us the best versions */
	      policy_filter_unwanted(solv, &dq, POLICY_MODE_RECOMMEND);

	      /* create map of result */
	      map_init(&dqmap, pool->nsolvables);
	      for (i = 0; i < dq.count; i++)
		MAPSET(&dqmap, dq.elements[i]);

	      /* install all supplemented packages */
	      for (i = 0; i < dqs.count; i++)
		{
		  p = dqs.elements[i];
		  if (solv->decisionmap[p] || !MAPTST(&dqmap, p))
		    continue;
		  POOL_DEBUG(SAT_DEBUG_POLICY, "installing supplemented %s\n", solvid2str(pool, p));
		  queue_push(&solv->recommendations, p);
		  olevel = level;
		  level = setpropagatelearn(solv, level, p, 0, 0);
		  if (level <= olevel)
		    break;
		}
	      if (i < dqs.count || solv->decisionq.count < decisioncount)
		{
		  map_free(&dqmap);
		  continue;
		}

	      /* install all recommended packages */
	      /* more work as we want to created branches if multiple
               * choices are valid */
	      for (i = 0; i < decisioncount; i++)
		{
		  Id rec, *recp, pp;
		  p = solv->decisionq.elements[i];
		  if (p < 0)
		    continue;
		  s = pool->solvables + p;
		  if (!s->repo || (solv->ignorealreadyrecommended && s->repo == solv->installed))
		    continue;
		  if (!s->recommends)
		    continue;
		  recp = s->repo->idarraydata + s->recommends;
		  while ((rec = *recp++) != 0)
		    {
		      queue_empty(&dq);
		      FOR_PROVIDES(p, pp, rec)
			{
			  if (solv->decisionmap[p] > 0)
			    {
			      dq.count = 0;
			      break;
			    }
			  else if (solv->decisionmap[p] == 0 && MAPTST(&dqmap, p))
			    queue_pushunique(&dq, p);
			}
		      if (!dq.count)
			continue;
		      if (dq.count > 1)
			{
			  /* multiple candidates, open a branch */
			  for (i = 1; i < dq.count; i++)
			    queue_push(&solv->branches, dq.elements[i]);
			  queue_push(&solv->branches, -level);
			}
		      p = dq.elements[0];
		      POOL_DEBUG(SAT_DEBUG_POLICY, "installing recommended %s\n", solvid2str(pool, p));
		      queue_push(&solv->recommendations, p);
		      olevel = level;
		      level = setpropagatelearn(solv, level, p, 0, 0);
		      if (level <= olevel || solv->decisionq.count < decisioncount)
			break;	/* we had to revert some decisions */
		    }
		  if (rec)
		    break;	/* had a problem above, quit loop */
		}
	      map_free(&dqmap);

	      continue;		/* back to main loop */
	    }
	}

     if (solv->distupgrade && solv->installed)
	{
	  int installedone = 0;

	  /* let's see if we can install some unsupported package */
	  POOL_DEBUG(SAT_DEBUG_STATS, "deciding unsupported packages\n");
	  for (i = 0; i < solv->orphaned.count; i++)
	    {
	      p = solv->orphaned.elements[i];
	      if (solv->decisionmap[p])
		continue;	/* already decided */
	      olevel = level;
	      if (solv->distupgrade_removeunsupported)
		{
		  POOL_DEBUG(SAT_DEBUG_STATS, "removing unsupported %s\n", solvid2str(pool, p));
		  level = setpropagatelearn(solv, level, -p, 0, 0);
		}
	      else
		{
		  POOL_DEBUG(SAT_DEBUG_STATS, "keeping unsupported %s\n", solvid2str(pool, p));
		  level = setpropagatelearn(solv, level, p, 0, 0);
		  installedone = 1;
		}
	      if (level < olevel)
		break;
	    }
	  if (installedone || i < solv->orphaned.count)
	    continue;
	}

     if (solv->solution_callback)
	{
	  solv->solution_callback(solv, solv->solution_callback_data);
	  if (solv->branches.count)
	    {
	      int i = solv->branches.count - 1;
	      int l = -solv->branches.elements[i];
	      Id why;

	      for (; i > 0; i--)
		if (solv->branches.elements[i - 1] < 0)
		  break;
	      p = solv->branches.elements[i];
	      POOL_DEBUG(SAT_DEBUG_STATS, "branching with %s\n", solvid2str(pool, p));
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
	      why = -solv->decisionq_why.elements[solv->decisionq_why.count];
	      assert(why >= 0);
	      level = setpropagatelearn(solv, level, p, disablerules, why);
	      if (level == 0)
		{
		  queue_free(&dq);
		  queue_free(&dqs);
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
	  Id why;

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
	      POOL_DEBUG(SAT_DEBUG_STATS, "minimizing %d -> %d with %s\n", solv->decisionmap[p], lastl, solvid2str(pool, p));
	      minimizationsteps++;

	      level = lastl;
	      revert(solv, level);
	      why = -solv->decisionq_why.elements[solv->decisionq_why.count];
	      assert(why >= 0);
	      olevel = level;
	      level = setpropagatelearn(solv, level, p, disablerules, why);
	      if (level == 0)
		{
		  queue_free(&dq);
		  queue_free(&dqs);
		  return;
		}
	      continue;
	    }
	}
      break;
    }
  POOL_DEBUG(SAT_DEBUG_STATS, "solver statistics: %d learned rules, %d unsolvable, %d minimization steps\n", solv->stats_learned, solv->stats_unsolvable, minimizationsteps);

  POOL_DEBUG(SAT_DEBUG_STATS, "done solving.\n\n");
  queue_free(&dq);
  queue_free(&dqs);
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
refine_suggestion(Solver *solv, Queue *job, Id *problem, Id sug, Queue *refined, int essentialok)
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
  if (!essentialok && sug < 0 && (job->elements[-sug - 1] & SOLVER_ESSENTIAL) != 0)
    return;
  queue_init(&disabled);
  queue_push(refined, sug);

  /* re-enable all problem rules with the exception of "sug"(gestion) */
  revert(solv, 1);
  reset_solver(solv);

  for (i = 0; problem[i]; i++)
    if (problem[i] != sug)
      enableproblem(solv, problem[i]);

  if (sug < 0)
    reenablepolicyrules(solv, job, -(sug + 1));
  else if (sug >= solv->updaterules && sug < solv->updaterules_end)
    {
      /* enable feature rule */
      Rule *r = solv->rules + solv->featurerules + (sug - solv->updaterules);
      if (r->p)
	enablerule(solv, r);
    }

  enableweakrules(solv);

  for (;;)
    {
      int njob, nfeature, nupdate;
      queue_empty(&solv->problems);
      revert(solv, 1);		/* XXX no longer needed? */
      reset_solver(solv);

      if (!solv->problems.count)
        run_solver(solv, 0, 0);

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
	      if (!essentialok && (job->elements[-v -1] & SOLVER_ESSENTIAL) != 0)
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
	  disableproblem(solv, v);
	  if (v >= solv->updaterules && v < solv->updaterules_end)
	    {
	      Rule *r = solv->rules + (v - solv->updaterules + solv->featurerules);
	      if (r->p)
		enablerule(solv, r);	/* enable corresponding feature rule */
	    }
	  if (v < 0)
	    reenablepolicyrules(solv, job, -(v + 1));
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
	      disableproblem(solv, v);
	      if (v >= solv->updaterules && v < solv->updaterules_end)
		{
		  Rule *r = solv->rules + (v - solv->updaterules + solv->featurerules);
		  if (r->p)
		    enablerule(solv, r);
		}
	    }
	}
    }
  /* all done, get us back into the same state as before */
  /* enable refined rules again */
  for (i = 0; i < disabled.count; i++)
    enableproblem(solv, disabled.elements[i]);
  queue_free(&disabled);
  /* reset policy rules */
  for (i = 0; problem[i]; i++)
    enableproblem(solv, problem[i]);
  disablepolicyrules(solv, job);
  /* disable problem rules again */
  for (i = 0; problem[i]; i++)
    disableproblem(solv, problem[i]);
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
	p = -solv->rules[why].p; /* XXX: what to do here? */
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
	p = -solv->rules[why].p; /* XXX: what to do here? */
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
      if (solv->distupgrade && solv->rules[why].p != p && solv->decisionmap[p] > 0)
	{
	  /* distupgrade case, allow to keep old package */
	  queue_push(solutionq, p);
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
prepare_solutions(Solver *solv)
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
  solv->recommendations.count = 0;	/* so that revert() doesn't mess with it */
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
      refine_suggestion(solv, &solv->job, problem.elements, problem.elements[i], &solution, essentialok);
      queue_push(&solv->solutions, 0);	/* reserve room for number of elements */
      for (j = 0; j < solution.count; j++)
	convertsolution(solv, solution.elements[j], &solv->solutions);
      if (solv->solutions.count == solstart + 1)
	{
	  solv->solutions.count--;
	  if (!essentialok && i + 1 == problem.count)
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

Id
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

Id
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

Id
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
  int reqassert = 0;

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
	      if (!d && r->w2 == 0 && !reqassert)
		{
		  if (*reqrp > 0 && r->p < -1)
		    {
		      Id op = -solv->rules[*reqrp].p;
		      if (op > 1 && solv->pool->solvables[op].arch != solv->pool->solvables[-r->p].arch)
			continue;	/* different arch, skip */
		    }
		  /* prefer assertions */
		  *reqrp = rid;
		  reqassert = 1;
		}
	      if (!*reqrp)
		*reqrp = rid;
	      else if (solv->installed && r->p < 0 && solv->pool->solvables[-r->p].repo == solv->installed && !reqassert)
		{
		  /* prefer rules of installed packages */
		  *reqrp = rid;
		}
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


/*-------------------------------------------------------------------
 * 
 * remove disabled conflicts
 *
 * purpose: update the decisionmap after some rules were disabled.
 * this is used to calculate the suggested/recommended package list.
 * Also returns a "removed" list to undo the discisionmap changes.
 */

static void
removedisabledconflicts(Solver *solv, Queue *removed)
{
  Pool *pool = solv->pool;
  int i, n;
  Id p, why, *dp;
  Id new;
  Rule *r;
  Id *decisionmap = solv->decisionmap;

  POOL_DEBUG(SAT_DEBUG_SCHUBI, "removedisabledconflicts\n");
  queue_empty(removed);
  for (i = 0; i < solv->decisionq.count; i++)
    {
      p = solv->decisionq.elements[i];
      if (p > 0)
	continue;
      /* a conflict. we never do conflicts on free decisions, so there
       * must have been an unit rule */
      why = solv->decisionq_why.elements[i];
      assert(why > 0);
      r = solv->rules + why;
      if (r->d < 0 && decisionmap[-p])
	{
	  /* rule is now disabled, remove from decisionmap */
	  POOL_DEBUG(SAT_DEBUG_SCHUBI, "removing conflict for package %s[%d]\n", solvid2str(pool, -p), -p);
	  queue_push(removed, -p);
	  queue_push(removed, decisionmap[-p]);
	  decisionmap[-p] = 0;
	}
    }
  if (!removed->count)
    return;
  /* we removed some confliced packages. some of them might still
   * be in conflict, so search for unit rules and re-conflict */
  new = 0;
  for (i = n = 1, r = solv->rules + i; n < solv->nrules; i++, r++, n++)
    {
      if (i == solv->nrules)
	{
	  i = 1;
	  r = solv->rules + i;
	}
      if (r->d < 0)
	continue;
      if (!r->w2)
	{
	  if (r->p < 0 && !decisionmap[-r->p])
	    new = r->p;
	}
      else if (!r->d)
	{
	  /* binary rule */
	  if (r->p < 0 && decisionmap[-r->p] == 0 && DECISIONMAP_FALSE(r->w2))
	    new = r->p;
	  else if (r->w2 < 0 && decisionmap[-r->w2] == 0 && DECISIONMAP_FALSE(r->p))
	    new = r->w2;
	}
      else
	{
	  if (r->p < 0 && decisionmap[-r->p] == 0)
	    new = r->p;
	  if (new || DECISIONMAP_FALSE(r->p))
	    {
	      dp = pool->whatprovidesdata + r->d;
	      while ((p = *dp++) != 0)
		{
		  if (new && p == new)
		    continue;
		  if (p < 0 && decisionmap[-p] == 0)
		    {
		      if (new)
			{
			  new = 0;
			  break;
			}
		      new = p;
		    }
		  else if (!DECISIONMAP_FALSE(p))
		    {
		      new = 0;
		      break;
		    }
		}
	    }
	}
      if (new)
	{
	  POOL_DEBUG(SAT_DEBUG_SCHUBI, "re-conflicting package %s[%d]\n", solvid2str(pool, -new), -new);
	  decisionmap[-new] = -1;
	  new = 0;
	  n = 0;	/* redo all rules */
	}
    }
}

static inline void
undo_removedisabledconflicts(Solver *solv, Queue *removed)
{
  int i;
  for (i = 0; i < removed->count; i += 2)
    solv->decisionmap[removed->elements[i]] = removed->elements[i + 1];
}


/*-------------------------------------------------------------------
 *
 * weaken solvable dependencies
 */

static void
weaken_solvable_deps(Solver *solv, Id p)
{
  int i;
  Rule *r;

  for (i = 1, r = solv->rules + i; i < solv->rpmrules_end; i++, r++)
    {
      if (r->p != -p)
	continue;
      if ((r->d == 0 || r->d == -1) && r->w2 < 0)
	continue;	/* conflict */
      queue_push(&solv->weakruleq, i);
    }
}


/********************************************************************/
/* main() */


static void
addinfarchrules(Solver *solv, Map *addedmap)
{
  Pool *pool = solv->pool;
  int first, i, j;
  Id p, pp, a, aa, bestarch;
  Solvable *s, *ps, *bests;
  Queue badq, allowedarchs;

  queue_init(&badq);
  queue_init(&allowedarchs);
  solv->infarchrules = solv->nrules;
  for (i = 1; i < pool->nsolvables; i++)
    {
      if (i == SYSTEMSOLVABLE || !MAPTST(addedmap, i))
	continue;
      s = pool->solvables + i;
      first = i;
      bestarch = 0;
      bests = 0;
      queue_empty(&allowedarchs);
      FOR_PROVIDES(p, pp, s->name)
	{
	  ps = pool->solvables + p;
	  if (ps->name != s->name || !MAPTST(addedmap, p))
	    continue;
	  if (p == i)
	    first = 0;
	  if (first)
	    break;
	  a = ps->arch;
	  a = (a <= pool->lastarch) ? pool->id2arch[a] : 0;
	  if (a != 1 && pool->installed && ps->repo == pool->installed)
	    {
	      if (!solv->distupgrade)
	        queue_pushunique(&allowedarchs, ps->arch);	/* also ok to keep this architecture */
	      continue;		/* ignore installed solvables when calculating the best arch */
	    }
	  if (a && a != 1 && (!bestarch || a < bestarch))
	    {
	      bestarch = a;
	      bests = ps;
	    }
	}
      if (first)
	continue;
      /* speed up common case where installed package already has best arch */
      if (allowedarchs.count == 1 && bests && allowedarchs.elements[0] == bests->arch)
	allowedarchs.count--;	/* installed arch is best */
      queue_empty(&badq);
      FOR_PROVIDES(p, pp, s->name)
	{
	  ps = pool->solvables + p;
	  if (ps->name != s->name || !MAPTST(addedmap, p))
	    continue;
	  a = ps->arch;
	  a = (a <= pool->lastarch) ? pool->id2arch[a] : 0;
	  if (a != 1 && bestarch && ((a ^ bestarch) & 0xffff0000) != 0)
	    {
	      for (j = 0; j < allowedarchs.count; j++)
		{
		  aa = allowedarchs.elements[j];
		  if (ps->arch == aa)
		    break;
		  aa = (aa <= pool->lastarch) ? pool->id2arch[aa] : 0;
		  if (aa && ((a ^ aa) & 0xffff0000) == 0)
		    break;	/* compatible */
		}
	      if (j == allowedarchs.count)
		queue_push(&badq, p);
	    }
	}
      if (!badq.count)
	continue;
      /* block all solvables in the badq! */
      for (j = 0; j < badq.count; j++)
	{
	  p = badq.elements[j];
	  addrule(solv, -p, 0);
	}
    }
  queue_free(&badq);
  queue_free(&allowedarchs);
  solv->infarchrules_end = solv->nrules;
}

static void
createdupmaps(Solver *solv, Queue *job)
{
  Pool *pool = solv->pool;
  Repo *repo;
  Id how, what, p, pi, pp, obs, *obsp;
  Solvable *s, *ps;
  int i;

  map_init(&solv->dupmap, pool->nsolvables);
  map_init(&solv->dupinvolvedmap, pool->nsolvables);
  for (i = 0; i < job->count; i += 2)
    {
      how = job->elements[i];
      what = job->elements[i + 1];
      switch (how & SOLVER_JOBMASK)
	{
	case SOLVER_DISTUPGRADE:
	  if (what < 0 || what > pool->nrepos)
	    break;
	  repo = pool->repos[what];
	  FOR_REPO_SOLVABLES(repo, p, s)
	    {
	      MAPSET(&solv->dupmap, p);
	      FOR_PROVIDES(pi, pp, s->name)
		{
		  ps = pool->solvables + pi;
		  if (ps->name != s->name)
		    continue;
		  MAPSET(&solv->dupinvolvedmap, pi);
		}
	      if (s->obsoletes)
		{
		  /* FIXME: check obsoletes/provides combination */
		  obsp = s->repo->idarraydata + s->obsoletes;
		  while ((obs = *obsp++) != 0)
		    {
		      FOR_PROVIDES(pi, pp, obs)
			{
			  if (!solv->obsoleteusesprovides && !pool_match_nevr(pool, pool->solvables + pi, obs))
			    continue;
		          MAPSET(&solv->dupinvolvedmap, pi);
			}
		    }
		}
	    }
	  break;
	default:
	  break;
	}
    }
  MAPCLR(&solv->dupinvolvedmap, SYSTEMSOLVABLE);
}

static void
freedupmaps(Solver *solv)
{
  map_free(&solv->dupmap);
  map_free(&solv->dupinvolvedmap);
}

static void
addduprules(Solver *solv, Map *addedmap)
{
  Pool *pool = solv->pool;
  Id p, pp;
  Solvable *s, *ps;
  int first, i;

  solv->duprules = solv->nrules;
  for (i = 1; i < pool->nsolvables; i++)
    {
      if (i == SYSTEMSOLVABLE || !MAPTST(addedmap, i))
	continue;
      s = pool->solvables + i;
      first = i;
      FOR_PROVIDES(p, pp, s->name)
	{
	  ps = pool->solvables + p;
	  if (ps->name != s->name || !MAPTST(addedmap, p))
	    continue;
	  if (p == i)
	    first = 0;
	  if (first)
	    break;
	  if (!MAPTST(&solv->dupinvolvedmap, p))
	    continue;
	  if (solv->installed && ps->repo == solv->installed)
	    {
	      if (!solv->updatemap.size)
		map_init(&solv->updatemap, pool->nsolvables);
	      MAPSET(&solv->updatemap, p);
	      if (!MAPTST(&solv->dupmap, p))
		{
		  Id ip, ipp;
		  /* is installed identical to a good one? */
		  FOR_PROVIDES(ip, ipp, s->name)
		    {
		      Solvable *is = pool->solvables + ip;
		      if (!MAPTST(&solv->dupmap, ip))
			continue;
		      if (is->evr == s->evr && solvable_identical(s, is))
			break;
		    }
		  if (!ip)
		    addrule(solv, -p, 0);	/* no match, sorry */
		}
	    }
	  else if (!MAPTST(&solv->dupmap, p))
	    addrule(solv, -p, 0);
	}
    }
  solv->duprules_end = solv->nrules;
}


static void
findrecommendedsuggested(Solver *solv)
{
  Pool *pool = solv->pool;
  Queue redoq, disabledq;
  int goterase, i;
  Solvable *s;
  Rule *r;
  Map obsmap;

  map_init(&obsmap, pool->nsolvables);
  if (solv->installed)
    {
      Id obs, *obsp, p, po, ppo;
      for (p = solv->installed->start; p < solv->installed->end; p++)
	{
	  s = pool->solvables + p;
	  if (s->repo != solv->installed || !s->obsoletes)
	    continue;
	  if (solv->decisionmap[p] <= 0)
	    continue;
	  if (solv->noobsoletes.size && MAPTST(&solv->noobsoletes, p))
	    continue;
	  obsp = s->repo->idarraydata + s->obsoletes;
	  /* foreach obsoletes */
	  while ((obs = *obsp++) != 0)
	    FOR_PROVIDES(po, ppo, obs)
	      MAPSET(&obsmap, po);
	}
    }

  queue_init(&redoq);
  queue_init(&disabledq);
  goterase = 0;
  /* disable all erase jobs (including weak "keep uninstalled" rules) */
  for (i = solv->jobrules, r = solv->rules + i; i < solv->jobrules_end; i++, r++)
    {
      if (r->d < 0)	/* disabled ? */
	continue;
      if (r->p >= 0)	/* install job? */
	continue;
      queue_push(&disabledq, i);
      disablerule(solv, r);
      goterase++;
    }
  
  if (goterase)
    {
      enabledisablelearntrules(solv);
      removedisabledconflicts(solv, &redoq);
    }

  /*
   * find recommended packages
   */
    
  /* if redoq.count == 0 we already found all recommended in the
   * solver run */
  if (redoq.count || solv->dontinstallrecommended || !solv->dontshowinstalledrecommended || solv->ignorealreadyrecommended)
    {
      Id rec, *recp, p, pp;

      /* create map of all recommened packages */
      solv->recommends_index = -1;
      MAPZERO(&solv->recommendsmap);
      for (i = 0; i < solv->decisionq.count; i++)
	{
	  p = solv->decisionq.elements[i];
	  if (p < 0)
	    continue;
	  s = pool->solvables + p;
	  if (s->recommends)
	    {
	      recp = s->repo->idarraydata + s->recommends;
	      while ((rec = *recp++) != 0)
		{
		  FOR_PROVIDES(p, pp, rec)
		    if (solv->decisionmap[p] > 0)
		      break;
		  if (p)
		    {
		      if (!solv->dontshowinstalledrecommended)
			{
			  FOR_PROVIDES(p, pp, rec)
			    if (solv->decisionmap[p] > 0)
			      MAPSET(&solv->recommendsmap, p);
			}
		      continue;	/* p != 0: already fulfilled */
		    }
		  FOR_PROVIDES(p, pp, rec)
		    MAPSET(&solv->recommendsmap, p);
		}
	    }
	}
      for (i = 1; i < pool->nsolvables; i++)
	{
	  if (solv->decisionmap[i] < 0)
	    continue;
	  if (solv->decisionmap[i] > 0 && solv->dontshowinstalledrecommended)
	    continue;
          if (MAPTST(&obsmap, i))
	    continue;
	  s = pool->solvables + i;
	  if (!MAPTST(&solv->recommendsmap, i))
	    {
	      if (!s->supplements)
		continue;
	      if (!pool_installable(pool, s))
		continue;
	      if (!solver_is_supplementing(solv, s))
		continue;
	    }
	  if (solv->dontinstallrecommended)
	    queue_push(&solv->recommendations, i);
	  else
	    queue_pushunique(&solv->recommendations, i);
	}
      /* we use MODE_SUGGEST here so that repo prio is ignored */
      policy_filter_unwanted(solv, &solv->recommendations, POLICY_MODE_SUGGEST);
    }

  /*
   * find suggested packages
   */
    
  if (1)
    {
      Id sug, *sugp, p, pp;

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
		    {
		      if (!solv->dontshowinstalledrecommended)
			{
			  FOR_PROVIDES(p, pp, sug)
			    if (solv->decisionmap[p] > 0)
			      MAPSET(&solv->suggestsmap, p);
			}
		      continue;	/* already fulfilled */
		    }
		  FOR_PROVIDES(p, pp, sug)
		    MAPSET(&solv->suggestsmap, p);
		}
	    }
	}
      for (i = 1; i < pool->nsolvables; i++)
	{
	  if (solv->decisionmap[i] < 0)
	    continue;
	  if (solv->decisionmap[i] > 0 && solv->dontshowinstalledrecommended)
	    continue;
          if (MAPTST(&obsmap, i))
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
      policy_filter_unwanted(solv, &solv->suggestions, POLICY_MODE_SUGGEST);
    }

  /* undo removedisabledconflicts */
  if (redoq.count)
    undo_removedisabledconflicts(solv, &redoq);
  queue_free(&redoq);
  
  /* undo job rule disabling */
  for (i = 0; i < disabledq.count; i++)
    enablerule(solv, solv->rules + disabledq.elements[i]);
  queue_free(&disabledq);
  map_free(&obsmap);
}


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
  Map installcandidatemap;
  Id how, what, select, name, weak, p, pp, d;
  Queue q;
  Solvable *s;
  Rule *r;
  int now, solve_start;
  int hasdupjob = 0;

  solve_start = sat_timems(0);

  /* log solver options */
  POOL_DEBUG(SAT_DEBUG_STATS, "solver started\n");
  POOL_DEBUG(SAT_DEBUG_STATS, "fixsystem=%d updatesystem=%d dosplitprovides=%d, noupdateprovide=%d noinfarchcheck=%d\n", solv->fixsystem, solv->updatesystem, solv->dosplitprovides, solv->noupdateprovide, solv->noinfarchcheck);
  POOL_DEBUG(SAT_DEBUG_STATS, "distupgrade=%d distupgrade_removeunsupported=%d\n", solv->distupgrade, solv->distupgrade_removeunsupported);
  POOL_DEBUG(SAT_DEBUG_STATS, "allowuninstall=%d, allowdowngrade=%d, allowarchchange=%d, allowvendorchange=%d\n", solv->allowuninstall, solv->allowdowngrade, solv->allowarchchange, solv->allowvendorchange);
  POOL_DEBUG(SAT_DEBUG_STATS, "promoteepoch=%d, allowvirtualconflicts=%d, allowselfconflicts=%d\n", pool->promoteepoch, solv->allowvirtualconflicts, solv->allowselfconflicts);
  POOL_DEBUG(SAT_DEBUG_STATS, "obsoleteusesprovides=%d, implicitobsoleteusesprovides=%d\n", solv->obsoleteusesprovides, solv->implicitobsoleteusesprovides);
  POOL_DEBUG(SAT_DEBUG_STATS, "dontinstallrecommended=%d, ignorealreadyrecommended=%d, dontshowinstalledrecommended=%d\n", solv->dontinstallrecommended, solv->ignorealreadyrecommended, solv->dontshowinstalledrecommended);

  /* create whatprovides if not already there */
  if (!pool->whatprovides)
    pool_createwhatprovides(pool);

  /* create obsolete index */
  policy_create_obsolete_index(solv);

  /* remember job */
  queue_free(&solv->job);
  queue_clone(&solv->job, job);

  /*
   * create basic rule set of all involved packages
   * use addedmap bitmap to make sure we don't create rules twice
   */

  /* create noobsolete map if needed */
  for (i = 0; i < job->count; i += 2)
    {
      how = job->elements[i] & ~SOLVER_WEAK;
      if ((how & SOLVER_JOBMASK) != SOLVER_NOOBSOLETES)
	continue;
      what = job->elements[i + 1];
      select = how & SOLVER_SELECTMASK;
      if (!solv->noobsoletes.size)
	map_init(&solv->noobsoletes, pool->nsolvables);
      FOR_JOB_SELECT(p, pp, select, what)
        MAPSET(&solv->noobsoletes, p);
    }

  map_init(&addedmap, pool->nsolvables);
  MAPSET(&addedmap, SYSTEMSOLVABLE);

  map_init(&installcandidatemap, pool->nsolvables);
  queue_init(&q);

  now = sat_timems(0);
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

  /*
   * create rules for all packages involved in the job
   * (to be installed or removed)
   */
    
  POOL_DEBUG(SAT_DEBUG_SCHUBI, "*** create rpm rules for packages involved with a job ***\n");
  oldnrules = solv->nrules;
  for (i = 0; i < job->count; i += 2)
    {
      how = job->elements[i];
      what = job->elements[i + 1];
      select = how & SOLVER_SELECTMASK;

      switch (how & SOLVER_JOBMASK)
	{
	case SOLVER_INSTALL:
	  FOR_JOB_SELECT(p, pp, select, what)
	    {
	      MAPSET(&installcandidatemap, p);
	      addrpmrulesforsolvable(solv, pool->solvables + p, &addedmap);
	    }
	  break;
	case SOLVER_DISTUPGRADE:
	  if (!solv->distupgrade)
	    hasdupjob = 1;
	  break;
	default:
	  break;
	}
    }
  POOL_DEBUG(SAT_DEBUG_STATS, "added %d rpm rules for packages involved in a job\n", solv->nrules - oldnrules);

  POOL_DEBUG(SAT_DEBUG_SCHUBI, "*** create rpm rules for recommended/suggested packages ***\n");

  oldnrules = solv->nrules;
    
    /*
     * add rules for suggests, enhances
     */
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
      POOL_DEBUG(SAT_DEBUG_STATS, "%d of %d installable solvables considered for solving\n", possible, installable);
    }

  /*
   * first pass done, we now have all the rpm rules we need.
   * unify existing rules before going over all job rules and
   * policy rules.
   * at this point the system is always solvable,
   * as an empty system (remove all packages) is a valid solution
   */

  unifyrules(solv);	                          /* remove duplicate rpm rules */

  solv->rpmrules_end = solv->nrules;              /* mark end of rpm rules */

  POOL_DEBUG(SAT_DEBUG_STATS, "rpm rule memory usage: %d K\n", solv->nrules * (int)sizeof(Rule) / 1024);
  POOL_DEBUG(SAT_DEBUG_STATS, "rpm rule creation took %d ms\n", sat_timems(now));

  /* create dup maps if needed. We need the maps early to create our
   * update rules */
  if (hasdupjob)
    createdupmaps(solv, job);

  /*
   * create feature rules
   * 
   * foreach installed:
   *   create assertion (keep installed, if no update available)
   *   or
   *   create update rule (A|update1(A)|update2(A)|...)
   * 
   * those are used later on to keep a version of the installed packages in
   * best effort mode
   */
    
  POOL_DEBUG(SAT_DEBUG_SCHUBI, "*** Add feature rules ***\n");
  solv->featurerules = solv->nrules;              /* mark start of feature rules */
  if (installed)
    {
	/* foreach possibly installed solvable */
      for (i = installed->start, s = pool->solvables + i; i < installed->end; i++, s++)
	{
	  if (s->repo != installed)
	    {
	      addrule(solv, 0, 0);	/* create dummy rule */
	      continue;
	    }
	  addupdaterule(solv, s, 1);    /* allow s to be updated */
	}
	/*
	 * assert one rule per installed solvable,
	 * either an assertion (A)
	 * or a possible update (A|update1(A)|update2(A)|...)
	 */
      assert(solv->nrules - solv->featurerules == installed->end - installed->start);
    }
  solv->featurerules_end = solv->nrules;

    /*
     * Add update rules for installed solvables
     * 
     * almost identical to feature rules
     * except that downgrades/archchanges/vendorchanges are not allowed
     */
    
  POOL_DEBUG(SAT_DEBUG_SCHUBI, "*** Add update rules ***\n");
  solv->updaterules = solv->nrules;

  if (installed)
    { /* foreach installed solvables */
      /* we create all update rules, but disable some later on depending on the job */
      for (i = installed->start, s = pool->solvables + i; i < installed->end; i++, s++)
	{
	  Rule *sr;

	  if (s->repo != installed)
	    {
	      addrule(solv, 0, 0);	/* create dummy rule */
	      continue;
	    }
	  addupdaterule(solv, s, 0);	/* allowall = 0: downgrades not allowed */
	    /*
	     * check for and remove duplicate
	     */
	  r = solv->rules + solv->nrules - 1;           /* r: update rule */
	  sr = r - (installed->end - installed->start); /* sr: feature rule */
	  /* it's orphaned if there is no feature rule or the feature rule
           * consists just of the installed package */
	  if (!sr->p || (sr->p == i && !sr->d && !sr->w2))
	    queue_push(&solv->orphaned, i);
          if (!r->p)
	    {
	      assert(solv->distupgrade && !sr->p);
	      continue;
	    }
	  if (!unifyrules_sortcmp(r, sr, pool))
	    {
	      /* identical rule, kill unneeded one */
	      if (solv->allowuninstall)
		{
		  /* keep feature rule, make it weak */
		  memset(r, 0, sizeof(*r));
		  queue_push(&solv->weakruleq, sr - solv->rules);
		}
	      else
		{
		  /* keep update rule */
		  memset(sr, 0, sizeof(*sr));
		}
	    }
	  else if (solv->allowuninstall)
	    {
	      /* make both feature and update rule weak */
	      queue_push(&solv->weakruleq, r - solv->rules);
	      queue_push(&solv->weakruleq, sr - solv->rules);
	    }
	  else
	    disablerule(solv, sr);
	}
      /* consistency check: we added a rule for _every_ installed solvable */
      assert(solv->nrules - solv->updaterules == installed->end - installed->start);
    }
  solv->updaterules_end = solv->nrules;


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
      weak = how & SOLVER_WEAK;
      select = how & SOLVER_SELECTMASK;
      switch (how & SOLVER_JOBMASK)
	{
	case SOLVER_INSTALL:
	  POOL_DEBUG(SAT_DEBUG_JOB, "job: %sinstall %s\n", weak ? "weak " : "", solver_select2str(solv, select, what));
	  if (select == SOLVER_SOLVABLE)
	    {
	      p = what;
	      d = 0;
	    }
	  else
	    {
	      queue_empty(&q);
	      FOR_JOB_SELECT(p, pp, select, what)
		queue_push(&q, p);
	      if (!q.count)
		{
		  /* no candidate found, make this an impossible rule */
		  queue_push(&q, -SYSTEMSOLVABLE);
		}
	      p = queue_shift(&q);	/* get first candidate */
	      d = !q.count ? 0 : pool_queuetowhatprovides(pool, &q);	/* internalize */
	    }
	  addrule(solv, p, d);		/* add install rule */
	  queue_push(&solv->ruletojob, i);
	  if (weak)
	    queue_push(&solv->weakruleq, solv->nrules - 1);
	  break;
	case SOLVER_ERASE:
	  POOL_DEBUG(SAT_DEBUG_JOB, "job: %serase %s\n", weak ? "weak " : "", solver_select2str(solv, select, what));
          if (select == SOLVER_SOLVABLE && solv->installed && pool->solvables[what].repo == solv->installed)
	    {
	      /* special case for "erase a specific solvable": we also
               * erase all other solvables with that name, so that they
               * don't get picked up as replacement */
	      name = pool->solvables[what].name;
	      FOR_PROVIDES(p, pp, name)
		{
		  if (p == what)
		    continue;
		  s = pool->solvables + p;
		  if (s->name != name)
		    continue;
		  /* keep other versions installed */
		  if (s->repo == solv->installed)
		    continue;
		  /* keep installcandidates of other jobs */
		  if (MAPTST(&installcandidatemap, p))
		    continue;
		  addrule(solv, -p, 0);			/* remove by Id */
		  queue_push(&solv->ruletojob, i);
		  if (weak)
		    queue_push(&solv->weakruleq, solv->nrules - 1);
		}
	    }
	  FOR_JOB_SELECT(p, pp, select, what)
	    {
	      addrule(solv, -p, 0);
	      queue_push(&solv->ruletojob, i);
	      if (weak)
		queue_push(&solv->weakruleq, solv->nrules - 1);
	    }
	  break;

	case SOLVER_UPDATE:
	  POOL_DEBUG(SAT_DEBUG_JOB, "job: %supdate %s\n", weak ? "weak " : "", solver_select2str(solv, select, what));
	  FOR_JOB_SELECT(p, pp, select, what)
	    {
	      s = pool->solvables + p;
	      if (!solv->installed || s->repo != solv->installed)
		continue;
	      if (!solv->updatemap.size)
		map_init(&solv->updatemap, pool->nsolvables);
	      MAPSET(&solv->updatemap, p);
	    }
	  break;
	case SOLVER_WEAKENDEPS:
	  POOL_DEBUG(SAT_DEBUG_JOB, "job: %sweaken deps %s\n", weak ? "weak " : "", solver_select2str(solv, select, what));
	  if (select != SOLVER_SOLVABLE)
	    break;
	  s = pool->solvables + what;
	  weaken_solvable_deps(solv, what);
	  break;
	case SOLVER_NOOBSOLETES:
	  POOL_DEBUG(SAT_DEBUG_JOB, "job: %sno obsolete %s\n", weak ? "weak " : "", solver_select2str(solv, select, what));
	  break;
	case SOLVER_LOCK:
	  POOL_DEBUG(SAT_DEBUG_JOB, "job: %slock %s\n", weak ? "weak " : "", solver_select2str(solv, select, what));
	  FOR_JOB_SELECT(p, pp, select, what)
	    {
	      s = pool->solvables + p;
	      if (installed && s->repo == installed)
		addrule(solv, p, 0);
	      else
		addrule(solv, -p, 0);
	      queue_push(&solv->ruletojob, i);
	      if (weak)
		queue_push(&solv->weakruleq, solv->nrules - 1);
	    }
	  break;
	case SOLVER_DISTUPGRADE:
	  POOL_DEBUG(SAT_DEBUG_JOB, "job: distupgrade repo #%d\n", what);
	  break;
	default:
	  POOL_DEBUG(SAT_DEBUG_JOB, "job: unknown job\n");
	  break;
	}
	
	/*
	 * debug
	 */
	
      IF_POOLDEBUG (SAT_DEBUG_JOB)
	{
	  int j;
	  if (solv->nrules == oldnrules)
	    POOL_DEBUG(SAT_DEBUG_JOB, " - no rule created\n");
	  for (j = oldnrules; j < solv->nrules; j++)
	    {
	      POOL_DEBUG(SAT_DEBUG_JOB, " - job ");
	      solver_printrule(solv, SAT_DEBUG_JOB, solv->rules + j);
	    }
	}
    }
  assert(solv->ruletojob.count == solv->nrules - solv->jobrules);
  solv->jobrules_end = solv->nrules;

  /* now create infarch and dup rules */
  if (!solv->noinfarchcheck)
    addinfarchrules(solv, &addedmap);
  else
    solv->infarchrules = solv->infarchrules_end = solv->nrules;

  if (hasdupjob)
    {
      addduprules(solv, &addedmap);
      freedupmaps(solv);	/* no longer needed */
    }
  else
    solv->duprules = solv->duprules_end = solv->nrules;


  /* all rules created
   * --------------------------------------------------------------
   * prepare for solving
   */
    
  /* free unneeded memory */
  map_free(&addedmap);
  map_free(&installcandidatemap);
  queue_free(&q);

  POOL_DEBUG(SAT_DEBUG_STATS, "%d rpm rules, %d job rules, %d infarch rules, %d dup rules\n", solv->rpmrules_end - 1, solv->jobrules_end - solv->jobrules, solv->infarchrules_end - solv->infarchrules, solv->duprules_end - solv->duprules);

  /* create weak map */
  map_init(&solv->weakrulemap, solv->nrules);
  for (i = 0; i < solv->weakruleq.count; i++)
    {
      p = solv->weakruleq.elements[i];
      MAPSET(&solv->weakrulemap, p);
    }

  /* all new rules are learnt after this point */
  solv->learntrules = solv->nrules;

  /* create watches chains */
  makewatches(solv);

  /* create assertion index. it is only used to speed up
   * makeruledecsions() a bit */
  for (i = 1, r = solv->rules + i; i < solv->nrules; i++, r++)
    if (r->p && !r->w2 && (r->d == 0 || r->d == -1))
      queue_push(&solv->ruleassertions, i);

  /* disable update rules that conflict with our job */
  disablepolicyrules(solv, job);

  /* make decisions based on job/update assertions */
  makeruledecisions(solv);
  POOL_DEBUG(SAT_DEBUG_STATS, "problems so far: %d\n", solv->problems.count);

  /*
   * ********************************************
   * solve!
   * ********************************************
   */
    
  now = sat_timems(0);
  run_solver(solv, 1, solv->dontinstallrecommended ? 0 : 1);
  POOL_DEBUG(SAT_DEBUG_STATS, "solver took %d ms\n", sat_timems(now));

  /*
   * calculate recommended/suggested packages
   */
  findrecommendedsuggested(solv);

  /*
   * prepare solution queue if there were problems
   */
  prepare_solutions(solv);

  /*
   * finally prepare transaction info
   */
  solver_create_transaction(solv);

  POOL_DEBUG(SAT_DEBUG_STATS, "final solver statistics: %d problems, %d learned rules, %d unsolvable\n", solv->problems.count / 2, solv->stats_learned, solv->stats_unsolvable);
  POOL_DEBUG(SAT_DEBUG_STATS, "solver_solve took %d ms\n", sat_timems(solve_start));
}

/***********************************************************************/
/* disk usage computations */

/*-------------------------------------------------------------------
 * 
 * calculate DU changes
 */

void
solver_calc_duchanges(Solver *solv, DUChanges *mps, int nmps)
{
  Map installedmap;

  solver_create_state_maps(solv, &installedmap, 0);
  pool_calc_duchanges(solv->pool, &installedmap, mps, nmps);
  map_free(&installedmap);
}


/*-------------------------------------------------------------------
 * 
 * calculate changes in install size
 */

int
solver_calc_installsizechange(Solver *solv)
{
  Map installedmap;
  int change;

  solver_create_state_maps(solv, &installedmap, 0);
  change = pool_calc_installsizechange(solv->pool, &installedmap);
  map_free(&installedmap);
  return change;
}

void
solver_trivial_installable(Solver *solv, Queue *pkgs, Queue *res)
{
  Map installedmap;
  solver_create_state_maps(solv, &installedmap, 0);
  pool_trivial_installable_noobsoletesmap(solv->pool, &installedmap, pkgs, res, solv->noobsoletes.size ? &solv->noobsoletes : 0);
  map_free(&installedmap);
}


#define FIND_INVOLVED_DEBUG 0
void
solver_find_involved(Solver *solv, Queue *installedq, Solvable *ts, Queue *q)
{
  Pool *pool = solv->pool;
  Map im;
  Map installedm;
  Solvable *s;
  Queue iq;
  Queue installedq_internal;
  Id tp, ip, p, pp, req, *reqp, sup, *supp;
  int i, count;

  tp = ts - pool->solvables;
  queue_init(&iq);
  queue_init(&installedq_internal);
  map_init(&im, pool->nsolvables);
  map_init(&installedm, pool->nsolvables);

  if (!installedq)
    {
      installedq = &installedq_internal;
      if (solv->installed)
	{
	  for (ip = solv->installed->start; ip < solv->installed->end; ip++)
	    {
	      s = pool->solvables + ip;
	      if (s->repo != solv->installed)
		continue;
	      queue_push(installedq, ip);
	    }
	}
    }
  for (i = 0; i < installedq->count; i++)
    {
      ip = installedq->elements[i];
      MAPSET(&installedm, ip);
      MAPSET(&im, ip);
    }

  queue_push(&iq, ts - pool->solvables);
  while (iq.count)
    {
      ip = queue_shift(&iq);
      if (!MAPTST(&im, ip))
	continue;
      if (!MAPTST(&installedm, ip))
	continue;
      MAPCLR(&im, ip);
      s = pool->solvables + ip;
#if FIND_INVOLVED_DEBUG
      printf("hello %s\n", solvable2str(pool, s));
#endif
      if (s->requires)
	{
	  reqp = s->repo->idarraydata + s->requires;
	  while ((req = *reqp++) != 0)
	    {
	      if (req == SOLVABLE_PREREQMARKER)
		continue;
	      /* count number of installed packages that match */
	      count = 0;
	      FOR_PROVIDES(p, pp, req)
		if (MAPTST(&installedm, p))
		  count++;
	      if (count > 1)
		continue;
	      FOR_PROVIDES(p, pp, req)
		{
		  if (MAPTST(&im, p))
		    {
#if FIND_INVOLVED_DEBUG
		      printf("%s requires %s\n", solvid2str(pool, ip), solvid2str(pool, p));
#endif
		      queue_push(&iq, p);
		    }
		}
	    }
	}
      if (s->recommends)
	{
	  reqp = s->repo->idarraydata + s->recommends;
	  while ((req = *reqp++) != 0)
	    {
	      count = 0;
	      FOR_PROVIDES(p, pp, req)
		if (MAPTST(&installedm, p))
		  count++;
	      if (count > 1)
		continue;
	      FOR_PROVIDES(p, pp, req)
		{
		  if (MAPTST(&im, p))
		    {
#if FIND_INVOLVED_DEBUG
		      printf("%s recommends %s\n", solvid2str(pool, ip), solvid2str(pool, p));
#endif
		      queue_push(&iq, p);
		    }
		}
	    }
	}
      if (!iq.count)
	{
	  /* supplements pass */
	  for (i = 0; i < installedq->count; i++)
	    {
	      ip = installedq->elements[i];
	      s = pool->solvables + ip;
	      if (!s->supplements)
		continue;
	      if (!MAPTST(&im, ip))
		continue;
	      supp = s->repo->idarraydata + s->supplements;
	      while ((sup = *supp++) != 0)
		if (!dep_possible(solv, sup, &im) && dep_possible(solv, sup, &installedm))
		  break;
	      /* no longer supplemented, also erase */
	      if (sup)
		{
#if FIND_INVOLVED_DEBUG
		  printf("%s supplemented\n", solvid2str(pool, ip));
#endif
		  queue_push(&iq, ip);
		}
	    }
	}
    }

  for (i = 0; i < installedq->count; i++)
    {
      ip = installedq->elements[i];
      if (MAPTST(&im, ip))
	queue_push(&iq, ip);
    }

  while (iq.count)
    {
      ip = queue_shift(&iq);
      if (!MAPTST(&installedm, ip))
	continue;
      s = pool->solvables + ip;
#if FIND_INVOLVED_DEBUG
      printf("bye %s\n", solvable2str(pool, s));
#endif
      if (s->requires)
	{
	  reqp = s->repo->idarraydata + s->requires;
	  while ((req = *reqp++) != 0)
	    {
	      FOR_PROVIDES(p, pp, req)
		{
		  if (!MAPTST(&im, p))
		    {
		      if (p == tp)
			continue;
#if FIND_INVOLVED_DEBUG
		      printf("%s requires %s\n", solvid2str(pool, ip), solvid2str(pool, p));
#endif
		      MAPSET(&im, p);
		      queue_push(&iq, p);
		    }
		}
	    }
	}
      if (s->recommends)
	{
	  reqp = s->repo->idarraydata + s->recommends;
	  while ((req = *reqp++) != 0)
	    {
	      FOR_PROVIDES(p, pp, req)
		{
		  if (!MAPTST(&im, p))
		    {
		      if (p == tp)
			continue;
#if FIND_INVOLVED_DEBUG
		      printf("%s recommends %s\n", solvid2str(pool, ip), solvid2str(pool, p));
#endif
		      MAPSET(&im, p);
		      queue_push(&iq, p);
		    }
		}
	    }
	}
      if (!iq.count)
	{
	  /* supplements pass */
	  for (i = 0; i < installedq->count; i++)
	    {
	      ip = installedq->elements[i];
	      if (ip == tp)
	        continue;
	      s = pool->solvables + ip;
	      if (!s->supplements)
		continue;
	      if (MAPTST(&im, ip))
		continue;
	      supp = s->repo->idarraydata + s->supplements;
	      while ((sup = *supp++) != 0)
		if (dep_possible(solv, sup, &im))
		  break;
	      if (sup)
		{
#if FIND_INVOLVED_DEBUG
		  printf("%s supplemented\n", solvid2str(pool, ip));
#endif
		  MAPSET(&im, ip);
		  queue_push(&iq, ip);
		}
	    }
	}
    }
    
  queue_free(&iq);

  /* convert map into result */
  for (i = 0; i < installedq->count; i++)
    {
      ip = installedq->elements[i];
      if (MAPTST(&im, ip))
	continue;
      if (ip == ts - pool->solvables)
	continue;
      queue_push(q, ip);
    }
  map_free(&im);
  map_free(&installedm);
  queue_free(&installedq_internal);
}


static void
addrpmruleinfo(Solver *solv, Id p, Id d, int type, Id dep)
{
  Pool *pool = solv->pool;
  Rule *r;
  Id w2, op, od, ow2;

  /* check if this creates the rule we're searching for */
  r = solv->rules + solv->ruleinfoq->elements[0];
  op = r->p;
  od = r->d < 0 ? -r->d - 1 : r->d;
  ow2 = 0;

  /* normalize */
  w2 = d > 0 ? 0 : d;
  if (p < 0 && d > 0 && (!pool->whatprovidesdata[d] || !pool->whatprovidesdata[d + 1]))
    {
      w2 = pool->whatprovidesdata[d];
      d = 0;

    }
  if (p > 0 && d < 0)		/* this hack is used for buddy deps */
    {
      w2 = p;
      p = d;
    }

  if (d > 0)
    {
      if (p != op && !od)
	return;
      if (d != od)
	{
	  Id *dp = pool->whatprovidesdata + d;
	  Id *odp = pool->whatprovidesdata + od;
	  while (*dp)
	    if (*dp++ != *odp++)
	      return;
	  if (*odp)
	    return;
	}
      w2 = 0;
      /* handle multiversion conflict rules */
      if (p < 0 && pool->whatprovidesdata[d] < 0)
	{
	  w2 = pool->whatprovidesdata[d];
	  /* XXX: free memory */
	}
    }
  else
    {
      if (od)
	return;
      ow2 = r->w2;
      if (p > w2)
	{
	  if (w2 != op || p != ow2)
	    return;
	}
      else
	{
	  if (p != op || w2 != ow2)
	    return;
	}
    }
  /* yep, rule matches. record info */
  queue_push(solv->ruleinfoq, type);
  if (type == SOLVER_RULE_RPM_SAME_NAME)
    {
      /* we normalize same name order */
      queue_push(solv->ruleinfoq, op < 0 ? -op : 0);
      queue_push(solv->ruleinfoq, ow2 < 0 ? -ow2 : 0);
    }
  else
    {
      queue_push(solv->ruleinfoq, p < 0 ? -p : 0);
      queue_push(solv->ruleinfoq, w2 < 0 ? -w2 : 0);
    }
  queue_push(solv->ruleinfoq, dep);
}

static int
solver_allruleinfos_cmp(const void *ap, const void *bp, void *dp)
{
  const Id *a = ap, *b = bp;
  int r;

  r = a[0] - b[0];
  if (r)
    return r;
  r = a[1] - b[1];
  if (r)
    return r;
  r = a[2] - b[2];
  if (r)
    return r;
  r = a[3] - b[3];
  if (r)
    return r;
  return 0;
}

int
solver_allruleinfos(Solver *solv, Id rid, Queue *rq)
{
  Pool *pool = solv->pool;
  Rule *r = solv->rules + rid;
  int i, j;

  queue_empty(rq);
  if (rid <= 0 || rid >= solv->rpmrules_end)
    {
      Id type, from, to, dep;
      type = solver_ruleinfo(solv, rid, &from, &to, &dep);
      queue_push(rq, type);
      queue_push(rq, from);
      queue_push(rq, to);
      queue_push(rq, dep);
      return 1;
    }
  if (r->p >= 0)
    return 0;
  queue_push(rq, rid);
  solv->ruleinfoq = rq;
  addrpmrulesforsolvable(solv, pool->solvables - r->p, 0);
  /* also try reverse direction for conflicts */
  if ((r->d == 0 || r->d == -1) && r->w2 < 0)
    addrpmrulesforsolvable(solv, pool->solvables - r->w2, 0);
  solv->ruleinfoq = 0;
  queue_shift(rq);
  /* now sort & unify em */
  if (!rq->count)
    return 0;
  sat_sort(rq->elements, rq->count / 4, 4 * sizeof(Id), solver_allruleinfos_cmp, 0);
  /* throw out identical entries */
  for (i = j = 0; i < rq->count; i += 4)
    {
      if (j)
	{
	  if (rq->elements[i] == rq->elements[j - 4] && 
	      rq->elements[i + 1] == rq->elements[j - 3] &&
	      rq->elements[i + 2] == rq->elements[j - 2] &&
	      rq->elements[i + 3] == rq->elements[j - 1])
	    continue;
	}
      rq->elements[j++] = rq->elements[i];
      rq->elements[j++] = rq->elements[i + 1];
      rq->elements[j++] = rq->elements[i + 2];
      rq->elements[j++] = rq->elements[i + 3];
    }
  rq->count = j;
  return j / 4;
}

SolverRuleinfo
solver_ruleinfo(Solver *solv, Id rid, Id *fromp, Id *top, Id *depp)
{
  Pool *pool = solv->pool;
  Rule *r = solv->rules + rid;
  SolverRuleinfo type = SOLVER_RULE_UNKNOWN;

  if (fromp)
    *fromp = 0;
  if (top)
    *top = 0;
  if (depp)
    *depp = 0;
  if (rid > 0 && rid < solv->rpmrules_end)
    {
      Queue rq;
      int i;

      if (r->p >= 0)
	return SOLVER_RULE_RPM;
      if (fromp)
	*fromp = -r->p;
      queue_init(&rq);
      queue_push(&rq, rid);
      solv->ruleinfoq = &rq;
      addrpmrulesforsolvable(solv, pool->solvables - r->p, 0);
      /* also try reverse direction for conflicts */
      if ((r->d == 0 || r->d == -1) && r->w2 < 0)
	addrpmrulesforsolvable(solv, pool->solvables - r->w2, 0);
      solv->ruleinfoq = 0;
      type = SOLVER_RULE_RPM;
      for (i = 1; i < rq.count; i += 4)
	{
	  Id qt, qo, qp, qd;
	  qt = rq.elements[i];
	  qp = rq.elements[i + 1];
	  qo = rq.elements[i + 2];
	  qd = rq.elements[i + 3];
	  if (type == SOLVER_RULE_RPM || type > qt)
	    {
	      type = qt;
	      if (fromp)
		*fromp = qp;
	      if (top)
		*top = qo;
	      if (depp)
		*depp = qd;
	    }
	}
      queue_free(&rq);
      return type;
    }
  if (rid >= solv->jobrules && rid < solv->jobrules_end)
    {
      Id jidx = solv->ruletojob.elements[rid - solv->jobrules];
      if (fromp)
	*fromp = jidx;
      if (top)
	*top = solv->job.elements[jidx];
      if (depp)
	*depp = solv->job.elements[jidx + 1];
      if ((r->d == 0 || r->d == -1) && r->w2 == 0 && r->p == -SYSTEMSOLVABLE && (solv->job.elements[jidx] & SOLVER_SELECTMASK) != SOLVER_SOLVABLE_ONE_OF)
	return SOLVER_RULE_JOB_NOTHING_PROVIDES_DEP;
      return SOLVER_RULE_JOB;
    }
  if (rid >= solv->updaterules && rid < solv->updaterules_end)
    {
      if (fromp)
	*fromp = solv->installed->start + (rid - solv->updaterules);
      return SOLVER_RULE_UPDATE;
    }
  if (rid >= solv->featurerules && rid < solv->featurerules_end)
    {
      if (fromp)
	*fromp = solv->installed->start + (rid - solv->featurerules);
      return SOLVER_RULE_FEATURE;
    }
  if (rid >= solv->duprules && rid < solv->duprules_end)
    {
      if (fromp)
	*fromp = -r->p;
      if (depp)
	*depp = pool->solvables[-r->p].name;
      return SOLVER_RULE_DISTUPGRADE;
    }
  if (rid >= solv->infarchrules && rid < solv->infarchrules_end)
    {
      if (fromp)
	*fromp = -r->p;
      if (depp)
	*depp = pool->solvables[-r->p].name;
      return SOLVER_RULE_INFARCH;
    }
  if (rid >= solv->learntrules)
    {
      return SOLVER_RULE_LEARNT;
    }
  return SOLVER_RULE_UNKNOWN;
}

/* obsolete function */
SolverRuleinfo
solver_problemruleinfo(Solver *solv, Queue *job, Id rid, Id *depp, Id *sourcep, Id *targetp)
{
  return solver_ruleinfo(solv, rid, sourcep, targetp, depp);
}

/* EOF */
