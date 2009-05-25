/*
 * Copyright (c) 2007-2009, Novell Inc.
 *
 * This program is licensed under the BSD license, read LICENSE.BSD
 * for further information
 */

/*
 * rules.c
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

static void addrpmruleinfo(Solver *solv, Id p, Id d, int type, Id dep);

/*-------------------------------------------------------------------
 * Check if dependency is possible
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

int
solver_samerule(Solver *solv, Rule *r1, Rule *r2)
{
  return unifyrules_sortcmp(r1, r2, solv->pool);
}


/*-------------------------------------------------------------------
 *
 * unify rules
 * go over all rules and remove duplicates
 */

void
solver_unifyrules(Solver *solv)
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

Rule *
solver_addrule(Solver *solv, Id p, Id d)
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

/*
 *  special multiversion patch conflict handling:
 *  a patch conflict is also satisfied, if some other
 *  version with the same name/arch that doesn't conflict
 *  get's installed. The generated rule is thus:
 *  -patch|-cpack|opack1|opack2|...
 */
static Id
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
    solver_addrule(solv, p, d);
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

void
solver_addrpmrulesforsolvable(Solver *solv, Solvable *s, Map *m)
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

void
solver_addrpmrulesforweak(Solver *solv, Map *m)
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
      solver_addrpmrulesforsolvable(solv, s, m);
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

void
solver_addrpmrulesforupdaters(Solver *solv, Solvable *s, Map *m, int allow_all)
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
    solver_addrpmrulesforsolvable(solv, s, m);
    /* foreach update candidate, add rule if not already done */
  for (i = 0; i < qs.count; i++)
    if (!MAPTST(m, qs.elements[i]))
      solver_addrpmrulesforsolvable(solv, pool->solvables + qs.elements[i], m);
  queue_free(&qs);

  POOL_DEBUG(SAT_DEBUG_SCHUBI, "----- addrpmrulesforupdaters -----\n");
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
  solver_addrpmrulesforsolvable(solv, pool->solvables - r->p, 0);
  /* also try reverse direction for conflicts */
  if ((r->d == 0 || r->d == -1) && r->w2 < 0)
    solver_addrpmrulesforsolvable(solv, pool->solvables - r->w2, 0);
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
      solver_addrpmrulesforsolvable(solv, pool->solvables - r->p, 0);
      /* also try reverse direction for conflicts */
      if ((r->d == 0 || r->d == -1) && r->w2 < 0)
	solver_addrpmrulesforsolvable(solv, pool->solvables - r->w2, 0);
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

/* EOF */
