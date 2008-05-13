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
#include "solverdebug.h"

#define CODE10 0 /* set to '1' to enable patch atoms */

#define RULES_BLOCK 63

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
  Id p, *pp;
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

static Pool *unifyrules_sortcmp_data;

/*-------------------------------------------------------------------
 *
 * compare rules for unification sort
 *
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
  unifyrules_sortcmp_data = solv->pool;
  qsort(solv->rules + 1, solv->nrules - 1, sizeof(Rule), unifyrules_sortcmp);

  /* prune rules
   * i = unpruned
   * j = pruned
   */
  jr = 0;
  for (i = j = 1, ir = solv->rules + i; i < solv->nrules; i++, ir++)
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

#if 0
  if (n == 0 && !solv->rpmrules_end)
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
#endif

  if (n == 1 && p > d)
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
 * go through update and job rules and add direct assertions
 * to the decisionqueue. If we find a conflict, disable rules and
 * add them to problem queue.
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
	
      assert(solv->decisionq_why.elements[i]);
	
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
      if (!MAPTST(&solv->weakrulemap, ri))       /* skip non-weak */
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
      disablerule(solv, r);
    }
  
  POOL_DEBUG(SAT_DEBUG_SCHUBI, "----- makeruledecisions end; size decisionq: %d -----\n",solv->decisionq.count);
}


/*-------------------------------------------------------------------
 * enable/disable learnt rules 
 *
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

void
enableweakrules(Solver *solv)
{
  int i;
  Rule *r;

  for (i = 1, r = solv->rules + i; i < solv->learntrules; i++, r++)
    {
      if (r->d >= 0) /* skip non-direct literals */
	continue;
      if (!MAPTST(&solv->weakrulemap, i))
	continue;
      enablerule(solv, r);
    }
}


/* FIXME: bad code ahead, replace as soon as possible */
/* FIXME: should probably look at SOLVER_INSTALL_SOLVABLE_ONE_OF */

/*-------------------------------------------------------------------
 * disable update rules
 */

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
      how = job->elements[jobidx] & ~SOLVER_WEAK;
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
  for (i = solv->jobrules; i < solv->jobrules_end; i++)
    {
      r = solv->rules + i;
      if (r->d < 0)	/* disabled? */
	continue;
      j = solv->ruletojob.elements[i - solv->jobrules];
      if (j == lastjob)
	continue;
      lastjob = j;
      how = job->elements[j] & ~SOLVER_WEAK;
      what = job->elements[j + 1];
      switch(how)
	{
	case SOLVER_INSTALL_SOLVABLE:			/* install specific solvable */
	  s = pool->solvables + what;
	  if (solv->noobsoletes.size && MAPTST(&solv->noobsoletes, what))
	    break;
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
	      if (how == SOLVER_ERASE_SOLVABLE_NAME && !pool_match_nevr(pool, pool->solvables + p, what))
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
  if (jobidx != -1)
    {
      /* we just disabled job #jobidx. enable all update rules
       * that aren't disabled by the remaining job rules */
      how = job->elements[jobidx] & ~SOLVER_WEAK;
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
	      r = solv->rules + solv->updaterules + (p - installed->start);
	      if (r->d >= 0)
		continue;
	      enablerule(solv, r);
	      IF_POOLDEBUG (SAT_DEBUG_SOLUTIONS)
		{
		  POOL_DEBUG(SAT_DEBUG_SOLUTIONS, "@@@ re-enabling ");
		  solver_printrule(solv, SAT_DEBUG_SOLUTIONS, r);
		}
	    }
	  break;
	case SOLVER_ERASE_SOLVABLE:
	  s = pool->solvables + what;
	  if (s->repo != installed)
	    break;
	  if (MAPTST(&solv->noupdate, what - installed->start))
	    break;
	  r = solv->rules + solv->updaterules + (what - installed->start);
	  if (r->d >= 0)
	    break;
	  enablerule(solv, r);
	  IF_POOLDEBUG (SAT_DEBUG_SOLUTIONS)
	    {
	      POOL_DEBUG(SAT_DEBUG_SOLUTIONS, "@@@ re-enabling ");
	      solver_printrule(solv, SAT_DEBUG_SOLUTIONS, r);
	    }
	  break;
	case SOLVER_ERASE_SOLVABLE_NAME:                  /* remove by capability */
	case SOLVER_ERASE_SOLVABLE_PROVIDES:
	  FOR_PROVIDES(p, pp, what)
	    {
	      if (how == SOLVER_ERASE_SOLVABLE_NAME && !pool_match_nevr(pool, pool->solvables + p, what))
		continue;
	      if (pool->solvables[p].repo != installed)
		continue;
	      if (MAPTST(&solv->noupdate, p - installed->start))
		continue;
	      r = solv->rules + solv->updaterules + (p - installed->start);
	      if (r->d >= 0)
		continue;
	      enablerule(solv, r);
	      IF_POOLDEBUG (SAT_DEBUG_SOLUTIONS)
		{
		  POOL_DEBUG(SAT_DEBUG_SOLUTIONS, "@@@ re-enabling ");
		  solver_printrule(solv, SAT_DEBUG_SOLUTIONS, r);
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
      r = solv->rules + solv->updaterules + i;
      if (r->d >= 0 && MAPTST(&solv->noupdate, i))
        disablerule(solv, r);	/* was enabled, need to disable */
      r = solv->rules + solv->featurerules + i;
      if (r->d >= 0 && MAPTST(&solv->noupdate, i))
        disablerule(solv, r);	/* was enabled, need to disable */
    }
}

#if CODE10
/*-------------------------------------------------------------------
 * add patch atom requires
 */

static void
addpatchatomrequires(Solver *solv, Solvable *s, Id *dp, Queue *q, Map *m)
{
  Pool *pool = solv->pool;
  Id fre, *frep, p, *pp, ndp;
  Solvable *ps;
  Queue fq;
  Id qbuf[64];
  int i, used = 0;

  queue_init_buffer(&fq, qbuf, sizeof(qbuf)/sizeof(*qbuf));
  queue_push(&fq, -(s - pool->solvables));
  for (; *dp; dp++)
    queue_push(&fq, *dp);
  ndp = pool_queuetowhatprovides(pool, &fq);
  frep = s->repo->idarraydata + s->freshens;
  while ((fre = *frep++) != 0)
    {
      FOR_PROVIDES(p, pp, fre)
	{
	  ps = pool->solvables + p;
	  addrule(solv, -p, ndp);
	  used = 1;
	  if (!MAPTST(m, p))
	    queue_push(q, p);
	}
    }
  if (used)
    {
      for (i = 1; i < fq.count; i++)
	{
	  p = fq.elements[i];
	  if (!MAPTST(m, p))
	    queue_push(q, p);
	}
    }
  queue_free(&fq);
}
#endif


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
#if CODE10
  int patchatom;
#endif
    /* Id var and pointer for each dependency
     * (not used in parallel)
     */
  Id req, *reqp;
  Id con, *conp;
  Id obs, *obsp;
  Id rec, *recp;
  Id sug, *sugp;
    /* var and ptr for loops */
  Id p, *pp;
    /* ptr to 'whatprovides' */
  Id *dp;
    /* Id for current solvable 's' */
  Id n;

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

      n = queue_shift(&workq);             /* 'pop' next solvable to work on from queue */
      if (MAPTST(m, n))		           /* continue if already visited */
	continue;

      MAPSET(m, n);                        /* mark as visited */
      s = pool->solvables + n;	           /* s = Solvable in question */

      dontfix = 0;
      if (installed			   /* Installed system available */
	  && !solv->fixsystem		   /* NOT repair errors in rpm dependency graph */
	  && s->repo == installed)	   /* solvable is installed? */
      {
	dontfix = 1;		           /* dont care about broken rpm deps */
      }

      if (!dontfix
	  && s->arch != ARCH_SRC
	  && s->arch != ARCH_NOSRC
	  && !pool_installable(pool, s))
	{
	  POOL_DEBUG(SAT_DEBUG_RULE_CREATION, "package %s [%d] is not installable\n", solvable2str(pool, s), (Id)(s - pool->solvables));
	  addrule(solv, -n, 0);		   /* uninstallable */
	}
#if CODE10
      patchatom = 0;
      if (s->freshens && !s->supplements)
	{
	  const char *name = id2str(pool, s->name);
	  if (name[0] == 'a' && !strncmp(name, "atom:", 5))
	    patchatom = 1;
	}
#endif

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
	      dp = pool_whatprovides(pool, req);

	      if (*dp == SYSTEMSOLVABLE)	  /* always installed */
		continue;

#if CODE10
	      if (patchatom)
		{
		  addpatchatomrequires(solv, s, dp, &workq, m);
		  continue;
		}
#endif
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

	      /* descend the dependency tree
	         push all non-visited providers on the work queue */
	      for (; *dp; dp++)
		{
		  if (!MAPTST(m, *dp))
		    queue_push(&workq, *dp);
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
	  /* foreach conflicts of 's' */
	  while ((con = *conp++) != 0)
	    {
	      /* foreach providers of a conflict of 's' */
	      FOR_PROVIDES(p, pp, con)
		{
		  /* dontfix: dont care about conflicts with already installed packs */
		  if (dontfix && pool->solvables[p].repo == installed)
		    continue;
		  /* p == n: self conflict */
		  if (p == n && !solv->allowselfconflicts)
		    p = 0;	/* make it a negative assertion, aka 'uninstallable' */
                 /* rule: -n|-p: either solvable _or_ provider of conflict */
		  addrule(solv, -n, -p);
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
		      addrule(solv, -n, -p);
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
	      addrule(solv, -n, -p);
	    }
	}

      /*-----------------------------------------
       * add recommends to the work queue
       */
      if (s->recommends)
	{
	  recp = s->repo->idarraydata + s->recommends;
	  while ((rec = *recp++) != 0)
	    {
	      FOR_PROVIDES(p, pp, rec)
		if (!MAPTST(m, p))
		  queue_push(&workq, p);
	    }
	}
      if (s->suggests)
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
      if (i == pool->nsolvables)                 /* wrap i */
	i = 1;
      if (MAPTST(m, i))                          /* been there */
	continue;

      s = pool->solvables + i;
      if (!pool_installable(pool, s))            /* only look at installable ones */
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

	/* if nothing found, check for freshens
	 * (patterns use this)
	 */
      if (!sup && s->freshens)
	{
	  supp = s->repo->idarraydata + s->freshens;
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
	/* if notthing found, goto next solvables */
      if (!sup)
	continue;
      addrpmrulesforsolvable(solv, s, m);
      n = 0;
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
  Id d;
  Queue qs;
  Id qsbuf[64];

  POOL_DEBUG(SAT_DEBUG_SCHUBI, "-----  addupdaterule -----\n");

  queue_init_buffer(&qs, qsbuf, sizeof(qsbuf)/sizeof(*qsbuf));
    /* find update candidates for 's' */
  policy_findupdatepackages(solv, s, &qs, allow_all);
  if (qs.count == 0)		       /* no updaters found */
    d = 0;  /* assertion (keep installed) */
  else
    d = pool_queuetowhatprovides(pool, &qs);	/* intern computed queue */
  queue_free(&qs);
  addrule(solv, s - pool->solvables, d);	/* allow update of s */
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
 * add watches (for rule)
 * sets up watches for a single rule
 * 
 * see also makewatches()
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

/*-------------------------------------------------------------------
 * 
 * propagate
 *
 * make decision and propagate to all rules
 * 
 * Evaluate each term affected by the decision (linked through watches)
 * If we find unit rules we make new decisions based on them
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

	    /* 'pkg' was just decided
	     *   now find other literal watch, check clause
	     *  and advance on linked list
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
	     * The other literal is false or undecided
	     */
	    
          if (r->d)
	    {
	      /* not a binary clause, check if we need to move our watch.
	       * 
	       * search for a literal that is not other_watch and not false
	       * (true is also ok, in that case the rule is fulfilled)
	       */
	      if (r->p                                /* we have a 'p' */
		  && r->p != other_watch              /* which is not what we just checked */
		  && !DECISIONMAP_TRUE(-r->p))        /* and its not already decided 'negative' */
		{
		  p = r->p;                           /* we must get this to 'true' */
		}
	      else                                    /* go find a 'd' to make 'true' */
		{
		  /* foreach 'd' */
		  for (dp = pool->whatprovidesdata + r->d; (p = *dp++) != 0;)
		    if (p != other_watch              /* which is not what we just checked */
		        && !DECISIONMAP_TRUE(-p))     /* and its not already decided 'negative' */
		      break;
		}

		/*
		 * if p is free to watch, move watch to p
		 */
	      if (p)
		{
		  IF_POOLDEBUG (SAT_DEBUG_PROPAGATE)
		    {
		      if (p > 0)
			POOL_DEBUG(SAT_DEBUG_PROPAGATE, "    -> move w%d to %s\n", (pkg == r->w1 ? 1 : 2), solvable2str(pool, pool->solvables + p));
		      else
			POOL_DEBUG(SAT_DEBUG_PROPAGATE,"    -> move w%d to !%s\n", (pkg == r->w1 ? 1 : 2), solvable2str(pool, pool->solvables - p));
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
		
		/* !p */
		
	    } /* not binary */
	    
            /*
	     * unit clause found, set literal other_watch to TRUE
	     */

	  if (DECISIONMAP_TRUE(-other_watch))	   /* check if literal is FALSE */
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
	      Solvable *s = pool->solvables + (other_watch > 0 ? other_watch : -other_watch);
	      if (other_watch > 0)
		POOL_DEBUG(SAT_DEBUG_PROPAGATE, "    -> decided to install %s\n", solvable2str(pool, s));
	      else
		POOL_DEBUG(SAT_DEBUG_PROPAGATE, "    -> decided to conflict %s\n", solvable2str(pool, s));
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
      if (!why)			/* just in case, maye for SYSTEMSOLVABLE */
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
      /* disable last weak rule */
      solv->problems.count = oldproblemcount;
      solv->learnt_pool.count = oldlearntpoolcount;
      r = solv->rules + lastweak;
      POOL_DEBUG(SAT_DEBUG_UNSOLVABLE, "disabling ");
      solver_printruleclass(solv, SAT_DEBUG_UNSOLVABLE, r);
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
setpropagatelearn(Solver *solv, int level, Id decision, int disablerules)
{
  Pool *pool = solv->pool;
  Rule *r;
  Id p = 0, d = 0;
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
	  addwatches_rule(solv, r);
	}
      else
	{
	  /* learnt rule is an assertion */
          queue_push(&solv->ruleassertions, r - solv->rules);
	}
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
  queue_init(&solv->recommendations);
  queue_init(&solv->learnt_why);
  queue_init(&solv->learnt_pool);
  queue_init(&solv->branches);
  queue_init(&solv->covenantq);
  queue_init(&solv->weakruleq);
  queue_init(&solv->ruleassertions);

  map_init(&solv->recommendsmap, pool->nsolvables);
  map_init(&solv->suggestsmap, pool->nsolvables);
  map_init(&solv->noupdate, installed ? installed->end - installed->start : 0);
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
  queue_free(&solv->ruletojob);
  queue_free(&solv->decisionq);
  queue_free(&solv->decisionq_why);
  queue_free(&solv->learnt_why);
  queue_free(&solv->learnt_pool);
  queue_free(&solv->problems);
  queue_free(&solv->suggestions);
  queue_free(&solv->recommendations);
  queue_free(&solv->branches);
  queue_free(&solv->covenantq);
  queue_free(&solv->weakruleq);
  queue_free(&solv->ruleassertions);

  map_free(&solv->recommendsmap);
  map_free(&solv->suggestsmap);
  map_free(&solv->noupdate);
  map_free(&solv->weakrulemap);
  map_free(&solv->noobsoletes);

  sat_free(solv->decisionmap);
  sat_free(solv->rules);
  sat_free(solv->watches);
  sat_free(solv->obsoletes);
  sat_free(solv->obsoletes_data);
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
  Queue dq;         /* local decisionqueue */
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
	      /*
	       * Normal run (non-updating)
	       * Keep as many packages installed as possible
	       */
	      POOL_DEBUG(SAT_DEBUG_STATS, "installing old packages\n");
		
	      for (i = solv->installed->start; i < solv->installed->end; i++)
		{
		  s = pool->solvables + i;
		    
		    /* skip if not installed */
		  if (s->repo != solv->installed)
		    continue;
		    
		    /* skip if already decided */
		  if (solv->decisionmap[i] != 0)
		    continue;
		    
		  POOL_DEBUG(SAT_DEBUG_PROPAGATE, "keeping %s\n", solvable2str(pool, s));
		    
		  olevel = level;
		  level = setpropagatelearn(solv, level, i, disablerules);

		  if (level == 0)                /* unsolvable */
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
	    
	  POOL_DEBUG(SAT_DEBUG_STATS, "resolving update/feature rules\n");
	    
	  for (i = solv->installed->start, r = solv->rules + solv->updaterules; i < solv->installed->end; i++, r++)
	    {
	      Rule *rr;
	      Id d;
	      s = pool->solvables + i;
		
		/* skip if not installed (can't update) */
	      if (s->repo != solv->installed)
		continue;
		/* skip if already decided */
	      if (solv->decisionmap[i] > 0)
		continue;
		
		/* noupdate is set if a job is erasing the installed solvable or installing a specific version */
	      if (MAPTST(&solv->noupdate, i - solv->installed->start))
		continue;
		
	      queue_empty(&dq);
	      rr = r;
	      if (rr->d < 0)	/* disabled -> look at feature rule ? */
		rr -= solv->installed->end - solv->installed->start;
	      if (!rr->p)	/* identical to update rule? */
		rr = r;
	      d = (rr->d < 0) ? -rr->d - 1 : rr->d;
	      if (d == 0)
		{
		  if (!rr->w2 || solv->decisionmap[rr->w2] > 0)
		    continue;
		    /* decide w2 if yet undecided */
		  if (solv->decisionmap[rr->w2] == 0)
		    queue_push(&dq, rr->w2);
		}
	      else
		{
		  dp = pool->whatprovidesdata + d;
		  while ((p = *dp++) != 0)
		    {
		      if (solv->decisionmap[p] > 0)
			break;
			/* decide p if yet undecided */
		      if (solv->decisionmap[p] == 0)
			queue_push(&dq, p);
		    }
		  if (p)
		    continue;
		}
	      if (!dq.count && solv->decisionmap[i] != 0)
		continue;
	      olevel = level;
	      /* FIXME: i is handled a bit different because we do not want
	       * to have it pruned just because it is not recommened.
	       * we should not prune installed packages instead
	       */
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

      if (doweak)
	{
	  int qcount;

	  POOL_DEBUG(SAT_DEBUG_STATS, "installing recommended packages\n");
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
	  if (dq.count)
	    {
	      if (dq.count > 1)
	        policy_filter_unwanted(solv, &dq, 0, POLICY_MODE_RECOMMEND);
	      p = dq.elements[0];
	      POOL_DEBUG(SAT_DEBUG_STATS, "installing recommended %s\n", solvable2str(pool, pool->solvables + p));
	      queue_push(&solv->recommendations, p);
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
  POOL_DEBUG(SAT_DEBUG_STATS, "solver statistics: %d learned rules, %d unsolvable\n", solv->stats_learned, solv->stats_unsolvable);

  POOL_DEBUG(SAT_DEBUG_STATS, "done solving.\n\n");
  queue_free(&dq);
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
refine_suggestion(Solver *solv, Queue *job, Id *problem, Id sug, Queue *refined)
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
  queue_init(&disabled);
  queue_empty(refined);
  queue_push(refined, sug);

  /* re-enable all problem rules with the exception of "sug"(gestion) */
  revert(solv, 1);
  reset_solver(solv);

  for (i = 0; problem[i]; i++)
    if (problem[i] != sug)
      enableproblem(solv, problem[i]);

  if (sug < 0)
    disableupdaterules(solv, job, -(sug + 1));
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
	    njob++;
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
	    disableupdaterules(solv, job, -(v + 1));
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
  /* disable problem rules again */

  /* FIXME! */
  for (i = 0; problem[i]; i++)
    enableproblem(solv, problem[i]);
  disableupdaterules(solv, job, -1);

  /* disable problem rules again */
  for (i = 0; problem[i]; i++)
    disableproblem(solv, problem[i]);
  POOL_DEBUG(SAT_DEBUG_SOLUTIONS, "refine_suggestion end\n");
}


/*-------------------------------------------------------------------
 * sorting helper for problems
 */

static int
problems_sortcmp(const void *ap, const void *bp)
{
  Id a = *(Id *)ap, b = *(Id *)bp;
  if (a < 0 && b > 0)
    return 1;
  if (a > 0 && b < 0)
    return -1;
  return a - b;
}


/*-------------------------------------------------------------------
 * sort problems
 */

static void
problems_sort(Solver *solv)
{
  int i, j;
  if (!solv->problems.count)
    return;
  for (i = j = 1; i < solv->problems.count; i++)
    {
      if (!solv->problems.elements[i])
	{
	  if (i > j + 1)
	    qsort(solv->problems.elements + j, i - j, sizeof(Id), problems_sortcmp);
	  if (++i == solv->problems.count)
	    break;
	  j = i + 1;
	}
    }
}


/*-------------------------------------------------------------------
 * convert problems to solutions
 */

static void
problems_to_solutions(Solver *solv, Queue *job)
{
  Pool *pool = solv->pool;
  Queue problems;
  Queue solution;
  Queue solutions;
  Id *problem;
  Id why;
  int i, j, nsol;

  if (!solv->problems.count)
    return;
  problems_sort(solv);
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

      nsol = 0;
      for (j = 0; j < solution.count; j++)
	{
	  why = solution.elements[j];
	  /* must be either job descriptor or update rule */
	  assert(why < 0 || (why >= solv->updaterules && why < solv->updaterules_end));
#if 0
	  solver_printproblem(solv, why);
#endif
	  if (why < 0)
	    {
	      /* job descriptor */
	      queue_push(&solutions, 0);
	      queue_push(&solutions, -why);
	    }
	  else
	    {
	      /* update rule, find replacement package */
	      Id p, d, *dp, rp = 0;
	      Rule *rr;
	      p = solv->installed->start + (why - solv->updaterules);
	      if (solv->decisionmap[p] > 0)
		continue;	/* false alarm, turned out we can keep the package */
	      rr = solv->rules + solv->featurerules + (why - solv->updaterules);
	      if (!rr->p)
		rr = solv->rules + why;
	      if (rr->w2)
		{
		  d = rr->d < 0 ? -rr->d - 1 : rr->d;
		  if (!d)
		    {
		      if (solv->decisionmap[rr->w2] > 0 && pool->solvables[rr->w2].repo != solv->installed)
			rp = rr->w2;
		    }
		  else
		    {
		      for (dp = pool->whatprovidesdata + d; *dp; dp++)
			{
			  if (solv->decisionmap[*dp] > 0 && pool->solvables[*dp].repo != solv->installed)
			    {
			      rp = *dp;
			      break;
			    }
			}
	 	    }
		}
	      queue_push(&solutions, p);
	      queue_push(&solutions, rp);
	    }
	  nsol++;
	}
      /* mark end of this solution */
      if (nsol)
	{
	  queue_push(&solutions, 0);
	  queue_push(&solutions, 0);
	}
      else
	{
	  POOL_DEBUG(SAT_DEBUG_SOLUTIONS, "Oops, everything was fine?\n");
	}
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


/*-------------------------------------------------------------------
 * 
 * problem iterator
 * 
 * advance to next problem
 */

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


/*-------------------------------------------------------------------
 * 
 * solution iterator
 */

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


/*-------------------------------------------------------------------
 * 
 * solution element iterator
 */

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


/*-------------------------------------------------------------------
 * 
 * Retrieve information about a problematic rule
 *
 * this is basically the reverse of addrpmrulesforsolvable
 */

SolverProbleminfo
solver_problemruleinfo(Solver *solv, Queue *job, Id rid, Id *depp, Id *sourcep, Id *targetp)
{
  Pool *pool = solv->pool;
  Repo *installed = solv->installed;
  Rule *r;
  Solvable *s;
  int dontfix = 0;
  Id p, d, *pp, req, *reqp, con, *conp, obs, *obsp, *dp;

  assert(rid > 0);
  if (rid >= solv->jobrules && rid < solv->jobrules_end)
    {

      r = solv->rules + rid;
      p = solv->ruletojob.elements[rid - solv->jobrules];
      *depp = job->elements[p + 1];
      *sourcep = p;
      *targetp = job->elements[p];
      d = r->d < 0 ? -r->d - 1 : r->d;
      if (d == 0 && r->w2 == 0 && r->p == -SYSTEMSOLVABLE && job->elements[p] != SOLVER_INSTALL_SOLVABLE_ONE_OF)
	return SOLVER_PROBLEM_JOB_NOTHING_PROVIDES_DEP;
      return SOLVER_PROBLEM_JOB_RULE;
    }
  if (rid >= solv->updaterules && rid < solv->updaterules_end)
    {
      *depp = 0;
      *sourcep = solv->installed->start + (rid - solv->updaterules);
      *targetp = 0;
      return SOLVER_PROBLEM_UPDATE_RULE;
    }
  assert(rid < solv->rpmrules_end);
  r = solv->rules + rid;
  assert(r->p < 0);
  d = r->d < 0 ? -r->d - 1 : r->d;
  if (d == 0 && r->w2 == 0)
    {
      /* a rpm rule assertion */
      s = pool->solvables - r->p;
      if (installed && !solv->fixsystem && s->repo == installed)
	dontfix = 1;
      assert(!dontfix);	/* dontfix packages never have a neg assertion */
      *sourcep = -r->p;
      *targetp = 0;
      /* see why the package is not installable */
      if (s->arch != ARCH_SRC && s->arch != ARCH_NOSRC && !pool_installable(pool, s))
	{
	  *depp = 0;
	  return SOLVER_PROBLEM_NOT_INSTALLABLE;
	}
      /* check requires */
      if (s->requires)
	{
	  reqp = s->repo->idarraydata + s->requires;
	  while ((req = *reqp++) != 0)
	    {
	      if (req == SOLVABLE_PREREQMARKER)
		continue;
	      dp = pool_whatprovides(pool, req);
	      if (*dp == 0)
		break;
	    }
	  if (req)
	    {
	      *depp = req;
	      return SOLVER_PROBLEM_NOTHING_PROVIDES_DEP;
	    }
	}
      assert(!solv->allowselfconflicts);
      assert(s->conflicts);
      conp = s->repo->idarraydata + s->conflicts;
      while ((con = *conp++) != 0)
	FOR_PROVIDES(p, pp, con)
	  if (p == -r->p)
	    {
	      *depp = con;
	      return SOLVER_PROBLEM_SELF_CONFLICT;
	    }
      assert(0);
    }
  s = pool->solvables - r->p;
  if (installed && !solv->fixsystem && s->repo == installed)
    dontfix = 1;
  if (d == 0 && r->w2 < 0)
    {
      /* a package conflict */
      Solvable *s2 = pool->solvables - r->w2;
      int dontfix2 = 0;

      if (installed && !solv->fixsystem && s2->repo == installed)
	dontfix2 = 1;

      /* if both packages have the same name and at least one of them
       * is not installed, they conflict */
      if (s->name == s2->name && !(installed && s->repo == installed && s2->repo == installed))
	{
	  /* also check noobsoletes map */
	  if ((s->evr == s2->evr && s->arch == s2->arch) || !solv->noobsoletes.size
		|| ((!installed || s->repo != installed) && !MAPTST(&solv->noobsoletes, -r->p))
		|| ((!installed || s2->repo != installed) && !MAPTST(&solv->noobsoletes, -r->w2)))
	    {
	      *depp = 0;
	      *sourcep = -r->p;
	      *targetp = -r->w2;
	      return SOLVER_PROBLEM_SAME_NAME;
	    }
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
      if ((!installed || s->repo != installed) && s->obsoletes && !(solv->noobsoletes.size && MAPTST(&solv->noobsoletes, -r->p)))
	{
	  obsp = s->repo->idarraydata + s->obsoletes;
	  while ((obs = *obsp++) != 0)
	    {
	      FOR_PROVIDES(p, pp, obs)
		{
		  if (p != -r->w2)
		    continue;
		  if (!solv->obsoleteusesprovides && !pool_match_nevr(pool, pool->solvables + p, obs))
		    continue;
		  *depp = obs;
		  *sourcep = -r->p;
		  *targetp = p;
		  return SOLVER_PROBLEM_PACKAGE_OBSOLETES;
		}
	    }
	}
      if ((!installed || s2->repo != installed) && s2->obsoletes && !(solv->noobsoletes.size && MAPTST(&solv->noobsoletes, -r->w2)))
	{
	  obsp = s2->repo->idarraydata + s2->obsoletes;
	  while ((obs = *obsp++) != 0)
	    {
	      FOR_PROVIDES(p, pp, obs)
		{
		  if (p != -r->p)
		    continue;
		  if (!solv->obsoleteusesprovides && !pool_match_nevr(pool, pool->solvables + p, obs))
		    continue;
		  *depp = obs;
		  *sourcep = -r->w2;
		  *targetp = p;
		  return SOLVER_PROBLEM_PACKAGE_OBSOLETES;
		}
	    }
	}
      if (solv->implicitobsoleteusesprovides && (!installed || s->repo != installed) && !(solv->noobsoletes.size && MAPTST(&solv->noobsoletes, -r->p)))
	{
	  FOR_PROVIDES(p, pp, s->name)
	    {
	      if (p != -r->w2)
		continue;
	      *depp = s->name;
	      *sourcep = -r->p;
	      *targetp = p;
	      return SOLVER_PROBLEM_PACKAGE_OBSOLETES;
	    }
	}
      if (solv->implicitobsoleteusesprovides && (!installed || s2->repo != installed) && !(solv->noobsoletes.size && MAPTST(&solv->noobsoletes, -r->w2)))
	{
	  FOR_PROVIDES(p, pp, s2->name)
	    {
	      if (p != -r->p)
		continue;
	      *depp = s2->name;
	      *sourcep = -r->w2;
	      *targetp = p;
	      return SOLVER_PROBLEM_PACKAGE_OBSOLETES;
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
      if (d == 0)
	{
	  if (*dp == r->w2 && dp[1] == 0)
	    break;
	}
      else if (dp - pool->whatprovidesdata == d)
	break;
    }
  assert(req);
  *depp = req;
  *sourcep = -r->p;
  *targetp = 0;
  return SOLVER_PROBLEM_DEP_PROVIDERS_NOT_INSTALLABLE;
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
      else if (rid >= solv->jobrules && rid < solv->jobrules_end)
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
		  /* prefer assertions (XXX: bad idea?) */
		  *reqrp = rid;
		  reqassert = 1;
		}
	      if (!*reqrp)
		*reqrp = rid;
	      else if (solv->installed && r->p < 0 && solv->pool->solvables[-r->p].repo == solv->installed)
		{
		  /* prefer rules of installed packages */
		  Id op = *reqrp >= 0 ? solv->rules[*reqrp].p : -*reqrp;
		  if (op <= 0 || solv->pool->solvables[op].repo != solv->installed)
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


/*-------------------------------------------------------------------
 * 
 * find problem rule
 *
 * search for a rule that describes the problem to the
 * user. A pretty hopeless task, actually. We currently
 * prefer simple requires.
 */

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


/*-------------------------------------------------------------------
 * 
 * create reverse obsoletes map for installed solvables
 *
 * for each installed solvable find which packages with *different* names
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
  solv->obsoletes = obsoletes = sat_calloc(installed->end - installed->start, sizeof(Id));
  for (i = 1; i < pool->nsolvables; i++)
    {
      s = pool->solvables + i;
      if (!s->obsoletes)
	continue;
      if (!pool_installable(pool, s))
	continue;
      obsp = s->repo->idarraydata + s->obsoletes;
      while ((obs = *obsp++) != 0)
	{
	  FOR_PROVIDES(p, pp, obs)
	    {
	      if (pool->solvables[p].repo != installed)
		continue;
	      if (pool->solvables[p].name == s->name)
		continue;
	      if (!solv->obsoleteusesprovides && !pool_match_nevr(pool, pool->solvables + p, obs))
		continue;
	      obsoletes[p - installed->start]++;
	    }
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
      if (!s->obsoletes)
	continue;
      if (!pool_installable(pool, s))
	continue;
      obsp = s->repo->idarraydata + s->obsoletes;
      while ((obs = *obsp++) != 0)
	{
	  FOR_PROVIDES(p, pp, obs)
	    {
	      if (pool->solvables[p].repo != installed)
		continue;
	      if (pool->solvables[p].name == s->name)
		continue;
	      if (!solv->obsoleteusesprovides && !pool_match_nevr(pool, pool->solvables + p, obs))
		continue;
	      p -= installed->start;
	      if (obsoletes_data[obsoletes[p]] != i)
		obsoletes_data[--obsoletes[p]] = i;
	    }
	}
    }
}


/*-------------------------------------------------------------------
 * 
 * remove disabled conflicts
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

  POOL_DEBUG(SAT_DEBUG_STATS, "removedisabledconflicts\n");
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
	  POOL_DEBUG(SAT_DEBUG_STATS, "removing conflict for package %s[%d]\n", solvable2str(pool, pool->solvables - p), -p);
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
	  POOL_DEBUG(SAT_DEBUG_STATS, "re-conflicting package %s[%d]\n", solvable2str(pool, pool->solvables - new), -new);
	  decisionmap[-new] = -1;
	  new = 0;
	  n = 0;	/* redo all rules */
	}
    }
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

  for (i = 1, r = solv->rules + i; i < solv->featurerules; i++, r++)
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
  Id how, what, weak, p, *pp, d;
  Queue q, redoq;
  Solvable *s;
  int goterase;
  Rule *r;

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

  /* create noobsolete map if needed */
  for (i = 0; i < job->count; i += 2)
    {
      how = job->elements[i] & ~SOLVER_WEAK;
      what = job->elements[i + 1];
      switch(how)
	{
	case SOLVER_NOOBSOLETES_SOLVABLE:
	case SOLVER_NOOBSOLETES_SOLVABLE_NAME:
	case SOLVER_NOOBSOLETES_SOLVABLE_PROVIDES:
	  if (!solv->noobsoletes.size)
	    map_init(&solv->noobsoletes, pool->nsolvables);
	  if (how == SOLVER_NOOBSOLETES_SOLVABLE)
	    {
	      MAPSET(&solv->noobsoletes, what);
	      break;
	    }
	  FOR_PROVIDES(p, pp, what)
	    {
	      if (how == SOLVER_NOOBSOLETES_SOLVABLE_NAME && !pool_match_nevr(pool, pool->solvables + p, what))
		continue;
	      MAPSET(&solv->noobsoletes, p);
	    }
	  break;
	default:
	  break;
	}
    }

  map_init(&addedmap, pool->nsolvables);
  queue_init(&q);

  /*
   * always install our system solvable
   */
  MAPSET(&addedmap, SYSTEMSOLVABLE);
  queue_push(&solv->decisionq, SYSTEMSOLVABLE);
  queue_push(&solv->decisionq_why, 0);
  solv->decisionmap[SYSTEMSOLVABLE] = 1; /* installed at level '1' */

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
      how = job->elements[i] & ~SOLVER_WEAK;
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
	      if (how == SOLVER_INSTALL_SOLVABLE_NAME && !pool_match_nevr(pool, pool->solvables + p, what))
		continue;
	      addrpmrulesforsolvable(solv, pool->solvables + p, &addedmap);
	    }
	  break;
	case SOLVER_INSTALL_SOLVABLE_UPDATE:
	  /* dont allow downgrade */
	  addrpmrulesforupdaters(solv, pool->solvables + what, &addedmap, 0);
	  break;
	case SOLVER_INSTALL_SOLVABLE_ONE_OF:
	  pp = pool->whatprovidesdata + what;
	  while ((p = *pp++) != 0)
	    addrpmrulesforsolvable(solv, pool->solvables + p, &addedmap);
	  break;
	}
    }
  POOL_DEBUG(SAT_DEBUG_STATS, "added %d rpm rules for packages involved in a job\n", solv->nrules - oldnrules);

  POOL_DEBUG(SAT_DEBUG_SCHUBI, "*** create rpm rules for recommended/suggested packages ***\n");

  oldnrules = solv->nrules;
    
    /*
     * add rules for suggests, [freshens,] enhances
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

  solv->directdecisions = solv->decisionq.count;
  POOL_DEBUG(SAT_DEBUG_STATS, "decisions so far: %d\n", solv->decisionq.count);

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
	/* foreach installed solvable */
      for (i = installed->start, s = pool->solvables + i; i < installed->end; i++, s++)
	{
	    /*
	     * patterns use this
	     */

	  if (s->freshens && !s->supplements)
	    {
	      const char *name = id2str(pool, s->name);
	      if (name[0] == 'a' && !strncmp(name, "atom:", 5))
		{
		  addrule(solv, 0, 0);
		  continue;
		}
	    }

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
     * except that downgrades are allowed
     */
    
  POOL_DEBUG(SAT_DEBUG_SCHUBI, "*** Add update rules ***\n");
  solv->updaterules = solv->nrules;
  if (installed)
    { /* foreach installed solvables */
      /* we create all update rules, but disable some later on depending on the job */
      for (i = installed->start, s = pool->solvables + i; i < installed->end; i++, s++)
	{
	  Rule *sr;

#if CODE10
	  /* no update rules for patch atoms */
	  if (s->freshens && !s->supplements)
	    {
	      const char *name = id2str(pool, s->name);
	      if (name[0] == 'a' && !strncmp(name, "atom:", 5))
		{
		  addrule(solv, 0, 0);
		  continue;
		}
	    }
#endif
	  if (s->repo != installed)
	    {
	      addrule(solv, 0, 0);	/* create dummy rule */
	      continue;
	    }
	  addupdaterule(solv, s, 0);	/* allowall = 0: downgrades allowed */
	    
	    /*
	     * check for and remove duplicate
	     */
	    
	  r = solv->rules + solv->nrules - 1;           /* r: update rule */
	  sr = r - (installed->end - installed->start); /* sr: feature rule */
	  unifyrules_sortcmp_data = pool;
	  if (!unifyrules_sortcmp(r, sr))
	    {
	      /* identical rule, kill unneeded rule */
	      if (solv->allowuninstall)
		{
		  /* keep feature rule */
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

      how = job->elements[i] & ~SOLVER_WEAK;
      weak = job->elements[i] & SOLVER_WEAK;
      what = job->elements[i + 1];
      switch(how)
	{
	case SOLVER_INSTALL_SOLVABLE:			/* install specific solvable */
	  s = pool->solvables + what;
	  POOL_DEBUG(SAT_DEBUG_JOB, "job: %sinstall solvable %s\n", weak ? "weak " : "", solvable2str(pool, s));
          addrule(solv, what, 0);			/* install by Id */
	  queue_push(&solv->ruletojob, i);
	  if (weak)
	    queue_push(&solv->weakruleq, solv->nrules - 1);
	  break;
	case SOLVER_ERASE_SOLVABLE:
	  s = pool->solvables + what;
	  POOL_DEBUG(SAT_DEBUG_JOB, "job: %serase solvable %s\n", weak ? "weak " : "", solvable2str(pool, s));
          addrule(solv, -what, 0);			/* remove by Id */
	  queue_push(&solv->ruletojob, i);
	  if (weak)
	    queue_push(&solv->weakruleq, solv->nrules - 1);
	  break;
	case SOLVER_INSTALL_SOLVABLE_NAME:		/* install by capability */
	case SOLVER_INSTALL_SOLVABLE_PROVIDES:
	  if (how == SOLVER_INSTALL_SOLVABLE_NAME)
	    POOL_DEBUG(SAT_DEBUG_JOB, "job: %sinstall name %s\n", weak ? "weak " : "", dep2str(pool, what));
	  if (how == SOLVER_INSTALL_SOLVABLE_PROVIDES)
	    POOL_DEBUG(SAT_DEBUG_JOB, "job: %sinstall provides %s\n", weak ? "weak " : "", dep2str(pool, what));
	  queue_empty(&q);
	  FOR_PROVIDES(p, pp, what)
	    {
              /* if by name, ensure that the name matches */
	      if (how == SOLVER_INSTALL_SOLVABLE_NAME && !pool_match_nevr(pool, pool->solvables + p, what))
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
	  if (weak)
	    queue_push(&solv->weakruleq, solv->nrules - 1);
	  break;
	case SOLVER_ERASE_SOLVABLE_NAME:                  /* remove by capability */
	case SOLVER_ERASE_SOLVABLE_PROVIDES:
	  if (how == SOLVER_ERASE_SOLVABLE_NAME)
	    POOL_DEBUG(SAT_DEBUG_JOB, "job: %serase name %s\n", weak ? "weak " : "", dep2str(pool, what));
	  if (how == SOLVER_ERASE_SOLVABLE_PROVIDES)
	    POOL_DEBUG(SAT_DEBUG_JOB, "job: %serase provides %s\n", weak ? "weak " : "", dep2str(pool, what));
	  FOR_PROVIDES(p, pp, what)
	    {
	      /* if by name, ensure that the name matches */
	      if (how == SOLVER_ERASE_SOLVABLE_NAME && !pool_match_nevr(pool, pool->solvables + p, what))
	        continue;
	      addrule(solv, -p, 0);  /* add 'remove' rule */
	      queue_push(&solv->ruletojob, i);
	      if (weak)
		queue_push(&solv->weakruleq, solv->nrules - 1);
	    }
	  break;
	case SOLVER_INSTALL_SOLVABLE_UPDATE:              /* find update for solvable */
	  s = pool->solvables + what;
	  POOL_DEBUG(SAT_DEBUG_JOB, "job: %supdate %s\n", weak ? "weak " : "", solvable2str(pool, s));
	  addupdaterule(solv, s, 0);
	  queue_push(&solv->ruletojob, i);
	  if (weak)
	    queue_push(&solv->weakruleq, solv->nrules - 1);
	  break;
	case SOLVER_INSTALL_SOLVABLE_ONE_OF:
	  POOL_DEBUG(SAT_DEBUG_JOB, "job: %sone of\n", weak ? "weak " : "");
	  for (pp = pool->whatprovidesdata + what; *pp; pp++)
	    POOL_DEBUG(SAT_DEBUG_JOB, "  %s\n", solvable2str(pool, pool->solvables + *pp));
	  addrule(solv, -SYSTEMSOLVABLE, what);
	  queue_push(&solv->ruletojob, i);
	  if (weak)
	    queue_push(&solv->weakruleq, solv->nrules - 1);
	  break;
	case SOLVER_WEAKEN_SOLVABLE_DEPS:
	  s = pool->solvables + what;
	  POOL_DEBUG(SAT_DEBUG_JOB, "job: weaken deps %s\n", solvable2str(pool, s));
	  weaken_solvable_deps(solv, what);
	  break;
	case SOLVER_NOOBSOLETES_SOLVABLE:
	  POOL_DEBUG(SAT_DEBUG_JOB, "job: no obsolete %s\n", solvable2str(pool, pool->solvables + what));
	  break;
	case SOLVER_NOOBSOLETES_SOLVABLE_NAME:
	  POOL_DEBUG(SAT_DEBUG_JOB, "job: no obsolete name %s\n", dep2str(pool, what));
	  break;
	case SOLVER_NOOBSOLETES_SOLVABLE_PROVIDES:
	  POOL_DEBUG(SAT_DEBUG_JOB, "job: no obsolete provides %s\n", dep2str(pool, what));
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

    /* all rules created
     * --------------------------------------------------------------
     * prepare for solving
     */
    
  /* free unneeded memory */
  map_free(&addedmap);
  queue_free(&q);

  /* create weak map */
  map_init(&solv->weakrulemap, solv->nrules);
  for (i = 0; i < solv->weakruleq.count; i++)
    {
      p = solv->weakruleq.elements[i];
      MAPSET(&solv->weakrulemap, p);
    }

  /* all new rules are learnt after this point */
  solv->learntrules = solv->nrules;

  /* create assertion index. it is only used to speed up
   * makeruledecsions() a bit */
  for (i = 1, r = solv->rules + i; i < solv->nrules; i++, r++)
    if (r->p && !r->w2 && (r->d == 0 || r->d == -1))
      queue_push(&solv->ruleassertions, i);

  /* disable update rules that conflict with our job */
  disableupdaterules(solv, job, -1);

  /* make decisions based on job/update assertions */
  makeruledecisions(solv);

  /* create watches chains */
  makewatches(solv);

  POOL_DEBUG(SAT_DEBUG_STATS, "problems so far: %d\n", solv->problems.count);

  /*
   * ********************************************
   * solve!
   * ********************************************
   */
    
  run_solver(solv, 1, solv->dontinstallrecommended ? 0 : 1);

  queue_init(&redoq);
  goterase = 0;
  /* disable all erase jobs (including weak "keep uninstalled" rules) */
  for (i = solv->jobrules, r = solv->rules + i; i < solv->learntrules; i++, r++)
    {
      if (r->d < 0)	/* disabled ? */
	continue;
      if (r->p > 0)	/* install job? */
	continue;
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
  if (redoq.count || solv->dontinstallrecommended || !solv->dontshowinstalledrecommended)
    {
      Id rec, *recp, p, *pp;

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
      policy_filter_unwanted(solv, &solv->recommendations, 0, POLICY_MODE_SUGGEST);
    }

  /*
   * find suggested packages
   */
    
  if (1)
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

  if (redoq.count)
    {
      /* restore decisionmap */
      for (i = 0; i < redoq.count; i += 2)
	solv->decisionmap[redoq.elements[i]] = redoq.elements[i + 1];
    }

    /*
     * if unsolvable, prepare solutions
     */

  if (solv->problems.count)
    {
      int recocount = solv->recommendations.count;
      solv->recommendations.count = 0;	/* so that revert() doesn't mess with it */
      queue_empty(&redoq);
      for (i = 0; i < solv->decisionq.count; i++)
	{
	  Id p = solv->decisionq.elements[i];
	  queue_push(&redoq, p);
	  queue_push(&redoq, solv->decisionq_why.elements[i]);
	  queue_push(&redoq, solv->decisionmap[p > 0 ? p : -p]);
	}
      problems_to_solutions(solv, job);
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
    }

  POOL_DEBUG(SAT_DEBUG_STATS, "final solver statistics: %d learned rules, %d unsolvable\n", solv->stats_learned, solv->stats_unsolvable);
  queue_free(&redoq);
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
  pool_calc_duchanges(solv->pool, solv->installed, &installedmap, mps, nmps);
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
  change = pool_calc_installsizechange(solv->pool, solv->installed, &installedmap);
  map_free(&installedmap);
  return change;
}

/* EOF */
