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

#include "solver.h"
#include "bitmap.h"
#include "pool.h"
#include "util.h"
#include "evr.h"
#include "policy.h"

#define RULES_BLOCK 63


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
printruleelement(Solver *solv, Rule *r, Id v)
{
  Pool *pool = solv->pool;
  Solvable *s;
  if (v < 0)
    {
      s = pool->solvables + -v;
      printf("    !%s-%s.%s [%d]", id2str(pool, s->name), id2str(pool, s->evr), id2str(pool, s->arch), -v);
    }
  else
    {
      s = pool->solvables + v;
      printf("    %s-%s.%s [%d]", id2str(pool, s->name), id2str(pool, s->evr), id2str(pool, s->arch), v);
    }
  if (r)
    {
      if (r->w1 == v)
	printf(" (w1)");
      if (r->w2 == v)
	printf(" (w2)");
    }
  if (solv->decisionmap[s - pool->solvables] > 0)
    printf(" Install.level%d", solv->decisionmap[s - pool->solvables]);
  if (solv->decisionmap[s - pool->solvables] < 0)
    printf(" Conflict.level%d", -solv->decisionmap[s - pool->solvables]);
  if (r && r->w1 == 0)
    printf(" (disabled)");
  printf("\n");
}


/*
 * print rule
 */

static void
printrule(Solver *solv, Rule *r)
{
  int i;
  Id v;

  if (r >= solv->rules && r < solv->rules + solv->nrules)   /* r is a solver rule */
    printf("Rule #%d:\n", (int)(r - solv->rules));
  else
    printf("Rule:\n");		       /* r is any rule */
  for (i = 0; ; i++)
    {
      if (i == 0)
	v = r->p;
      else if (r->d == ID_NULL)
	{
	  if (i == 2)
	    break;
	  v = r->w2;
	}
      else
	v = solv->pool->whatprovidesdata[r->d + i - 1];
      if (v == ID_NULL)
	break;
      printruleelement(solv, r, v);
    }
  printf("    next: %d %d\n", r->n1, r->n2);
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
  int i, j;
  Rule *ir, *jr;

  if (solv->nrules <= 1)	       /* nothing to unify */
    return;

  if (solv->pool->verbose > 3) 
      printf ("----- unifyrules -----\n");

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
  if (solv->pool->verbose) printf("pruned rules from %d to %d\n", solv->nrules, j);

  /* adapt rule buffer */
  solv->rules = (Rule *)xrealloc(solv->rules, ((solv->nrules + RULES_BLOCK) & ~RULES_BLOCK) * sizeof(Rule));
  solv->nrules = j;
#if 1
  if (solv->pool->verbose)
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
      printf("  binary: %d\n", binr);
      printf("  normal: %d, %d literals\n", solv->nrules - 1 - binr, lits);
    }
#endif
  if (solv->pool->verbose > 3) 
      printf ("----- unifyrules end -----\n");  
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
 *   Conflicts:  p < 0, d < 0   (-A|-B)         either p (conflict issuer) or d (conflict provider)
 *   ?           p > 0, d < 0   (A|-B)
 *   No-op ?:    p = 0, d = 0   (null)          (used as policy rule placeholder)
 *
 * always returns a rule for non-rpm rules
 */

static Rule *
addrule(Solver *solv, Id p, Id d)
{
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
      for (dp = solv->pool->whatprovidesdata + d; *dp; dp++, n++)
	if (*dp == -p)
	  return 0;			/* rule is self-fulfilling */
      if (n == 1)
	d = dp[-1];
    }

  if (n == 0 && !solv->jobrules)
    {
      /* this is a rpm rule assertion, we do not have to allocate it */
      /* it can be identified by a level of 1 and a zero reason */
      /* we must not drop those rules from the decisionq when rewinding! */
      if (p >= 0)
	abort();
      if (solv->decisionmap[-p] > 0 || solv->decisionmap[-p] < -1)
	abort();
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
    return r;

  if (r && n > 1 && r->d && r->p == p)
    {
      Id *dp2;
      if (d == r->d)
	return r;
      dp2 = solv->pool->whatprovidesdata + r->d;
      for (dp = solv->pool->whatprovidesdata + d; *dp; dp++, dp2++)
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
      solv->rules = (Rule *)xrealloc(solv->rules, (solv->nrules + (RULES_BLOCK + 1)) * sizeof(Rule));
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
      r->w2 = solv->pool->whatprovidesdata[d];
    }
  r->n1 = 0;
  r->n2 = 0;

  if (solv->pool->verbose > 3)
    {
      printf ("  Add rule: ");
      printrule (solv, r);
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
  int i;
  Rule *r;
  Id *jp;

  if (v > 0)
    printrule(solv, solv->rules + v);
  else
    {
      v = -(v + 1);
      printf("JOB %d\n", v);
      jp = solv->ruletojob.elements;
      for (i = solv->jobrules, r = solv->rules + i; i < solv->systemrules; i++, r++, jp++)
	if (*jp == v)
	  {
	    printf(" -");
	    printrule(solv, r);
	  }
      printf("ENDJOB\n");
    }
}


static const char *
id2rc(Solver *solv, Id id)
{
  const char *evr;
  if (solv->rc_output != 2)
    return "";
  evr = id2str(solv->pool, id);
  if (*evr < '0' || *evr > '9')
    return "0:";
  while (*evr >= '0' && *evr <= '9')
    evr++;
  if (*evr != ':')
    return "0:";
  return "";
}


/**********************************************************************************/

/* go through system and job rules and add direct assertions
 * to the decisionqueue. If we find a conflict, disable rules and
 * add them to problem queue.
 */
static void
makeruledecisions(Solver *solv)
{
  int i, ri;
  Rule *r, *rr;
  Id v, vv;
  int decisionstart;

  if (solv->pool->verbose > 3)
      printf ("----- makeruledecisions ; size decisionq: %d -----\n",solv->decisionq.count);
  
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
	  if (solv->pool->verbose > 3)
	    {
		Solvable *s = solv->pool->solvables + vv;
		if (v < 0)
		    printf("removing  %s-%s%s\n", id2str(solv->pool, s->name), id2rc(solv, s->evr), id2str(solv->pool, s->evr));
		else
		    printf("installing  %s-%s%s\n", id2str(solv->pool, s->name), id2rc(solv, s->evr), id2str(solv->pool, s->evr));		    
	    }
	  continue;
	}
      if (v > 0 && solv->decisionmap[vv] > 0)
	continue;
      if (v < 0 && solv->decisionmap[vv] < 0)
	continue;
      /* found a conflict! */
      if (solv->learntrules && ri >= solv->learntrules)
        {
	  /* cannot happen, as this would mean that the problem
           * was not solvable, so we wouldn't have created the
           * learnt rule at all */
	  abort();
        }
      /* if we are weak, just disable ourself */
      if (ri >= solv->weakrules)
	{
	  printf("conflict, but I am weak, disabling ");
	  printrule(solv, r);
	  disablerule(solv, r);
	  continue;
	}
      /* only job and system rules left */
      for (i = 0; i < solv->decisionq.count; i++)
	if (solv->decisionq.elements[i] == -v)
	  break;
      if (i == solv->decisionq.count)
	abort();
      if (solv->decisionq_why.elements[i] == 0)
	{
	  /* conflict with rpm rule, need only disable our rule */
	  printf("conflict with rpm rule, disabling rule #%d\n", ri);
	  if (v < 0 && v != -SYSTEMSOLVABLE)
	    abort();
	  queue_push(&solv->problems, 0);
          if (v == -SYSTEMSOLVABLE)
	    queue_push(&solv->problems, 0);	/* sigh, we don't have a rule for that */
	  else
	    queue_push(&solv->problems, -v);	/* sigh, we don't have a rule for that */
	  v = ri;
	  if (ri < solv->systemrules)
	    v = -(solv->ruletojob.elements[ri - solv->jobrules] + 1);
	  queue_push(&solv->problems, v);
	  disableproblem(solv, v);
	  queue_push(&solv->problems, 0);
	  continue;
	}
      /* conflict with another job or system rule */
      /* remove old decision */
      printf("conflicting system/job rules over literal %d\n", vv);
      queue_push(&solv->problems, 0);
      queue_push(&solv->problems, solv->decisionq_why.elements[i]);
      /* push all of our rules asserting this literal on the problem stack */
      for (i = solv->jobrules, rr = solv->rules + i; i < solv->nrules; i++, rr++)
	{
	  if (!rr->w1 || rr->w2)
	    continue;
	  if (rr->p != v && rr->p != -v)
	    continue;
	  printf(" - disabling rule #%d\n", i);
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
  
  if (solv->pool->verbose > 3) 
      printf ("----- makeruledecisions end; size decisionq: %d -----\n",solv->decisionq.count);
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

  if (pool->verbose)
    printf("enabledisablelearntrules called\n");
  for (i = solv->learntrules, r = solv->rules + i; i < solv->nrules; i++, r++)
    {
      whyp = solv->learnt_pool.elements + solv->learnt_why.elements[i - solv->learntrules];
      while ((why = *whyp++) != 0)
	{
	  if (why < 0 || why >= i)
	    abort();		/* cannot reference newer learnt rules */
	  if (!solv->rules[why].w1)
	    break;
	}
      /* why != 0: we found a disabled rule, disable the learnt rule */
      if (why && r->w1)
	{
	  if (pool->verbose)
	    {
	      printf("disabling learnt ");
	      printrule(solv, r);
	    }
          disablerule(solv, r);
	}
      else if (!why && !r->w1)
	{
	  if (pool->verbose)
	    {
	      printf("re-enabling learnt ");
	      printrule(solv, r);
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
	      if (pool->verbose)
		{
		  printf("@@@ re-enabling ");
		  printrule(solv, r);
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
	  if (pool->verbose)
	    {
	      printf("@@@ re-enabling ");
	      printrule(solv, r);
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
	      if (pool->verbose)
		{
		  printf("@@@ re-enabling ");
		  printrule(solv, r);
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
 * "unflag" a resolvable if it is not installable via "addrule(solv, -n, 0)"
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

  if (solv->pool->verbose > 3)
    printf ("----- addrpmrulesforsolvable -----\n");
  
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

	      dp = GET_PROVIDESP(req, p);	/* get providers of req; p is a dummy only */ 

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
		      if (pool->verbose)
			printf("ignoring broken requires %s of installed package %s-%s.%s\n", dep2str(pool, req), id2str(pool, s->name), id2str(pool, s->evr), id2str(pool, s->arch));
		      continue;
		    }
		}

	      if (!*dp)
		{
		  /* nothing provides req! */
		  if (pool->verbose)
		     printf("package %s-%s.%s [%d] is not installable (%s)\n", id2str(pool, s->name), id2str(pool, s->evr), id2str(pool, s->arch), (Id)(s - pool->solvables), dep2str(pool, req));
		  addrule(solv, -n, 0); /* mark requestor as uninstallable */
		  if (solv->rc_output)
		    printf(">!> !unflag %s-%s.%s[%s]\n", id2str(pool, s->name), id2str(pool, s->evr), id2str(pool, s->arch), repo_name(s->repo));
		  continue;
		}

	      if (pool->verbose > 2)
	        {
		  printf("  %s-%s.%s requires %s\n", id2str(pool, s->name), id2str(pool, s->evr), id2str(pool, s->arch), dep2str(pool, req));
		  for (i = 0; dp[i]; i++)
		    printf("   provided by %s-%s.%s\n", id2str(pool, pool->solvables[dp[i]].name), id2str(pool, pool->solvables[dp[i]].evr), id2str(pool, pool->solvables[dp[i]].arch));
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
  if (solv->pool->verbose > 3)
    printf ("----- addrpmrulesforsolvable end -----\n");
  
}

static void
addrpmrulesforweak(Solver *solv, Map *m)
{
  Pool *pool = solv->pool;
  Solvable *s;
  Id sup, *supp;
  int i, n;

  if (solv->pool->verbose > 3)
    printf ("----- addrpmrulesforweak -----\n");
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
  if (solv->pool->verbose > 3)
    printf ("----- addrpmrulesforweak end -----\n");  
}

static void
addrpmrulesforupdaters(Solver *solv, Solvable *s, Map *m, int allowall)
{
  Pool *pool = solv->pool;
  int i;
  Queue qs;
  Id qsbuf[64];

  if (solv->pool->verbose > 3)
    printf ("----- addrpmrulesforupdaters -----\n");

  queue_init_buffer(&qs, qsbuf, sizeof(qsbuf)/sizeof(*qsbuf));
  policy_findupdatepackages(solv, s, &qs, allowall);
  if (!MAPTST(m, s - pool->solvables))	/* add rule for s if not already done */
    addrpmrulesforsolvable(solv, s, m); 
  for (i = 0; i < qs.count; i++)
    if (!MAPTST(m, qs.elements[i]))
      addrpmrulesforsolvable(solv, pool->solvables + qs.elements[i], m);
  queue_free(&qs);
  
  if (solv->pool->verbose > 3)
    printf ("----- addrpmrulesforupdaters -----\n");
  
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

  if (solv->pool->verbose > 3)
    printf ("-----  addupdaterule -----\n");

  queue_init_buffer(&qs, qsbuf, sizeof(qsbuf)/sizeof(*qsbuf));
  policy_findupdatepackages(solv, s, &qs, allowall);
  if (qs.count == 0)		       /* no updaters found */
    d = 0;
  else
    d = pool_queuetowhatprovides(pool, &qs);	/* intern computed queue */
  queue_free(&qs);
  addrule(solv, s - pool->solvables, d);	/* allow update of s */
  if (solv->pool->verbose > 3)
    printf ("-----  addupdaterule end -----\n");  
}


/*-----------------------------------------------------------------*/
/* watches */


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

  xfree(solv->watches);
				       /* lower half for removals, upper half for installs */
  solv->watches = (Id *)xcalloc(2 * nsolvables, sizeof(Id));
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

  if (solv->pool->verbose > 3)
    printf ("----- propagate -----\n");

  while (solv->propagate_index < solv->decisionq.count)
    {
      /* negate because our watches trigger if literal goes FALSE */
      pkg = -solv->decisionq.elements[solv->propagate_index++];
      if (pool->verbose > 3)
        {
	  printf("popagate for decision %d level %d\n", -pkg, level);
	  printruleelement(solv, 0, -pkg);
        }

      for (rp = watches + pkg; *rp; rp = nrp)
	{
	  r = solv->rules + *rp;
	  
	  if (pool->verbose > 3)
	    {
	      printf("  watch triggered ");
	      printrule(solv, r);
	    }
	  
	  if (pkg == r->w1)
	    {
	      ow = r->w2;
	      nrp = &r->n1;
	    }
	  else
	    {
	      ow = r->w1;
	      nrp = &r->n2;
	    }
	  /* if clause is TRUE, nothing to do */
	  if (DECISIONMAP_TRUE(ow))
	    continue;

          if (r->d)
	    {
	      /* not a binary clause, check if we need to move our watch */
	      if (r->p && r->p != ow && !DECISIONMAP_TRUE(-r->p))
		p = r->p;
	      else
		for (dp = pool->whatprovidesdata + r->d; (p = *dp++) != 0;)
		  if (p != ow && !DECISIONMAP_TRUE(-p))
		    break;
	      if (p)
		{
		  /* p is free to watch, move watch to p */
		  if (pool->verbose > 3)
		    {
		      if (p > 0)
			printf("    -> move w%d to %s-%s.%s\n", (pkg == r->w1 ? 1 : 2), id2str(pool, pool->solvables[p].name), id2str(pool, pool->solvables[p].evr), id2str(pool, pool->solvables[p].arch));
		      else
			printf("    -> move w%d to !%s-%s.%s\n", (pkg == r->w1 ? 1 : 2), id2str(pool, pool->solvables[-p].name), id2str(pool, pool->solvables[-p].evr), id2str(pool, pool->solvables[-p].arch));
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
	  if (pool->verbose > 2)
	    {
	      printf("unit ");
	      printrule(solv, r);
	    }
	  if (ow > 0)
            decisionmap[ow] = level;
	  else
            decisionmap[-ow] = -level;
	  queue_push(&solv->decisionq, ow);
	  queue_push(&solv->decisionq_why, r - solv->rules);
	  if (pool->verbose > 3)
	    {
	      Solvable *s = pool->solvables + (ow > 0 ? ow : -ow);
	      if (ow > 0)
		printf("  -> decided to install %s-%s.%s\n", id2str(pool, s->name), id2str(pool, s->evr), id2str(pool, s->arch));
	      else
		printf("  -> decided to conflict %s-%s.%s\n", id2str(pool, s->name), id2str(pool, s->evr), id2str(pool, s->arch));
	    }
	}
    }
  if (solv->pool->verbose > 3)
    printf ("----- propagate end-----\n");
  
  return 0;	/* all is well */
}


/*-----------------------------------------------------------------*/
/* Analysis */

/*
 * analyze
 *   and learn
 */

static int
analyze(Solver *solv, int level, Rule *c, int *pr, int *dr, int *why)
{
  Pool *pool = solv->pool;
  Queue r;
  int rlevel = 1;
  Map seen;		/* global? */
  Id v, vv, *dp;
  int l, i, idx;
  int num = 0;
  int learnt_why = solv->learnt_pool.count;
  Id *decisionmap = solv->decisionmap;
 
  queue_init(&r);

  if (pool->verbose > 1) printf("ANALYZE at %d ----------------------\n", level);
  map_init(&seen, pool->nsolvables);
  idx = solv->decisionq.count;
  for (;;)
    {
      if (pool->verbose > 1)
	{
	  if (c - solv->rules >= solv->learntrules)
	    printf("LEARNT ");
	  else if (c - solv->rules >= solv->weakrules)
	    printf("WEAK ");
	  else if (c - solv->rules >= solv->systemrules)
	    printf("SYSTEM ");
	  else if (c - solv->rules >= solv->jobrules)
	    printf("JOB ");
	  printrule(solv, c);
	}
      queue_push(&solv->learnt_pool, c - solv->rules);
      dp = c->d ? pool->whatprovidesdata + c->d : 0;
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
#if 0
	      int j;
	      for (j = 0; j < solv->decisionq.count; j++)
		if (solv->decisionq.elements[j] == v)
		  break;
	      if (j == solv->decisionq.count)
		abort();
	      queue_push(&rulq, -(j + 1));
#endif
	      continue;			/* initial setting */
	    }
	  MAPSET(&seen, vv);
	  if (l == level)
	    num++;			/* need to do this one as well */
	  else
	    {
	      queue_push(&r, v);
	      if (pool->verbose > 3)
	        {
		  printf("PUSH %d ", v);
		  printruleelement(solv, 0, v);
	        }
	      if (l > rlevel)
		rlevel = l;
	    }
	}
      if (pool->verbose > 3)
	printf("num = %d\n", num);
      if (num <= 0)
	abort();
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
  *pr = -v;
  if (r.count == 0)
    *dr = 0;
  else if (r.count == 1 && r.elements[0] < 0)
    *dr = r.elements[0];
  else
    *dr = pool_queuetowhatprovides(pool, &r);
  if (pool->verbose > 1)
    {
      printf("learned rule for level %d (am %d)\n", rlevel, level);
      printruleelement(solv, 0, -v);
      for (i = 0; i < r.count; i++)
        {
          v = r.elements[i];
          printruleelement(solv, 0, v);
        }
    }
  map_free(&seen);
  queue_push(&solv->learnt_pool, 0);
  if (pool->verbose > 3)
    {
      for (i = learnt_why; solv->learnt_pool.elements[i]; i++)
        {
	  printf("learnt_why ");
	  printrule(solv, solv->rules + solv->learnt_pool.elements[i]);
        }
    }
  if (why)
    *why = learnt_why;
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
  int i;
  Id v;

#if 0
  /* delete all learnt rules */
  solv->nrules = solv->learntrules;
  queue_empty(&solv->learnt_why);
  queue_empty(&solv->learnt_pool);
#else
  enabledisablelearntrules(solv);
#endif

  /* redo all direct rpm rule decisions */
  /* we break at the first decision with a why attached, this is
   * either a job/system rule assertion or a propagated decision */
  for (i = 0; i < solv->decisionq.count; i++)
    {
      v = solv->decisionq.elements[i];
      solv->decisionmap[v > 0 ? v : -v] = 0;
    }
  for (i = 0; i < solv->decisionq_why.count; i++)
    if (solv->decisionq_why.elements[i])
      break;
    else
      {
        v = solv->decisionq.elements[i];
        solv->decisionmap[v > 0 ? v : -v] = v > 0 ? 1 : -1;
      }

  if (solv->pool->verbose > 1)
    printf("decisions done reduced from %d to %d\n", solv->decisionq.count, i);

  solv->decisionq_why.count = i;
  solv->decisionq.count = i;
  solv->recommends_index = -1;
  solv->propagate_index = 0;

  /* redo all job/system decisions */
  makeruledecisions(solv);
  if (solv->pool->verbose > 1)
    printf("decisions so far: %d\n", solv->decisionq.count);
  /* recreate watch chains */
  makewatches(solv);
}


/*
 * analyze_unsolvable_rule
 */

static void
analyze_unsolvable_rule(Solver *solv, Rule *r)
{
  int i;
  Id why = r - solv->rules;
  if (solv->pool->verbose > 1)
    {
      if (why >= solv->jobrules && why < solv->systemrules)
	printf("JOB ");
      if (why >= solv->systemrules && why < solv->weakrules)
	printf("SYSTEM %d ", why - solv->systemrules);
      if (why >= solv->weakrules && why < solv->learntrules)
	printf("WEAK ");
      if (solv->learntrules && why >= solv->learntrules)
	printf("LEARNED ");
      printrule(solv, r);
    }
  if (solv->learntrules && why >= solv->learntrules)
    {
      for (i = solv->learnt_why.elements[why - solv->learntrules]; solv->learnt_pool.elements[i]; i++)
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
  int lastweak;

  if (pool->verbose > 1)
    printf("ANALYZE UNSOLVABLE ----------------------\n");
  oldproblemcount = solv->problems.count;

  /* make room for conflicting rule */
  queue_push(&solv->problems, 0);
  queue_push(&solv->problems, 0);

  r = cr;
  map_init(&seen, pool->nsolvables);
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
	  if (pool->verbose > 3)
	    {
	      printf("RPM ");
	      printruleelement(solv, 0, v);
	    }
	  continue;
	}
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
      for (i = oldproblemcount + 2; i < solv->problems.count - 1; i++)
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
      r = solv->rules + lastweak;
      printf("disabling weak ");
      printrule(solv, r);
      disablerule(solv, r);
      reset_solver(solv);
      return 1;
    }

  /* patch conflicting rule data */
  if (cr - solv->rules >= solv->learntrules)
    {
      /* we have to store the rule internals for learnt rules
       * as they get freed for every solver run */
      solv->problems.elements[oldproblemcount] = cr->p;
      solv->problems.elements[oldproblemcount + 1] = cr->d;
    }
  else
    solv->problems.elements[oldproblemcount + 1] = cr - solv->rules;

  if (disablerules)
    {
      for (i = oldproblemcount + 2; i < solv->problems.count - 1; i++)
        disableproblem(solv, solv->problems.elements[i]);
      reset_solver(solv);
      return 1;
    }
  if (pool->verbose)
    printf("UNSOLVABLE\n");
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
  Id v, vv;
  while (solv->decisionq.count)
    {
      v = solv->decisionq.elements[solv->decisionq.count - 1];
      vv = v > 0 ? v : -v;
      if (solv->decisionmap[vv] <= level && solv->decisionmap[vv] >= -level)
        break;
      if (solv->pool->verbose > 3)
	printf("reverting decision %d at %d\n", v, solv->decisionmap[vv]);
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
 * add free decision to decision q, increase level
 * propagate decision, return if no conflict.
 * in conflict case, analyze conflict rule, add resulting
 * rule to learnt rule set, make decision from learnt
 * rule (always unit) and re-propagate.
 */

static int
setpropagatelearn(Solver *solv, int level, Id decision, int disablerules)
{
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
      printf("conflict with rule #%d\n", (int)(r - solv->rules));
      l = analyze(solv, level, r, &p, &d, &why);	/* learnt rule in p and d */
      if (l >= level || l <= 0)
	abort();
      printf("reverting decisions (level %d -> %d)\n", level, l);
      level = l;
      revert(solv, level);
      r = addrule(solv, p, d);       /* p requires d */
      if (!r)
	abort();
      if (solv->learnt_why.count != (r - solv->rules) - solv->learntrules)
	{
	  printf("%d %d\n", solv->learnt_why.count, (int)(r - solv->rules) - solv->learntrules);
	  abort();
	}
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
      if (solv->pool->verbose > 1)
	{
	  printf("decision: ");
	  printruleelement(solv, 0, p);
	  printf("new rule: ");
	  printrule(solv, r);
	}
    }
  return level;
}


/*
 * install best package from the queue. We add an extra package, inst, if
 * provided. See comment in weak install section.
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

  if (pool->verbose > 3)
    {
      Solvable *s = pool->solvables + p;
      printf("installing %s-%s.%s\n", id2str(pool, s->name), id2str(pool, s->evr), id2str(pool, s->arch));
    }

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
  solv = (Solver *)xcalloc(1, sizeof(Solver));
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

  solv->decisionmap = (Id *)xcalloc(pool->nsolvables, sizeof(Id));
  solv->rules = (Rule *)xmalloc((solv->nrules + (RULES_BLOCK + 1)) * sizeof(Rule));
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
  xfree(solv->decisionmap);
  xfree(solv->rules);
  xfree(solv->watches);
  xfree(solv->weaksystemrules);
  xfree(solv->obsoletes);
  xfree(solv->obsoletes_data);
  xfree(solv);
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

  if (pool->verbose > 3)
    {
      printf("number of rules: %d\n", solv->nrules);
      for (i = 0; i < solv->nrules; i++)
	printrule(solv, solv->rules + i);
    }

  /* create watches chains */
  makewatches(solv);

  if (pool->verbose) printf("initial decisions: %d\n", solv->decisionq.count);
  if (pool->verbose > 3)
      printdecisions (solv);

  /* start SAT algorithm */
  level = 1;
  systemlevel = level + 1;
  if (pool->verbose) printf("solving...\n");

  queue_init(&dq);
  for (;;)
    {
      /*
       * propagate
       */
      
      if (level == 1)
	{
	  if (pool->verbose) printf("propagating (propagate_index: %d;  size decisionq: %d)...\n", solv->propagate_index, solv->decisionq.count);
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
	      if (pool->verbose) printf("installing system packages\n");
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
		  if (pool->verbose > 3)
		    printf("keeping %s-%s.%s\n", id2str(pool, s->name), id2str(pool, s->evr), id2str(pool, s->arch));
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
	      if (pool->verbose) printf("installing weak system packages\n");
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
      
      if (pool->verbose) printf("deciding unresolved rules\n");
      for (i = 1, n = 1; ; i++, n++)
	{
	  if (n == solv->nrules)
	    break;
	  if (i == solv->nrules)
	    i = 1;
	  r = solv->rules + i;
	  if (!r->w1)
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
	  if (dq.count < 2)
	    {
	      /* cannot happen as this means that
               * the rule is unit */
	      printrule(solv, r);
	      abort();
	    }
	  if (pool->verbose > 2)
	    printrule(solv, r);

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

	  if (pool->verbose) printf("installing recommended packages\n");
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
	      s = pool->solvables + p;
	      if (pool->verbose > 0)
		printf("installing recommended %s-%s.%s\n", id2str(pool, s->name), id2str(pool, s->evr), id2str(pool, s->arch));
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
	      if (pool->verbose > 0)
	        {
		  s = pool->solvables + p;
		  printf("branching with %s-%s.%s\n", id2str(pool, s->name), id2str(pool, s->evr), id2str(pool, s->arch));
	        }
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
	      s = pool->solvables + p;
	      if (pool->verbose > 0)
	        printf("minimizing %d -> %d with %s-%s.%s\n", solv->decisionmap[p], l, id2str(pool, s->name), id2str(pool, s->evr), id2str(pool, s->arch));

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

  if (pool->verbose)
    {
      printf("refine_suggestion start\n");
      for (i = 0; problem[i]; i++)
	{
	  if (problem[i] == sug)
	    printf("=> ");
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
      run_solver(solv, 0, 0);
      if (!solv->problems.count)
	{
	  if (pool->verbose)
	    printf("no more problems!\n");
	  if (pool->verbose > 3)
	    printdecisions(solv);
	  break;		/* great, no more problems */
	}
      disabledcnt = disabled.count;
      /* skip over problem rule */
      for (i = 2; i < solv->problems.count - 1; i++)
	{
	  /* ignore solutions in refined */
          v = solv->problems.elements[i];
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
	  if (pool->verbose)
	    printf("no solution found!\n");
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
	  if (pool->verbose > 1)
	    {
	      printf("more than one solution found:\n");
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
  if (pool->verbose)
    printf("refine_suggestion end\n");
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
  /* copy over problem rule */
  queue_push(&solutions, problems.elements[0]);
  queue_push(&solutions, problems.elements[1]);
  problem = problems.elements + 2;
  for (i = 2; i < problems.count; i++)
    {
      Id v = problems.elements[i];
      if (v == 0)
	{
	  /* mark end of this problem */
	  queue_push(&solutions, 0);
	  queue_push(&solutions, 0);
	  if (i + 1 == problems.count)
	    break;
	  /* copy over problem rule of next problem */
          queue_push(&solutions, problems.elements[i + 1]);
          queue_push(&solutions, problems.elements[i + 2]);
	  i += 2;
	  problem = problems.elements + i + 1;
	  continue;
	}
      refine_suggestion(solv, job, problem, v, &solution);
      if (!solution.count)
	continue;	/* this solution didn't work out */

      for (j = 0; j < solution.count; j++)
	{
	  why = solution.elements[j];
#if 0
	  printproblem(solv, why);
#endif
	  if (why < 0)
	    {
	      queue_push(&solutions, 0);
	      queue_push(&solutions, -why);
	    }
	  else if (why >= solv->systemrules && why < solv->weakrules)
	    {
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
	  else
	    abort();
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
  queue_free(&solutions);
}


  
/*
 * printdecisions
 */
  

void
printdecisions(Solver *solv)
{
  Pool *pool = solv->pool;
  Repo *installed = solv->installed;
  Id p, *obsoletesmap;
  int i;
  Solvable *s;

  obsoletesmap = (Id *)xcalloc(pool->nsolvables, sizeof(Id));
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

  if (solv->rc_output)
    printf(">!> Solution #1:\n");

  int installs = 0, uninstalls = 0, upgrades = 0;
  
  /* print solvables to be erased */

  if (installed)
    {
      for (i = installed->start; i < installed->end; i++)
	{
	  s = pool->solvables + i;
	  if (s->repo != installed)
	    continue;
	  if (solv->decisionmap[i] >= 0)
	    continue;
	  if (obsoletesmap[i])
	    continue;
	  if (solv->rc_output == 2)
	    printf(">!> remove  %s-%s%s\n", id2str(pool, s->name), id2rc(solv, s->evr), id2str(pool, s->evr));
	  else if (solv->rc_output)
	    printf(">!> remove  %s-%s.%s\n", id2str(pool, s->name), id2str(pool, s->evr), id2str(pool, s->arch));
	  else
	    printf("erase   %s-%s.%s\n", id2str(pool, s->name), id2str(pool, s->evr), id2str(pool, s->arch));
	  uninstalls++;
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
	  if (solv->rc_output)
	    printf(">!> ");
          printf("install %s-%s%s", id2str(pool, s->name), id2rc(solv, s->evr), id2str(pool, s->evr));
	  if (solv->rc_output != 2)
            printf(".%s", id2str(pool, s->arch));
	  installs++;
        }
      else if (!solv->rc_output)
	{
	  printf("update  %s-%s.%s  (obsoletes", id2str(pool, s->name), id2str(pool, s->evr), id2str(pool, s->arch));
	  for (j = installed->start; j < installed->end; j++)
	    {
	      if (obsoletesmap[j] != p)
		continue;
	      s = pool->solvables + j;
	      printf(" %s-%s.%s", id2str(pool, s->name), id2str(pool, s->evr), id2str(pool, s->arch));
	    }
	  printf(")");
	  upgrades++;
	}
      else
	{
	  Solvable *f, *fn = 0;
	  for (j = installed->start; j < installed->end; j++)
	    {
	      if (obsoletesmap[j] != p)
		continue;
	      f = pool->solvables + j;
	      if (fn || f->name != s->name)
		{
		  if (solv->rc_output == 2)
		    printf(">!> remove  %s-%s%s\n", id2str(pool, f->name), id2rc(solv, f->evr), id2str(pool, f->evr));
		  else if (solv->rc_output)
		    printf(">!> remove  %s-%s.%s\n", id2str(pool, f->name), id2str(pool, f->evr), id2str(pool, f->arch));
		  uninstalls++;
		}
	      else
		fn = f;
	    }
	  if (!fn)
	    {
	      printf(">!> install %s-%s%s", id2str(pool, s->name), id2rc(solv, s->evr), id2str(pool, s->evr));
	      if (solv->rc_output != 2)
	        printf(".%s", id2str(pool, s->arch));
	      installs++;
	    }
	  else
	    {
	      if (solv->rc_output == 2)
	        printf(">!> upgrade %s-%s => %s-%s%s", id2str(pool, fn->name), id2str(pool, fn->evr), id2str(pool, s->name), id2rc(solv, s->evr), id2str(pool, s->evr));
	      else
	        printf(">!> upgrade %s-%s.%s => %s-%s.%s", id2str(pool, fn->name), id2str(pool, fn->evr), id2str(pool, fn->arch), id2str(pool, s->name), id2str(pool, s->evr), id2str(pool, s->arch));
	      upgrades++;
	    }
	}
      if (solv->rc_output)
	{
	  Repo *repo = s->repo;
	  if (repo && strcmp(repo_name(repo), "locales"))
	    printf("[%s]", repo_name(repo));
        }
      printf("\n");
    }

  if (solv->rc_output)
    printf(">!> installs=%d, upgrades=%d, uninstalls=%d\n", installs, upgrades, uninstalls);
  
  xfree(obsoletesmap);

  if (solv->suggestions.count)
    {
      printf("\nsuggested packages:\n");
      for (i = 0; i < solv->suggestions.count; i++)
	{
	  s = pool->solvables + solv->suggestions.elements[i];
	  printf("- %s-%s.%s\n", id2str(pool, s->name), id2str(pool, s->evr), id2str(pool, s->arch));
	}
    }
}

int
printconflicts(Solver *solv, Solvable *s, Id pc)
{
  Pool *pool = solv->pool;
  Solvable *sc = pool->solvables + pc;
  Id p, *pp, con, *conp, obs, *obsp;
  int numc = 0;

  if (s->conflicts)
    {
      conp = s->repo->idarraydata + s->conflicts;
      while ((con = *conp++) != 0)
	{
	  FOR_PROVIDES(p, pp, con)
	    {
	      if (p != pc)
		continue;
	      printf("packags %s-%s.%s conflicts with %s, which is provided by %s-%s.%s\n", id2str(pool, s->name), id2str(pool, s->evr), id2str(pool, s->arch), dep2str(pool, con), id2str(pool, sc->name), id2str(pool, sc->evr), id2str(pool, sc->arch));
	      numc++;
	    }
	}
    }
  if (s->obsoletes && (!solv->installed || s->repo != solv->installed))
    {
      obsp = s->repo->idarraydata + s->obsoletes;
      while ((obs = *obsp++) != 0)
	{
	  FOR_PROVIDES(p, pp, obs)
	    {
	      if (p != pc)
		continue;
	      printf("packags %s-%s.%s obsolets %s, which is provided by %s-%s.%s\n", id2str(pool, s->name), id2str(pool, s->evr), id2str(pool, s->arch), dep2str(pool, obs), id2str(pool, sc->name), id2str(pool, sc->evr), id2str(pool, sc->arch));
	      numc++;
	    }
	}
    }
  return numc;
}

void
printprobleminfo(Solver *solv, Id p, Id d, Queue *job, Id firstp, Id firstrp)
{
  Pool *pool = solv->pool;
  Rule *r;
  Solvable *s;
  Id what;

  if (p != 0)
    {
      /* learnt rule, ignore for now */
      printf("some learnt rule...\n");
      return;
    }
  if (d == 0)
    {
      /* conflict with system solvable, i.e. could not create rule */
      if (firstp)
	{
	  printf("got firstp\n");
	  abort();
	}
      what = job->elements[firstrp];
      switch (job->elements[firstrp - 1])
	{
	case SOLVER_INSTALL_SOLVABLE_NAME:
	  printf("no solvable exists with name %s\n", dep2str(pool, what));
	  break;
	case SOLVER_INSTALL_SOLVABLE_PROVIDES:
	  printf("no solvable provides %s\n", dep2str(pool, what));
	  break;
	default:
	  printf("unknown  job\n");
	  abort();
	}
      return;
    }
  else if (d < 0)
    {
      Id req, *reqp, *dp;
      int count = 0;
      /* conflict with rpm rule, package -d is not installable */
      s = pool->solvables + (-d);
      if (s->requires)
	{
	  reqp = s->repo->idarraydata + s->requires;
	  while ((req = *reqp++) != 0)
	    {
	      if (req == SOLVABLE_PREREQMARKER)
		continue;
	      dp = GET_PROVIDESP(req, p);
	      if (*dp)
		continue;
	      printf("package %s-%s.%s requires %s, but no package provides it\n", id2str(pool, s->name), id2str(pool, s->evr), id2str(pool, s->arch), dep2str(pool, req));
	      count++;
	    }
	}
      if (!count)
        printf("package %s-%s.%s is not installable\n", id2str(pool, s->name), id2str(pool, s->evr), id2str(pool, s->arch));
      return;
    }
  if (d >= solv->jobrules)
    {
      r = solv->rules + d;
      p = r->p;
      d = r->d;
      if (p < 0 && d < 0)
	{
	  Solvable *sp, *sd;
	  sp = pool->solvables + (-p);
	  sd = pool->solvables + (-d);
	  printf("package %s-%s.%s cannot be installed with package %s-%s.%s\n", id2str(pool, sp->name), id2str(pool, sp->evr), id2str(pool, sp->arch), id2str(pool, sd->name), id2str(pool, sd->evr), id2str(pool, sd->arch));
	  return;
	}
      printf("some job rule...\n");
      printrule(solv, r);
      return;
    }
  r = solv->rules + d;
  p = r->p;
  if (p >= 0)
    abort();
  d = r->d;
  if (d == 0 && r->w2 < 0)
    {
      Solvable *sp, *sd;
      d = r->w2;
      sp = pool->solvables + (-p);
      sd = pool->solvables + (-d);
      if (sp->name == sd->name)
	{
	  printf("cannot install both %s-%s.%s and %s-%s.%s\n", id2str(pool, sp->name), id2str(pool, sp->evr), id2str(pool, sp->arch), id2str(pool, sd->name), id2str(pool, sd->evr), id2str(pool, sd->arch));
	}
      else
	{
	  printconflicts(solv, pool->solvables + (-p), -d);
	  printconflicts(solv, pool->solvables + (-d), -p);
	}
    }
  else
    {
      /* find requires of p that corresponds with our rule */
      Id req, *reqp, *dp;
      s = pool->solvables + (-p);
      reqp = s->repo->idarraydata + s->requires;
      while ((req = *reqp++) != 0)
	{
	  if (req == SOLVABLE_PREREQMARKER)
	    continue;
	  dp = GET_PROVIDESP(req, p);
          if (d == 0)
	    {
	      if (*dp == r->w2 && dp[1] == 0)
		break;
	    }
	  else if (dp - pool->whatprovidesdata == d)
	    break;
	}
      if (!req)
	{
	  printf("req not found\n");
	  abort();
	}
      printf("package %s-%s.%s requires %s, but none of its providers can be installed\n", id2str(pool, s->name), id2str(pool, s->evr), id2str(pool, s->arch), dep2str(pool, req));
    }
}

void
printsolutions(Solver *solv, Queue *job)
{
  Pool *pool = solv->pool;
  int pcnt;
  int i;
  Id p, d, rp, what;
  Solvable *s, *sd;

  printf("Encountered problems! Here are the solutions:\n\n");
  pcnt = 1;
  for (i = 0; i < solv->problems.count; )
    {
      printf("Problem %d:\n", pcnt++);
      printf("====================================\n");
      p = solv->problems.elements[i++];
      d = solv->problems.elements[i++];
      printprobleminfo(solv, p, d, job, solv->problems.elements[i], solv->problems.elements[i + 1]);
      printf("\n");
      for (;;)
        {
	  if (solv->problems.elements[i] == 0 && solv->problems.elements[i + 1] == 0)
	    {
	      /* end of solutions for this problems reached */
	      i += 2;
	      break;
	    }
	  for (;;)
	    {
	      p = solv->problems.elements[i];
	      rp = solv->problems.elements[i + 1];
	      i += 2;
	      if (p == 0 && rp == 0)
		{
		  /* end of this solution reached */
		  printf("\n");
		  break;
		}
	      if (p == 0)
		{
		  /* job, p is index into job queue */
		  what = job->elements[rp];
		  switch (job->elements[rp - 1])
		    {
		    case SOLVER_INSTALL_SOLVABLE:
		      s = pool->solvables + what;
		      if (solv->installed && s->repo == solv->installed)
			printf("- do not keep %s-%s.%s installed\n", id2str(pool, s->name), id2str(pool, s->evr), id2str(pool, s->arch));
		      else
			printf("- do not install %s-%s.%s\n", id2str(pool, s->name), id2str(pool, s->evr), id2str(pool, s->arch));
		      break;
		    case SOLVER_ERASE_SOLVABLE:
		      s = pool->solvables + what;
		      if (solv->installed && s->repo == solv->installed)
			printf("- do not deinstall %s-%s.%s\n", id2str(pool, s->name), id2str(pool, s->evr), id2str(pool, s->arch));
		      else
			printf("- do not forbid installation of %s-%s.%s\n", id2str(pool, s->name), id2str(pool, s->evr), id2str(pool, s->arch));
		      break;
		    case SOLVER_INSTALL_SOLVABLE_NAME:
		      printf("- do not install %s\n", id2str(pool, what));
		      break;
		    case SOLVER_ERASE_SOLVABLE_NAME:
		      printf("- do not deinstall %s\n", id2str(pool, what));
		      break;
		    case SOLVER_INSTALL_SOLVABLE_PROVIDES:
		      printf("- do not install a solvable providing %s\n", dep2str(pool, what));
		      break;
		    case SOLVER_ERASE_SOLVABLE_PROVIDES:
		      printf("- do not deinstall all solvables providing %s\n", dep2str(pool, what));
		      break;
		    case SOLVER_INSTALL_SOLVABLE_UPDATE:
		      s = pool->solvables + what;
		      printf("- do not install most recent version of %s-%s.%s\n", id2str(pool, s->name), id2str(pool, s->evr), id2str(pool
    , s->arch));
		      break;
		    default:
		      printf("- do something different\n");
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
		      if (!solv->allowdowngrade && evrcmp(pool, s->evr, sd->evr) > 0)
			{
			  printf("- allow downgrade of %s-%s.%s to %s-%s.%s\n", id2str(pool, s->name), id2str(pool, s->evr), id2str(pool, s->arch), id2str(pool, sd->name), id2str(pool, sd->evr), id2str(pool, sd->arch));
			  gotone = 1;
			}
		      if (!solv->allowarchchange && s->name == sd->name && s->arch != sd->arch && policy_illegal_archchange(pool, s, sd))
			{
			  printf("- allow architecture change of %s-%s.%s to %s-%s.%s\n", id2str(pool, s->name), id2str(pool, s->evr), id2str(pool, s->arch), id2str(pool, sd->name), id2str(pool, sd->evr), id2str(pool, sd->arch));
			  gotone = 1;
			}
		      if (!solv->allowvendorchange && s->name == sd->name && s->vendor != sd->vendor && policy_illegal_vendorchange(pool, s, sd))
			{
			  if (sd->vendor)
			    printf("- allow vendor change from '%s' (%s-%s.%s) to '%s' (%s-%s.%s)\n", id2str(pool, s->vendor), id2str(pool, s->name), id2str(pool, s->evr), id2str(pool, s->arch), id2str(pool, sd->vendor), id2str(pool, sd->name), id2str(pool, sd->evr), id2str(pool, sd->arch));
			  else
			    printf("- allow vendor change from '%s' (%s-%s.%s) to no vendor (%s-%s.%s)\n", id2str(pool, s->vendor), id2str(pool, s->name), id2str(pool, s->evr), id2str(pool, s->arch), id2str(pool, sd->name), id2str(pool, sd->evr), id2str(pool, sd->arch));
			  gotone = 1;
			}
		      if (!gotone)
			printf("- allow replacement of %s-%s.%s with %s-%s.%s\n", id2str(pool, s->name), id2str(pool, s->evr), id2str(pool, s->arch), id2str(pool, sd->name), id2str(pool, sd->evr), id2str(pool, sd->arch));
		    }
		  else
		    {
		      printf("- allow deinstallation of %s-%s.%s [%d]\n", id2str(pool, s->name), id2str(pool, s->evr), id2str(pool, s->arch), (Id)(s - pool->solvables));
		    }

		}
	    }
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
  solv->obsoletes = obsoletes = xcalloc(installed->end - installed->start, sizeof(Id));
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
  solv->obsoletes_data = obsoletes_data = xcalloc(n + 1, sizeof(Id));
  if (pool->verbose) printf("obsoletes data: %d entries\n", n + 1);
  for (i = pool->nsolvables - 1; i > 0; i--)
    {
      s = pool->solvables + i;
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
solve(Solver *solv, Queue *job)
{
  Pool *pool = solv->pool;
  Repo *installed = solv->installed;
  int i;
  int oldnrules;
  Map addedmap;			       /* '1' == have rule for solvable */
  Id how, what, p, *pp, d;
  Queue q;
  Solvable *s;

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
      if (pool->verbose > 3)
	printf ("*** create rpm rules for installed solvables ***\n");
      for (i = installed->start, s = pool->solvables + i; i < installed->end; i++, s++)
	if (s->repo == installed)
	  addrpmrulesforsolvable(solv, s, &addedmap);
      if (pool->verbose)
	printf("added %d rpm rules for installed solvables\n", solv->nrules - oldnrules);
      if (pool->verbose > 3)
	printf ("*** create rpm rules for updaters of installed solvables ***\n");
      oldnrules = solv->nrules;
      for (i = installed->start, s = pool->solvables + i; i < installed->end; i++, s++)
	if (s->repo == installed)
	  addrpmrulesforupdaters(solv, s, &addedmap, 1);
      if (pool->verbose)
	printf("added %d rpm rules for updaters of installed solvables\n", solv->nrules - oldnrules);
    }

  if (solv->pool->verbose > 3)
    printf ("*** create rpm rules for packages involved with a job ***\n");
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
  if (pool->verbose)
    printf("added %d rpm rules for packages involved in a job\n", solv->nrules - oldnrules);

  if (solv->pool->verbose > 3)
    printf ("*** create rpm rules for recommended/suggested packages ***\n");

  oldnrules = solv->nrules;
  addrpmrulesforweak(solv, &addedmap);
  if (pool->verbose)
    printf("added %d rpm rules because of weak dependencies\n", solv->nrules - oldnrules);

#if 1
  if (pool->verbose)
    {
      int possible = 0, installable = 0;
      for (i = 1; i < pool->nsolvables; i++)
	{
	  if (pool_installable(pool, pool->solvables + i))
	    installable++;
	  if (MAPTST(&addedmap, i))
	    possible++;
	}
      printf("%d of %d installable solvables considered for solving\n", possible, installable);
    }
#endif

  /*
   * first pass done, we now have all the rpm rules we need.
   * unify existing rules before going over all job rules and
   * policy rules.
   * at this point the system is always solvable,
   * as an empty system (remove all packages) is a valid solution
   */
  
  unifyrules(solv);	/* remove duplicate rpm rules */

  if (pool->verbose) printf("decisions so far: %d\n", solv->decisionq.count);
  if (pool->verbose > 3)
      printdecisions (solv);

  /*
   * now add all job rules
   */

  if (solv->pool->verbose > 3)
      printf ("*** Add JOB rules ***\n");  
  
  solv->jobrules = solv->nrules;

  for (i = 0; i < job->count; i += 2)
    {
      how = job->elements[i];
      what = job->elements[i + 1];
      switch(how)
	{
	case SOLVER_INSTALL_SOLVABLE:			/* install specific solvable */
	  s = pool->solvables + what;
	  if (solv->rc_output)
            {
	      printf(">!> Installing %s from channel %s\n", id2str(pool, s->name), repo_name(s->repo));
	    }
	  if (pool->verbose)
	    printf("job: install solvable %s-%s.%s\n", id2str(pool, s->name), id2str(pool, s->evr), id2str(pool, s->arch));
          addrule(solv, what, 0);			/* install by Id */
	  queue_push(&solv->ruletojob, i);
	  break;
	case SOLVER_ERASE_SOLVABLE:
	  s = pool->solvables + what;
	  if (pool->verbose)
	    printf("job: erase solvable %s-%s.%s\n", id2str(pool, s->name), id2str(pool, s->evr), id2str(pool, s->arch));
          addrule(solv, -what, 0);			/* remove by Id */
	  queue_push(&solv->ruletojob, i);
	  break;
	case SOLVER_INSTALL_SOLVABLE_NAME:		/* install by capability */
	case SOLVER_INSTALL_SOLVABLE_PROVIDES:
	  if (pool->verbose && how == SOLVER_INSTALL_SOLVABLE_NAME)
	    printf("job: install name %s\n", id2str(pool, what));
	  if (pool->verbose && how == SOLVER_INSTALL_SOLVABLE_PROVIDES)
	    printf("job: install provides %s\n", dep2str(pool, what));
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
	  if (pool->verbose && how == SOLVER_ERASE_SOLVABLE_NAME)
	    printf("job: erase name %s\n", id2str(pool, what));
	  if (pool->verbose && how == SOLVER_ERASE_SOLVABLE_PROVIDES)
	    printf("job: erase provides %s\n", dep2str(pool, what));
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
	  if (pool->verbose)
	    printf("job: update %s-%s.%s\n", id2str(pool, s->name), id2str(pool, s->evr), id2str(pool, s->arch));
	  addupdaterule(solv, s, 0);
	  queue_push(&solv->ruletojob, i);
	  break;
	}
    }

  if (solv->ruletojob.count != solv->nrules - solv->jobrules)
    abort();

  /*
   * now add system rules
   * 
   */

 if (solv->pool->verbose > 3)
     printf ("*** Add system rules ***\n");
  
  
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
      if (solv->nrules - solv->systemrules != installed->end - installed->start)
	abort();
    }

  /* create special weak system rules */
  /* those are used later on to keep a version of the installed packages in
     best effort mode */
  if (installed && installed->nsolvables)
    {
      solv->weaksystemrules = xcalloc(installed->end - installed->start, sizeof(Id));
      for (i = installed->start, s = pool->solvables + i; i < installed->end; i++, s++)
	if (s->repo == installed)
	  {
	    policy_findupdatepackages(solv, s, &q, 1);
	    if (q.count)
	      solv->weaksystemrules[i - installed->start] = pool_queuetowhatprovides(pool, &q);
	  }
    }

  /* free unneeded memory */
  map_free(&addedmap);
  queue_free(&q);

  solv->weakrules = solv->nrules;

  /* try real hard to keep packages installed */
  if (0)
    {
      for (i = installed->start, s = pool->solvables + i; i < installed->end; i++, s++)
	if (s->repo == installed)
	  {
	    /* FIXME: can't work with refine_suggestion! */
            /* need to always add the rule but disable it */
	    if (MAPTST(&solv->noupdate, i - installed->start))
	      continue;
	    d = solv->weaksystemrules[i - installed->start];
	    addrule(solv, i, d);
	  }
    }

  /* all new rules are learnt after this point */
  solv->learntrules = solv->nrules;

  /*
   * solve !
   * 
   */
  
  disableupdaterules(solv, job, -1);
  makeruledecisions(solv);

  if (pool->verbose) printf("problems so far: %d\n", solv->problems.count);

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

