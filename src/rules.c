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
#include "poolarch.h"
#include "util.h"
#include "policy.h"
#include "solverdebug.h"

#define RULES_BLOCK 63

static void addrpmruleinfo(Solver *solv, Id p, Id d, int type, Id dep);
static void solver_createcleandepsmap(Solver *solv);

/*-------------------------------------------------------------------
 * Check if dependency is possible
 * 
 * mirrors solver_dep_fulfilled but uses map m instead of the decisionmap
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
   * debug: log rule statistics
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


/******************************************************************************
 ***
 *** rpm rule part: create rules representing the package dependencies
 ***
 ***/

/*
 *  special multiversion patch conflict handling:
 *  a patch conflict is also satisfied if some other
 *  version with the same name/arch that doesn't conflict
 *  gets installed. The generated rule is thus:
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
	  && s->repo == installed	/* solvable is installed */
	  && (!solv->fixmap.size || !MAPTST(&solv->fixmap, n - installed->start)))
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
		  if (p == n && !pool->allowselfconflicts)
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
       * check obsoletes and implicit obsoletes of a package
       * if ignoreinstalledsobsoletes is not set, we're also checking
       * obsoletes of installed packages (like newer rpm versions)
       */
      if ((!installed || s->repo != installed) || !pool->noinstalledobsoletes)
	{
	  int noobs = solv->noobsoletes.size && MAPTST(&solv->noobsoletes, n);
	  int isinstalled = (installed && s->repo == installed);
	  if (s->obsoletes && !noobs)
	    {
	      obsp = s->repo->idarraydata + s->obsoletes;
	      /* foreach obsoletes */
	      while ((obs = *obsp++) != 0)
		{
		  /* foreach provider of an obsoletes of 's' */ 
		  FOR_PROVIDES(p, pp, obs)
		    {
		      Solvable *ps = pool->solvables + p;
		      if (p == n)
			continue;
		      if (isinstalled && dontfix && ps->repo == installed)
			continue;	/* don't repair installed/installed problems */
		      if (!pool->obsoleteusesprovides /* obsoletes are matched names, not provides */
			  && !pool_match_nevr(pool, ps, obs))
			continue;
		      if (pool->obsoleteusescolors && !pool_colormatch(pool, s, ps))
			continue;
		      if (!isinstalled)
			addrpmrule(solv, -n, -p, SOLVER_RULE_RPM_PACKAGE_OBSOLETES, obs);
		      else
			addrpmrule(solv, -n, -p, SOLVER_RULE_RPM_INSTALLEDPKG_OBSOLETES, obs);
		    }
		}
	    }
	  /* check implicit obsoletes
           * for installed packages we only need to check installed/installed problems (and
           * only when dontfix is not set), as the others are picked up when looking at the
           * uninstalled package.
           */
	  if (!isinstalled || !dontfix)
	    {
	      FOR_PROVIDES(p, pp, s->name)
		{
		  Solvable *ps = pool->solvables + p;
		  if (p == n)
		    continue;
		  if (isinstalled && ps->repo != installed)
		    continue;
		  /* we still obsolete packages with same nevra, like rpm does */
		  /* (actually, rpm mixes those packages. yuck...) */
		  if (noobs && (s->name != ps->name || s->evr != ps->evr || s->arch != ps->arch))
		    continue;
		  if (!pool->implicitobsoleteusesprovides && s->name != ps->name)
		    continue;
		  if (pool->obsoleteusescolors && !pool_colormatch(pool, s, ps))
		    continue;
		  if (s->name == ps->name)
		    addrpmrule(solv, -n, -p, SOLVER_RULE_RPM_SAME_NAME, 0);
		  else
		    addrpmrule(solv, -n, -p, SOLVER_RULE_RPM_IMPLICIT_OBSOLETES, s->name);
		}
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


/***********************************************************************
 ***
 ***  Update/Feature rule part
 ***
 ***  Those rules make sure an installed package isn't silently deleted
 ***
 ***/

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

void
solver_addupdaterule(Solver *solv, Solvable *s, int allow_all)
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
      if (j < qs.count)
	{
	  if (d && solv->updatesystem && solv->installed && s->repo == solv->installed)
	    {
	      if (!solv->multiversionupdaters)
		solv->multiversionupdaters = sat_calloc(solv->installed->end - solv->installed->start, sizeof(Id));
	      solv->multiversionupdaters[s - pool->solvables - solv->installed->start] = d;
	    }
	  if (j == 0 && p == -SYSTEMSOLVABLE && solv->distupgrade)
	    {
	      queue_push(&solv->orphaned, s - pool->solvables);	/* treat as orphaned */
	      j = qs.count;
	    }
	  qs.count = j;
	}
    }
  if (qs.count && p == -SYSTEMSOLVABLE)
    p = queue_shift(&qs);
  d = qs.count ? pool_queuetowhatprovides(pool, &qs) : 0;
  queue_free(&qs);
  solver_addrule(solv, p, d);	/* allow update of s */
  POOL_DEBUG(SAT_DEBUG_SCHUBI, "-----  addupdaterule end -----\n");
}

static inline void 
disableupdaterule(Solver *solv, Id p)
{
  Rule *r;

  MAPSET(&solv->noupdate, p - solv->installed->start);
  r = solv->rules + solv->updaterules + (p - solv->installed->start);
  if (r->p && r->d >= 0)
    solver_disablerule(solv, r);
  r = solv->rules + solv->featurerules + (p - solv->installed->start);
  if (r->p && r->d >= 0)
    solver_disablerule(solv, r);
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
      solver_enablerule(solv, r);
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
      solver_enablerule(solv, r);
      IF_POOLDEBUG (SAT_DEBUG_SOLUTIONS)
	{
	  POOL_DEBUG(SAT_DEBUG_SOLUTIONS, "@@@ re-enabling ");
	  solver_printruleclass(solv, SAT_DEBUG_SOLUTIONS, r);
	}
    }
}


/***********************************************************************
 ***
 ***  Infarch rule part
 ***
 ***  Infarch rules make sure the solver uses the best architecture of
 ***  a package if multiple archetectures are available
 ***
 ***/

void
solver_addinfarchrules(Solver *solv, Map *addedmap)
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
	      if (pool->installed && ps->repo == pool->installed)
		continue;	/* always ok to keep an installed package */
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
	  solver_addrule(solv, -p, 0);
	}
    }
  queue_free(&badq);
  queue_free(&allowedarchs);
  solv->infarchrules_end = solv->nrules;
}

static inline void
disableinfarchrule(Solver *solv, Id name)
{
  Pool *pool = solv->pool;
  Rule *r;
  int i;
  for (i = solv->infarchrules, r = solv->rules + i; i < solv->infarchrules_end; i++, r++)
    {
      if (r->p < 0 && r->d >= 0 && pool->solvables[-r->p].name == name)
        solver_disablerule(solv, r);
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
          solver_enablerule(solv, r);
          IF_POOLDEBUG (SAT_DEBUG_SOLUTIONS)
            {
              POOL_DEBUG(SAT_DEBUG_SOLUTIONS, "@@@ re-enabling ");
              solver_printruleclass(solv, SAT_DEBUG_SOLUTIONS, r);
            }
        }
    }
}


/***********************************************************************
 ***
 ***  Dup rule part
 ***
 ***  Dup rules make sure a package is selected from the specified dup
 ***  repositories if an update candidate is included in one of them.
 ***
 ***/

void
solver_createdupmaps(Solver *solv)
{
  Queue *job = &solv->job;
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
	  if ((how & SOLVER_SELECTMASK) != SOLVER_SOLVABLE_REPO)
	    break;
	  if (what <= 0 || what > pool->nrepos)
	    break;
	  repo = pool_id2repo(pool, what);
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
			  Solvable *pis = pool->solvables + pi;
			  if (!pool->obsoleteusesprovides && !pool_match_nevr(pool, pis, obs))
			    continue;
			  if (pool->obsoleteusescolors && !pool_colormatch(pool, s, pis))
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

void
solver_freedupmaps(Solver *solv)
{
  map_free(&solv->dupmap);
  map_free(&solv->dupinvolvedmap);
}

void
solver_addduprules(Solver *solv, Map *addedmap)
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
		map_grow(&solv->updatemap, solv->installed->end - solv->installed->start);
	      MAPSET(&solv->updatemap, p - solv->installed->start);
	      if (!MAPTST(&solv->dupmap, p))
		{
		  Id ip, ipp;
		  /* is installed identical to a good one? */
		  FOR_PROVIDES(ip, ipp, ps->name)
		    {
		      Solvable *is = pool->solvables + ip;
		      if (!MAPTST(&solv->dupmap, ip))
			continue;
		      if (is->evr == ps->evr && solvable_identical(ps, is))
			break;
		    }
		  if (!ip)
		    solver_addrule(solv, -p, 0);	/* no match, sorry */
		}
	    }
	  else if (!MAPTST(&solv->dupmap, p))
	    solver_addrule(solv, -p, 0);
	}
    }
  solv->duprules_end = solv->nrules;
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
	solver_disablerule(solv, r);
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
	  solver_enablerule(solv, r);
	  IF_POOLDEBUG (SAT_DEBUG_SOLUTIONS)
	    {
	      POOL_DEBUG(SAT_DEBUG_SOLUTIONS, "@@@ re-enabling ");
	      solver_printruleclass(solv, SAT_DEBUG_SOLUTIONS, r);
	    }
	}
    }
}


/***********************************************************************
 ***
 ***  Policy rule disabling/reenabling
 ***
 ***  Disable all policy rules that conflict with our jobs. If a job
 ***  gets disabled later on, reenable the involved policy rules again.
 ***
 ***/

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
		  queue_push2(q, DISABLE_INFARCH, s->name);
		}
	    }
	}
      if (select != SOLVER_SOLVABLE)
	break;
      s = pool->solvables + what;
      if (solv->infarchrules != solv->infarchrules_end)
	queue_push2(q, DISABLE_INFARCH, s->name);
      if (solv->duprules != solv->duprules_end)
	queue_push2(q, DISABLE_DUP, s->name);
      if (!installed)
	return;
      if (solv->noobsoletes.size && MAPTST(&solv->noobsoletes, what))
	{
	  /* XXX: remove if we always do distupgrade with DUP rules */
	  if (solv->distupgrade && s->repo == installed)
	    queue_push2(q, DISABLE_UPDATE, what);
	  return;
	}
      if (s->repo == installed)
	{
	  queue_push2(q, DISABLE_UPDATE, what);
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
		if (!pool->obsoleteusesprovides && !pool_match_nevr(pool, ps, obs))
		  continue;
	        if (pool->obsoleteusescolors && !pool_colormatch(pool, s, ps))
		  continue;
		queue_push2(q, DISABLE_UPDATE, p);
	      }
	}
      FOR_PROVIDES(p, pp, s->name)
	{
	  Solvable *ps = pool->solvables + p;
	  if (ps->repo != installed)
	    continue;
	  if (!pool->implicitobsoleteusesprovides && ps->name != s->name)
	    continue;
	  if (pool->obsoleteusescolors && !pool_colormatch(pool, s, ps))
	    continue;
	  queue_push2(q, DISABLE_UPDATE, p);
	}
      return;
    case SOLVER_ERASE:
      if (!installed)
	break;
      FOR_JOB_SELECT(p, pp, select, what)
	if (pool->solvables[p].repo == installed)
	  queue_push2(q, DISABLE_UPDATE, p);
      return;
    default:
      return;
    }
}

/* disable all policy rules that are in conflict with our job list */
void
solver_disablepolicyrules(Solver *solv)
{
  Queue *job = &solv->job;
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
  if (solv->cleandepsmap.size)
    {
      solver_createcleandepsmap(solv);
      for (i = solv->installed->start; i < solv->installed->end; i++)
	if (MAPTST(&solv->cleandepsmap, i - solv->installed->start))
	  queue_push2(&allq, DISABLE_UPDATE, i);
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
void
solver_reenablepolicyrules(Solver *solv, int jobidx)
{
  Queue *job = &solv->job;
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
  if (solv->cleandepsmap.size)
    {
      solver_createcleandepsmap(solv);
      for (i = solv->installed->start; i < solv->installed->end; i++)
	if (MAPTST(&solv->cleandepsmap, i - solv->installed->start))
	  queue_push2(&allq, DISABLE_UPDATE, i);
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


/***********************************************************************
 ***
 ***  Rule info part, tell the user what the rule is about.
 ***
 ***/

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
  if (rid >= solv->choicerules && rid < solv->choicerules_end)
    {
      return SOLVER_RULE_CHOICE;
    }
  if (rid >= solv->learntrules)
    {
      return SOLVER_RULE_LEARNT;
    }
  return SOLVER_RULE_UNKNOWN;
}

void
addchoicerules(Solver *solv)
{
  Pool *pool = solv->pool;
  Map m, mneg;
  Rule *r;
  Queue q, qi;
  int i, j, rid, havechoice;
  Id p, d, *pp;
  Id p2, pp2;
  Solvable *s, *s2;

  solv->choicerules = solv->nrules;
  if (!pool->installed)
    {
      solv->choicerules_end = solv->nrules;
      return;
    }
  solv->choicerules_ref = sat_calloc(solv->rpmrules_end, sizeof(Id));
  queue_init(&q);
  queue_init(&qi);
  map_init(&m, pool->nsolvables);
  map_init(&mneg, pool->nsolvables);
  /* set up negative assertion map from infarch and dup rules */
  for (rid = solv->infarchrules, r = solv->rules + rid; rid < solv->infarchrules_end; rid++, r++)
    if (r->p < 0 && !r->w2 && (r->d == 0 || r->d == -1))
      MAPSET(&mneg, -r->p);
  for (rid = solv->duprules, r = solv->rules + rid; rid < solv->duprules_end; rid++, r++)
    if (r->p < 0 && !r->w2 && (r->d == 0 || r->d == -1))
      MAPSET(&mneg, -r->p);
  for (rid = 1; rid < solv->rpmrules_end ; rid++)
    {
      r = solv->rules + rid;
      if (r->p >= 0 || ((r->d == 0 || r->d == -1) && r->w2 < 0))
	continue;	/* only look at requires rules */
      // solver_printrule(solv, SAT_DEBUG_RESULT, r);
      queue_empty(&q);
      queue_empty(&qi);
      havechoice = 0;
      FOR_RULELITERALS(p, pp, r)
	{
	  if (p < 0)
	    continue;
	  s = pool->solvables + p;
	  if (!s->repo)
	    continue;
	  if (s->repo == pool->installed)
	    {
	      queue_push(&q, p);
	      continue;
	    }
	  /* check if this package is "blocked" by a installed package */
	  s2 = 0;
	  FOR_PROVIDES(p2, pp2, s->name)
	    {
	      s2 = pool->solvables + p2;
	      if (s2->repo != pool->installed)
		continue;
	      if (!pool->implicitobsoleteusesprovides && s->name != s2->name)
	        continue;
	      if (pool->obsoleteusescolors && !pool_colormatch(pool, s, s2))
	        continue;
	      break;
	    }
	  if (p2)
	    {
	      /* found installed package p2 that we can update to p */
	      if (!solv->allowarchchange && s->arch != s2->arch && policy_illegal_archchange(solv, s, s2))
		continue;
	      if (!solv->allowvendorchange && s->vendor != s2->vendor && policy_illegal_vendorchange(solv, s, s2))
		continue;
	      if (MAPTST(&mneg, p))
		continue;
	      queue_push(&qi, p2);
	      queue_push(&q, p);
	      continue;
	    }
	  if (s->obsoletes)
	    {
	      Id obs, *obsp = s->repo->idarraydata + s->obsoletes;
	      s2 = 0;
	      while ((obs = *obsp++) != 0)
		{
		  FOR_PROVIDES(p2, pp2, obs)
		    {
		      s2 = pool->solvables + p2;
		      if (s2->repo != pool->installed)
			continue;
		      if (!pool->obsoleteusesprovides && !pool_match_nevr(pool, pool->solvables + p2, obs))
			continue;
		      if (pool->obsoleteusescolors && !pool_colormatch(pool, s, s2))
			continue;
		      break;
		    }
		  if (p2)
		    break;
		}
	      if (obs)
		{
		  /* found installed package p2 that we can update to p */
		  if (!solv->allowarchchange && s->arch != s2->arch && policy_illegal_archchange(solv, s, s2))
		    continue;
		  if (!solv->allowvendorchange && s->vendor != s2->vendor && policy_illegal_vendorchange(solv, s, s2))
		    continue;
		  if (MAPTST(&mneg, p))
		    continue;
		  queue_push(&qi, p2);
		  queue_push(&q, p);
		  continue;
		}
	    }
	  /* package p is independent of the installed ones */
	  havechoice = 1;
	}
      if (!havechoice || !q.count)
	continue;	/* no choice */

      /* now check the update rules of the installed package.
       * if all packages of the update rules are contained in
       * the dependency rules, there's no need to set up the choice rule */
      map_empty(&m);
      FOR_RULELITERALS(p, pp, r)
        if (p > 0)
	  MAPSET(&m, p);
      for (i = 0; i < qi.count; i++)
	{
	  if (!qi.elements[i])
	    continue;
	  Rule *ur = solv->rules + solv->updaterules + (qi.elements[i] - pool->installed->start);
	  if (!ur->p)
	    ur = solv->rules + solv->featurerules + (qi.elements[i] - pool->installed->start);
	  if (!ur->p)
	    continue;
	  FOR_RULELITERALS(p, pp, ur)
	    if (!MAPTST(&m, p))
	      break;
	  if (p)
	    break;
	  for (j = i + 1; j < qi.count; j++)
	    if (qi.elements[i] == qi.elements[j])
	      qi.elements[j] = 0;
	}
      if (i == qi.count)
	{
#if 0
	  printf("skipping choice ");
	  solver_printrule(solv, SAT_DEBUG_RESULT, solv->rules + rid);
#endif
	  continue;
	}
      d = q.count ? pool_queuetowhatprovides(pool, &q) : 0;
      solver_addrule(solv, r->p, d);
      queue_push(&solv->weakruleq, solv->nrules - 1);
      solv->choicerules_ref[solv->nrules - 1 - solv->choicerules] = rid;
#if 0
      printf("OLD ");
      solver_printrule(solv, SAT_DEBUG_RESULT, solv->rules + rid);
      printf("WEAK CHOICE ");
      solver_printrule(solv, SAT_DEBUG_RESULT, solv->rules + solv->nrules - 1);
#endif
    }
  queue_free(&q);
  queue_free(&qi);
  map_free(&m);
  map_free(&mneg);
  solv->choicerules_end = solv->nrules;
}

/* called when a choice rule is disabled by analyze_unsolvable. We also
 * have to disable all other choice rules so that the best packages get
 * picked */
 
void
disablechoicerules(Solver *solv, Rule *r)
{
  Id rid, p, *pp;
  Pool *pool = solv->pool;
  Map m;
  Rule *or;

  or = solv->rules + solv->choicerules_ref[(r - solv->rules) - solv->choicerules];
  map_init(&m, pool->nsolvables);
  FOR_RULELITERALS(p, pp, or)
    if (p > 0)
      MAPSET(&m, p);
  FOR_RULELITERALS(p, pp, r)
    if (p > 0)
      MAPCLR(&m, p);
  for (rid = solv->choicerules; rid < solv->choicerules_end; rid++)
    {
      r = solv->rules + rid;
      if (r->d < 0)
	continue;
      or = solv->rules + solv->choicerules_ref[(r - solv->rules) - solv->choicerules];
      FOR_RULELITERALS(p, pp, or)
        if (p > 0 && MAPTST(&m, p))
	  break;
      if (p)
	solver_disablerule(solv, r);
    }
}

void solver_createcleandepsmap(Solver *solv)
{
  Pool *pool = solv->pool;
  Repo *installed = solv->installed;
  Queue *job = &solv->job;
  Map userinstalled;
  Map im;
  Map installedm;
  Rule *r;
  Id rid, how, what, select;
  Id p, pp, ip, *jp;
  Id req, *reqp, sup, *supp;
  Solvable *s;
  Queue iq;
  int i;

  map_init(&userinstalled, installed->end - installed->start);
  map_init(&im, pool->nsolvables);
  map_init(&installedm, pool->nsolvables);
  map_empty(&solv->cleandepsmap);
  queue_init(&iq);

  for (i = 0; i < job->count; i += 2)
    {
      how = job->elements[i];
      if ((how & SOLVER_JOBMASK) == SOLVER_USERINSTALLED)
	{
	  what = job->elements[i + 1];
	  select = how & SOLVER_SELECTMASK;
	  FOR_JOB_SELECT(p, pp, select, what)
	    if (pool->solvables[p].repo == installed)
	      MAPSET(&userinstalled, p - installed->start);
	}
    }
  /* add all positive elements (e.g. locks) to "userinstalled" */
  for (rid = solv->jobrules; rid < solv->jobrules_end; rid++)
    {
      r = solv->rules + rid;
      if (r->d < 0)
	continue;
      FOR_RULELITERALS(p, jp, r)
	if (p > 0 && pool->solvables[p].repo == installed)
	  MAPSET(&userinstalled, p - installed->start);
    }
  for (rid = solv->jobrules; rid < solv->jobrules_end; rid++)
    {
      r = solv->rules + rid;
      if (r->p >= 0 || r->d != 0)
	continue;	/* disabled or not erase */
      p = -r->p;
      if (pool->solvables[p].repo != installed)
	continue;
      MAPCLR(&userinstalled, p - installed->start);
      i = solv->ruletojob.elements[rid - solv->jobrules];
      how = job->elements[i];
      if ((how & (SOLVER_JOBMASK|SOLVER_CLEANDEPS)) == (SOLVER_ERASE|SOLVER_CLEANDEPS))
	queue_push(&iq, p);
    }
  for (p = installed->start; p < installed->end; p++)
    {
      if (pool->solvables[p].repo != installed)
	continue;
      MAPSET(&installedm, p);
      MAPSET(&im, p);
    }

  while (iq.count)
    {
      ip = queue_shift(&iq);
      if (!MAPTST(&im, ip))
	continue;
      if (!MAPTST(&installedm, ip))
	continue;
      if (MAPTST(&userinstalled, ip))
	continue;
      MAPCLR(&im, ip);
      s = pool->solvables + ip;
#ifdef CLEANDEPSDEBUG
      printf("hello %s\n", solvable2str(pool, s));
#endif
      if (s->requires)
	{
	  reqp = s->repo->idarraydata + s->requires;
	  while ((req = *reqp++) != 0)
	    {
	      if (req == SOLVABLE_PREREQMARKER)
		continue;
#if 0
	      /* count number of installed packages that match */
	      count = 0;
	      FOR_PROVIDES(p, pp, req)
		if (MAPTST(&installedm, p))
		  count++;
	      if (count > 1)
		continue;
#endif
	      FOR_PROVIDES(p, pp, req)
		{
		  if (MAPTST(&im, p))
		    {
#ifdef CLEANDEPSDEBUG
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
#if 0
	      count = 0;
	      FOR_PROVIDES(p, pp, req)
		if (MAPTST(&installedm, p))
		  count++;
	      if (count > 1)
		continue;
#endif
	      FOR_PROVIDES(p, pp, req)
		{
		  if (MAPTST(&im, p))
		    {
#ifdef CLEANDEPSDEBUG
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
	  for (ip = solv->installed->start; ip < solv->installed->end; ip++)
	    {
	      if (!MAPTST(&installedm, ip))
		continue;
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
#ifdef CLEANDEPSDEBUG
		  printf("%s supplemented\n", solvid2str(pool, ip));
#endif
		  queue_push(&iq, ip);
		}
	    }
	}
    }

  /* turn userinstalled into remove set for pruning */
  map_empty(&userinstalled);
  for (rid = solv->jobrules; rid < solv->jobrules_end; rid++)
    {
      r = solv->rules + rid;
      if (r->p >= 0 || r->d != 0)
	continue;	/* disabled or not erase */
      p = -r->p;
      MAPCLR(&im, p);
      if (pool->solvables[p].repo == installed)
        MAPSET(&userinstalled, p - installed->start);
    }
  for (p = installed->start; p < installed->end; p++)
    if (MAPTST(&im, p))
      queue_push(&iq, p);
  for (rid = solv->jobrules; rid < solv->jobrules_end; rid++)
    {
      r = solv->rules + rid;
      if (r->d < 0)
	continue;
      FOR_RULELITERALS(p, jp, r)
	if (p > 0)
          queue_push(&iq, p);
    }
  /* also put directly addressed packages on the install queue
   * so we can mark patterns as installed */
  for (i = 0; i < job->count; i += 2)
    {
      how = job->elements[i];
      if ((how & SOLVER_JOBMASK) == SOLVER_USERINSTALLED)
	{
	  what = job->elements[i + 1];
	  select = how & SOLVER_SELECTMASK;
	  if (select == SOLVER_SOLVABLE && pool->solvables[what].repo != installed)
            queue_push(&iq, what);
	}
    }
  while (iq.count)
    {
      ip = queue_shift(&iq);
      s = pool->solvables + ip;
#ifdef CLEANDEPSDEBUG
      printf("bye %s\n", solvable2str(pool, s));
#endif
      if (s->requires)
	{
	  reqp = s->repo->idarraydata + s->requires;
	  while ((req = *reqp++) != 0)
	    {
	      FOR_PROVIDES(p, pp, req)
		{
		  if (!MAPTST(&im, p) && MAPTST(&installedm, p))
		    {
		      if (p == ip)
			continue;
#ifdef CLEANDEPSDEBUG
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
		  if (!MAPTST(&im, p) && MAPTST(&installedm, p))
		    {
		      if (p == ip)
			continue;
		      if (MAPTST(&userinstalled, p - installed->start))
			continue;
#ifdef CLEANDEPSDEBUG
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
	  for (ip = installed->start; ip < installed->end; ip++)
	    {
	      if (!MAPTST(&installedm, ip))
		continue;
	      if (MAPTST(&userinstalled, ip - installed->start))
	        continue;
	      s = pool->solvables + ip;
	      if (!s->supplements)
		continue;
	      if (MAPTST(&im, ip) || !MAPTST(&installedm, ip))
		continue;
	      supp = s->repo->idarraydata + s->supplements;
	      while ((sup = *supp++) != 0)
		if (dep_possible(solv, sup, &im))
		  break;
	      if (sup)
		{
#ifdef CLEANDEPSDEBUG
		  printf("%s supplemented\n", solvid2str(pool, ip));
#endif
		  MAPSET(&im, ip);
		  queue_push(&iq, ip);
		}
	    }
	}
    }
    
  queue_free(&iq);
  for (p = installed->start; p < installed->end; p++)
    {
      if (pool->solvables[p].repo != installed)
	continue;
      if (!MAPTST(&im, p))
        MAPSET(&solv->cleandepsmap, p - installed->start);
    }
  map_free(&im);
  map_free(&installedm);
  map_free(&userinstalled);
}


/* EOF */
