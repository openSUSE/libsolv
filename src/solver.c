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
#include "policy.h"
#include "solverdebug.h"

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
	  solver_disablerule(solv, r);
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
	solver_disableproblem(solv, v);
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
	  solver_disableproblem(solv, v);
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
	  solver_disableproblem(solv, v);
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
      solver_disableproblem(solv, v);
      if (v < 0)
	solver_reenablepolicyrules(solv, -(v + 1));
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
          solver_disablerule(solv, r);
	}
      else if (!why && r->d < 0)
	{
	  IF_POOLDEBUG (SAT_DEBUG_SOLUTIONS)
	    {
	      POOL_DEBUG(SAT_DEBUG_SOLUTIONS, "re-enabling ");
	      solver_printruleclass(solv, SAT_DEBUG_SOLUTIONS, r);
	    }
          solver_enablerule(solv, r);
	}
    }
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
 * solver_reset
 * 
 * reset the solver decisions to right after the rpm rules.
 * called after rules have been enabled/disabled
 */

void
solver_reset(Solver *solv)
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
  solv->recommendations.count = 0;
  solv->branches.count = 0;

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
      solver_disableproblem(solv, v);
      if (v < 0)
	solver_reenablepolicyrules(solv, -(v + 1));
      solver_reset(solv);
      return 1;
    }

  /* finish proof */
  queue_push(&solv->learnt_pool, 0);
  solv->problems.elements[oldproblemcount] = oldlearntpoolcount;

  if (disablerules)
    {
      for (i = oldproblemcount + 1; i < solv->problems.count - 1; i++)
        solver_disableproblem(solv, solv->problems.elements[i]);
      /* XXX: might want to enable all weak rules again */
      solver_reset(solv);
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
      r = solver_addrule(solv, p, d);
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
 * solver_run_sat
 *
 * all rules have been set up, now actually run the solver
 *
 */

void
solver_run_sat(Solver *solv, int disablerules, int doweak)
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
      solver_disablerule(solv, r);
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
    solver_enablerule(solv, solv->rules + disabledq.elements[i]);
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
  queue_init_clone(&solv->job, job);

  /*
   * create basic rule set of all involved packages
   * use addedmap bitmap to make sure we don't create rules twice
   */

  /* create noobsolete map if needed */
  for (i = 0; i < job->count; i += 2)
    {
      how = job->elements[i];
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
	solver_addrpmrulesforsolvable(solv, s, &addedmap);
      POOL_DEBUG(SAT_DEBUG_STATS, "added %d rpm rules for installed solvables\n", solv->nrules - oldnrules);
      POOL_DEBUG(SAT_DEBUG_SCHUBI, "*** create rpm rules for updaters of installed solvables ***\n");
      oldnrules = solv->nrules;
      FOR_REPO_SOLVABLES(installed, p, s)
	solver_addrpmrulesforupdaters(solv, s, &addedmap, 1);
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
	      solver_addrpmrulesforsolvable(solv, pool->solvables + p, &addedmap);
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

    
  /*
   * add rules for suggests, enhances
   */
  POOL_DEBUG(SAT_DEBUG_SCHUBI, "*** create rpm rules for suggested/enhanced packages ***\n");
  oldnrules = solv->nrules;
  solver_addrpmrulesforweak(solv, &addedmap);
  POOL_DEBUG(SAT_DEBUG_STATS, "added %d rpm rules because of weak dependencies\n", solv->nrules - oldnrules);

  /*
   * first pass done, we now have all the rpm rules we need.
   * unify existing rules before going over all job rules and
   * policy rules.
   * at this point the system is always solvable,
   * as an empty system (remove all packages) is a valid solution
   */

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

  solver_unifyrules(solv);                          /* remove duplicate rpm rules */
  solv->rpmrules_end = solv->nrules;              /* mark end of rpm rules */

  POOL_DEBUG(SAT_DEBUG_STATS, "rpm rule memory usage: %d K\n", solv->nrules * (int)sizeof(Rule) / 1024);
  POOL_DEBUG(SAT_DEBUG_STATS, "rpm rule creation took %d ms\n", sat_timems(now));

  /* create dup maps if needed. We need the maps early to create our
   * update rules */
  if (hasdupjob)
    solver_createdupmaps(solv);

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
	      solver_addrule(solv, 0, 0);	/* create dummy rule */
	      continue;
	    }
	  solver_addupdaterule(solv, s, 1);    /* allow s to be updated */
	}
      /* make sure we accounted for all rules */
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
	      solver_addrule(solv, 0, 0);	/* create dummy rule */
	      continue;
	    }
	  solver_addupdaterule(solv, s, 0);	/* allowall = 0: downgrades not allowed */
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
	  if (!solver_samerule(solv, r, sr))
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
	    solver_disablerule(solv, sr);
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
	  solver_addrule(solv, p, d);		/* add install rule */
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
		  solver_addrule(solv, -p, 0);			/* remove by Id */
		  queue_push(&solv->ruletojob, i);
		  if (weak)
		    queue_push(&solv->weakruleq, solv->nrules - 1);
		}
	    }
	  FOR_JOB_SELECT(p, pp, select, what)
	    {
	      solver_addrule(solv, -p, 0);
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
		solver_addrule(solv, p, 0);
	      else
		solver_addrule(solv, -p, 0);
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
    solver_addinfarchrules(solv, &addedmap);
  else
    solv->infarchrules = solv->infarchrules_end = solv->nrules;

  if (hasdupjob)
    {
      solver_addduprules(solv, &addedmap);
      solver_freedupmaps(solv);	/* no longer needed */
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
  solver_disablepolicyrules(solv);

  /* make decisions based on job/update assertions */
  makeruledecisions(solv);
  POOL_DEBUG(SAT_DEBUG_STATS, "problems so far: %d\n", solv->problems.count);

  /*
   * ********************************************
   * solve!
   * ********************************************
   */
    
  now = sat_timems(0);
  solver_run_sat(solv, 1, solv->dontinstallrecommended ? 0 : 1);
  POOL_DEBUG(SAT_DEBUG_STATS, "solver took %d ms\n", sat_timems(now));

  /*
   * calculate recommended/suggested packages
   */
  findrecommendedsuggested(solv);

  /*
   * prepare solution queue if there were problems
   */
  solver_prepare_solutions(solv);

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


#if 0
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
#endif

