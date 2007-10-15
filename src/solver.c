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

#define RULES_BLOCK 63

static Pool *prune_best_version_arch_sortcmp_data;

/*-----------------------------------------------------------------*/

/*
 * prep for prune_best_version_arch
 *   sort by name
 */

static int
prune_best_version_arch_sortcmp(const void *ap, const void *bp)
{
  Pool *pool = prune_best_version_arch_sortcmp_data;
  Id a = *(Id *)ap;
  Id b = *(Id *)bp;
  return pool->solvables[a].name - pool->solvables[b].name;
}


#if 0
static Id
replaces_system(Solver *solv, Id id)
{
  Pool *pool = solv->pool;
  Source *system = solv->system;
  Id *name = pool->solvables[id].name;

  FOR_PROVIDES(p, pp, id)
    {
      s = pool->solvables + p;
      if (s->name != name)
	continue;
      if (p >= system->start && p < system->start + system->nsolvables)
	return p;
    }
}
#endif

static int
dep_installed(Solver *solv, Id dep)
{
  Pool *pool = solv->pool;
  Id p, *pp;

  if (ISRELDEP(dep))
    {
      Reldep *rd = GETRELDEP(pool, dep);
      if (rd->flags == REL_AND)
	{
	  if (!dep_installed(solv, rd->name))
	    return 0;
	  return dep_installed(solv, rd->evr);
	}
      if (rd->flags == REL_NAMESPACE && rd->name == NAMESPACE_INSTALLED)
	return dep_installed(solv, rd->evr);
    }
  FOR_PROVIDES(p, pp, dep)
    {
      if (p >= solv->system->start && p < solv->system->start + solv->system->nsolvables)
	return 1;
    }
  return 0;
}

static inline int
dep_fulfilled(Solver *solv, Id dep)
{
  Pool *pool = solv->pool;
  Id p, *pp;

  if (ISRELDEP(dep))
    {
      Reldep *rd = GETRELDEP(pool, dep);
      if (rd->flags == REL_AND)
	{
	  if (!dep_fulfilled(solv, rd->name))
	    return 0;
	  return dep_fulfilled(solv, rd->evr);
	}
      if (rd->flags == REL_NAMESPACE && rd->name == NAMESPACE_INSTALLED)
	return dep_installed(solv, rd->evr);
    }
  FOR_PROVIDES(p, pp, dep)
    {
      if (solv->decisionmap[p] > 0)
	return 1;
    }
  return 0;
}

/*
 * prune_to_recommended
 *
 */
static void
prune_to_recommended(Solver *solv, Queue *plist)
{
  Pool *pool = solv->pool;
  int i, j;
  Solvable *s;
  Id p, *pp, sup, *supp, rec, *recp, sug, *sugp, enh, *enhp;

  if (solv->recommends_index < 0)
    {
      MAPZERO(&solv->recommends);
      MAPZERO(&solv->suggests);
      solv->recommends_index = 0;
    }
  while (solv->recommends_index < solv->decisionq.count)
    {
      p = solv->decisionq.elements[solv->recommends_index++];
      if (p < 0)
	continue;
      s = pool->solvables + p;
      if ((recp = s->recommends) != 0)
        while ((rec = *recp++) != 0)
	  FOR_PROVIDES(p, pp, rec)
	    MAPSET(&solv->recommends, p);
      if ((sugp = s->suggests) != 0)
        while ((sug = *sugp++) != 0)
	  FOR_PROVIDES(p, pp, sug)
	    MAPSET(&solv->suggests, p);
    }
  /* prune to recommended/supplemented */
  for (i = j = 0; i < plist->count; i++)
    {
      p = plist->elements[i];
      if (MAPTST(&solv->recommends, p))
	{
	  plist->elements[j++] = p;
	  continue;
	}
      s = pool->solvables + p;
      if (!s->supplements && !s->freshens)
	continue;
      if ((supp = s->supplements) != 0)
	{
	  while ((sup = *supp++) != 0)
	    if (dep_fulfilled(solv, sup))
	      break;
	  if (!sup)
	    continue;
	}
      if ((supp = s->freshens) != 0)
	{
	  while ((sup = *supp++) != 0)
	    if (dep_fulfilled(solv, sup))
	      break;
	  if (!sup)
	    continue;
	}
      plist->elements[j++] = s - pool->solvables;
    }
  if (j)
    plist->count = j;

  /* prune to suggested/enhanced*/
  if (plist->count < 2)
    return;
  for (i = j = 0; i < plist->count; i++)
    {
      p = plist->elements[i];
      if (MAPTST(&solv->suggests, p))
	{
	  plist->elements[j++] = p;
	  continue;
	}
      s = pool->solvables + p;
      if (!(enhp = s->enhances))
	continue;
      while ((enh = *enhp++) != 0)
	if (dep_fulfilled(solv, enh))
	  break;
      if (!enh)
	continue;
      plist->elements[j++] = s - pool->solvables;
    }
  if (j)
    plist->count = j;
}

/*
 * prune_best_version_arch
 * 
 * sort list of packages (given through plist) by name and evr
 * return result through plist
 * 
 */

/* FIXME: must also look at update packages */

void
prune_best_version_arch(Pool *pool, Queue *plist)
{
  Id best = ID_NULL;
  int i, j;
  Solvable *s;
  Id a, bestscore;

  if (plist->count < 2)		/* no need to prune for a single entry */
    return;
  if (pool->verbose) printf("prune_best_version_arch %d\n", plist->count);

  /* prune to best architecture */
  if (pool->id2arch)
    {
      bestscore = 0;
      for (i = 0; i < plist->count; i++)
	{
	  s = pool->solvables + plist->elements[i];
	  a = s->arch;
	  if (a > pool->lastarch)
	    continue;
	  a = pool->id2arch[a];
	  if (!bestscore || (a & 0xffff0000) < bestscore)
	    bestscore = a & 0xffff0000;
	}
      for (i = j = 0; i < plist->count; i++)
	{
	  s = pool->solvables + plist->elements[i];
	  a = s->arch;
	  if (a > pool->lastarch)
	    continue;
	  a = pool->id2arch[a];
	  /* a == 1 -> noarch */
	  if (a != 1 && (a & 0xffff0000) != bestscore)
	    continue;
	  plist->elements[j++] = plist->elements[i];
	}
      plist->count = j;
      if (j == 0)
	return;
    }

  prune_best_version_arch_sortcmp_data = pool;
  /* sort by name first */
  qsort(plist->elements, plist->count, sizeof(Id), prune_best_version_arch_sortcmp);

  /* now find best 'per name' */
  for (i = j = 0; i < plist->count; i++)
    {
      s = pool->solvables + plist->elements[i];
      if (s->arch == ARCH_SRC || s->arch == ARCH_NOSRC)
	continue;

      if (pool->verbose) printf("- %s-%s.%s\n", id2str(pool, s->name), id2str(pool, s->evr), id2str(pool, s->arch));

      if (!best)		       /* if no best yet, the current is best */
        {
          best = plist->elements[i];
          continue;
        }

      /* name switch: re-init */
      if (pool->solvables[best].name != s->name)   /* new name */
        {
          if (pool->verbose) printf("BEST: %s-%s.%s\n", id2str(pool, pool->solvables[best].name), id2str(pool, pool->solvables[best].evr), id2str(pool, pool->solvables[best].arch));
          plist->elements[j++] = best; /* move old best to front */
          best = plist->elements[i];   /* take current as new best */
          continue;
        }

      if (pool->solvables[best].evr != s->evr)   /* compare evr */
        {
          if (evrcmp(pool, pool->solvables[best].evr, s->evr) < 0)
            best = plist->elements[i];
        }
    }

  if (best == ID_NULL)
    best = plist->elements[0];

  /* XXX also check obsoletes! */
  if (pool->verbose) printf("BEST: %s-%s.%s\n", id2str(pool, pool->solvables[best].name), id2str(pool, pool->solvables[best].evr), id2str(pool, pool->solvables[best].arch));

  plist->elements[j++] = best;
  plist->count = j;

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
    printf(" I.%d", solv->decisionmap[s - pool->solvables]);
  if (solv->decisionmap[s - pool->solvables] < 0)
    printf(" C.%d", -solv->decisionmap[s - pool->solvables]);
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
  for (; *ad && *ad == *bd; ad++, bd++)
    ;
  return *ad - *bd;
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
  {
    int binr = 0;
    int dc = 0;
    Id *dp;
    Rule *r;

    for (i = 1; i < solv->nrules; i++)
      {
	r = solv->rules + i;
	if (r->d == 0)		       /* assertion */
	  {
	    binr++;
	    continue;
	  }
        dp = solv->pool->whatprovidesdata + r->d;
	while (*dp++)
	  dc++;
      }
    if (solv->pool->verbose)
      {
        printf("  binary: %d\n", binr);
        printf("  normal: %d\n", solv->nrules - 1 - binr);
        printf("  normal lits: %d\n", dc);
      }
  }
#endif
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
 *  p = direct literal; > 0 for learnt, < 0 for installed pkg (rpm)
 *  d, if < 0 direct literal, if > 0 offset into whatprovides, if == 0 rule is assertion (look at p only)
 *
 *
 * A requires b, b provided by B1,B2,B3 => (-A|B1|B2|B3)
 * 
 * p < 0 : rule from rpm (installed pkg)
 * d > 0 : Offset in whatprovidesdata (list of providers)
 * 
 * A conflicts b, b provided by B1,B2,B3 => (-A|-B1), (-A|-B2), (-A|-B3)
 *  d < 0: Id of solvable (e.g. B1)
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
 */

static Rule *
addrule(Solver *solv, Id p, Id d)
{
  Rule *r = NULL;
  Id *dp = NULL;

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
      if (p == d)
	return NULL;		       /* ignore self conflict */
      n = 1;
    }
  else if (d == 0)		       /* user requested */
    n = 0;
  else
    {
      for (dp = solv->pool->whatprovidesdata + d; *dp; dp++, n++)
	if (*dp == -p)
	  return NULL;	/* rule is self-fulfilling */
      if (n == 1)
	d = dp[-1];
    }

  if (n == 0)			       /* direct assertion */
    {
      if (!solv->jobrules)
	{
	  /* this is a rpm rule assertion, we do not have to allocate it */
          /* we can identify it by looking at the decision level, it will be 1 */
	  if (p > 0)		           /*  */
	    abort();
	  if (solv->decisionmap[-p] > 0)   /*  */
	    abort();
	  if (solv->decisionmap[-p])       /*  */
	    return NULL;
	  queuepush(&solv->decisionq, p);
	  queuepush(&solv->decisionq_why, 0);
	  solv->decisionmap[-p] = -1;

	  return NULL;
	}
    }
  else if (n == 1 && p > d)
    {
      /* smallest literal first so we can find dups */
      n = p;
      p = d;
      d = n;
      n = 1;			       /* re-set n, was used as temp var */
    }

  if (r
      && n == 1
      && r->p == p
      && r->w2 == d)
  {
    return r;
  }

  if (r
      && r->d
      && n > 1
      && r->p == p)
    {
      Id *dp2 = solv->pool->whatprovidesdata + r->d;
      for (dp = solv->pool->whatprovidesdata + d; *dp; dp++, dp2++)
      {
	if (*dp != *dp2)
	  break;
      }
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

  /* we don't add the decision for learnt rules, as the code does that
   * right after calling addrule anyway */
  if (n == 0
      && p
      && !solv->learntrules)
    {
      /* must be system or job rule, as there are only negative unary rpm rules */
      Id vv = p > 0 ? p : -p;
      if (solv->decisionmap[vv])
	{
	  int i;
	  if (solv->decisionmap[vv] > 0 && p > 0)
	    return r;
	  if (solv->decisionmap[vv] < 0 && p < 0)
	    return r;
	  /* direct conflict! */
	  for (i = 0; i < solv->decisionq.count; i++)
	    if (solv->decisionq.elements[i] == -p)
	      break;
	  if (i == solv->decisionq.count)
	    abort();
	  if (solv->decisionq_why.elements[i] == 0)
	    {
	      /* conflict with rpm rule */
	      queuepush(&solv->problems, r - solv->rules);
	      queuepush(&solv->problems, 0);
	      r->w1 = 0;	/* disable */
	      return r;
	    }
	  /* conflict with other job or system rule */
	  queuepush(&solv->problems, solv->decisionq_why.elements[i]);
	  queuepush(&solv->problems, r - solv->rules);
	  queuepush(&solv->problems, 0);
	  r->w1 = 0;	/* disable */
          /* also disable conflicting rule */
	  solv->rules[solv->decisionq_why.elements[i]].w1 = 0;
	  /* XXX: remove from decisionq! */
printf("XXX remove from decisionq\n");
	  return r;
	}
      queuepush(&solv->decisionq, p);
      queuepush(&solv->decisionq_why, r - solv->rules);
      solv->decisionmap[p > 0 ? p : -p] = p > 0 ? 1 : -1;
    }
  return r;
}


/*
 * add (install) rules for solvable
 * 
 */

static void
addrulesforsolvable(Solver *solv, Solvable *s, Map *m)
{
  Pool *pool = solv->pool;
  Source *system = solv->system;
  Queue q;
  Id qbuf[64];
  int i;
  int dontfix;
  Id req, *reqp;
  Id con, *conp;
  Id obs, *obsp;
  Id rec, *recp;
  Id p, *pp;
  Id *dp;
  Id n;

  queueinit_buffer(&q, qbuf, sizeof(qbuf)/sizeof(*qbuf));
  queuepush(&q, s - pool->solvables);	/* push solvable Id */

  while (q.count)
    {
      /*
       * n: Id of solvable
       * s: Pointer to solvable
       */
      
      n = queueshift(&q);
      if (MAPTST(m, n))		       /* continue if already set in map */
	continue;

      MAPSET(m, n);
      s = pool->solvables + n;	       /* s = Solvable in question */

      dontfix = 0;
      if (system		       /* have rpm */
	  && !solv->fixsystem
	  && n >= system->start	       /* its an rpm rule */
	  && n < system->start + system->nsolvables)
      {
	dontfix = 1;		       /* dont care about broken rpm deps */
      }

      /*-----------------------------------------
       * check requires of s
       */
      
      if ((reqp = s->requires) != ID_NULL)
	{
	  while ((req = *reqp++) != ID_NULL)
	    {
	      if (req == SOLVABLE_PREREQMARKER)   /* skip the marker */
		continue;

	      dp = GET_PROVIDESP(req, p);      /* get providers of req */

	      if (!*dp		             /* dont care if noone provides rpmlib() */
		  && !strncmp(id2str(pool, req), "rpmlib(", 7))
		{
		  continue;
		}

	      if (dontfix)	       /* rpm rule, dont care about breakage */
		{
		  for (i = 0; dp[i]; i++)/* for all providers */
		    {
		      if (dp[i] >= system->start && dp[i] < system->start + system->nsolvables)
			break;	       /* provider is installed */
		    }
		  if (!dp[i])	       /* no provider found */
		    {
		      if (pool->verbose) printf("ignoring broken requires %s of system package %s-%s.%s\n", dep2str(pool, req), id2str(pool, s->name), id2str(pool, s->evr), id2str(pool, s->arch));
		      continue;
		    }
		}

	      if (!*dp)
		{
		  /* nothing provides req! */
  #if 1
		  if (pool->verbose) printf("package %s-%s.%s is not installable (%s)\n", id2str(pool, s->name), id2str(pool, s->evr), id2str(pool, s->arch), dep2str(pool, req));
  #endif
		  addrule(solv, -n, 0); /* mark requestor as uninstallable */
		  if (solv->rc_output)
		    printf(">!> !unflag %s-%s.%s[%s]\n", id2str(pool, s->name), id2str(pool, s->evr), id2str(pool, s->arch), source_name(pool_source(pool, s)));
		  continue;
		}
  #if 0
	      printf("addrule %s-%s.%s %s %d %d\n", id2str(pool, s->name), id2str(pool, s->evr), id2str(pool, s->arch), dep2str(pool, req), -n, dp - pool->whatprovidesdata);
	      for (i = 0; dp[i]; i++)
		printf("  %s-%s.%s\n", id2str(pool, pool->solvables[dp[i]].name), id2str(pool, pool->solvables[dp[i]].evr), id2str(pool, pool->solvables[dp[i]].arch));
  #endif
	      /* add 'requires' dependency */
	      addrule(solv, -n, dp - pool->whatprovidesdata);   /* rule: (-requestor|provider1|provider2|...|providerN) */

	      /* descend the dependency tree */
	      for (; *dp != ID_NULL; dp++)   /* loop through all providers */
		{
		  if (!MAPTST(m, *dp))
		    queuepush(&q, *dp);
		}

	    } /* while, requirements of n */

	} /* if, requirements */

      
      /*-----------------------------------------
       * check conflicts of s
       */
      
      if ((conp = s->conflicts) != ID_NULL)
	{
	  while ((con = *conp++) != ID_NULL)
	    {
	      FOR_PROVIDES(p, pp, con)   /* loop through all providers of this conflict */
		{
					   /* dontfix: dont care about conflicts with already installed packs */
		  if (dontfix && p >= system->start && p < system->start + system->nsolvables)
		    continue;
		  addrule(solv, -n, -p);   /* rule: -n|-p: either solvable _or_ provider of conflict */
		}
	    }
	}

      /*-----------------------------------------
       * check obsoletes if not installed
       */
      if (!system || n < system->start || n >= (system->start + system->nsolvables))
	{			       /* not installed */
	  if ((obsp = s->obsoletes) != ID_NULL)
	    {
	      while ((obs = *obsp++) != ID_NULL)
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
      if ((recp = s->recommends) != ID_NULL)
	while ((rec = *recp++) != ID_NULL)
	  {
	    FOR_PROVIDES(p, pp, rec)
	      if (!MAPTST(m, p))
	        queuepush(&q, p);
	  }
    }
  queuefree(&q);
}

static void
addrulesforsupplements(Solver *solv, Map *m)
{
  Pool *pool = solv->pool;
  Solvable *s;
  Id sup, *supp;
  Id p, *pp;
  int i, n;

  if (pool->verbose) printf("addrulesforsupplements... (%d)\n", solv->nrules);
  for (i = n = 1; n < pool->nsolvables; i++, n++)
    {
      if (i == pool->nsolvables)
	i = 1;
      if (MAPTST(m, i))
	continue;
      s = pool->solvables + i;
      if (s->arch == ARCH_SRC || s->arch == ARCH_NOSRC)
	continue;
      if (pool->id2arch && (s->arch > pool->lastarch || !pool->id2arch[s->arch]))
	continue;
      sup = 0;
      if ((supp = s->supplements) != 0)
	{
	  while ((sup = *supp++) != ID_NULL)
	    {
	      FOR_PROVIDES(p, pp, sup)
		if (MAPTST(m, p))
		  break;
	      if (p)
		break;
	    }
	}
      if (!sup && (supp = s->freshens) != 0)
	{
	  while ((sup = *supp++) != ID_NULL)
	    {
	      FOR_PROVIDES(p, pp, sup)
		if (MAPTST(m, p))
		  break;
	      if (p)
		break;
	    }
	}
      if (!sup)
	continue;
      addrulesforsolvable(solv, s, m);
      n = 0;
    }
  if (pool->verbose) printf("done. (%d)\n", solv->nrules);
}


static inline int
archchanges(Pool *pool, Solvable *s1, Solvable *s2)
{
  Id a1 = s1->arch, a2 = s2->arch;

  /* we allow changes to/from noarch */
  if (a1 == a2 || a1 == ARCH_NOARCH || a2 == ARCH_NOARCH)
    return 0;
  if (!pool->id2arch)
    return 0;
  a1 = a1 <= pool->lastarch ? pool->id2arch[a1] : 0;
  a2 = a2 <= pool->lastarch ? pool->id2arch[a2] : 0;
  if (((a1 ^ a2) & 0xffff0000) != 0)
    return 1;
  return 0;
}


static void
findupdatepackages(Solver *solv, Solvable *s, Queue *qs, Map *m, int allowdowngrade, int allowarchchange)
{
  /* system packages get a special upgrade allowed rule */
  Pool *pool = solv->pool;
  Id p, *pp, n, p2, *pp2;
  Id obs, *obsp;

  QUEUEEMPTY(qs);
  /*
   * s = solvable ptr
   * n = solvable Id
   */
  n = s - pool->solvables;
  if (m && !MAPTST(m, n))	/* add rule for s if not already done */
    addrulesforsolvable(solv, s, m);

#if defined(GNADENLOS)
  for (p = 1; p < pool->nsolvables; ++p)
    {
       if (p == n)
	 continue;

       if (s->name == pool->solvables[p].name) 
         continue;

       if ((obsp = pool->solvables[p].obsoletes) != 0)   /* provides/obsoletes combination ? */
        {
          while ((obs = *obsp++) != 0)  /* for all obsoletes */
            {
              FOR_PROVIDES(p2, pp2, obs)   /* and all matching providers of the obsoletes */
                {
                  if (p2 == n)          /* match ! */
                    break;
                }
              if (p2)                   /* match! */
                break;
            }
          if (!obs)                     /* continue if no match */
            continue;
     
         queuepush(qs, p); 
       }
    }
#endif

  /*
   * look for updates for s
   */
  FOR_PROVIDES(p, pp, s->name)	/* every provider of s' name */
    {
      if (p == n)		/* skip itself */
	continue;

      if (s->name == pool->solvables[p].name)	/* name match */
	{
	  if (!allowdowngrade			/* consider downgrades ? */
	      && evrcmp(pool, s->evr, pool->solvables[p].evr) > 0)
	    continue;
	  /* XXX */
	  if (!allowarchchange && archchanges(pool, s, pool->solvables + p))
	    continue;
	}
#if !defined(GNADENLOS)
      else if ((obsp = pool->solvables[p].obsoletes) != 0)   /* provides/obsoletes combination ? */
	{
	  while ((obs = *obsp++) != 0)	/* for all obsoletes */
	    {
	      FOR_PROVIDES(p2, pp2, obs)   /* and all matching providers of the obsoletes */
		{
		  if (p2 == n)		/* match ! */
		    break;
		}
	      if (p2)			/* match! */
		break;
	    }
	  if (!obs)			/* continue if no match */
	    continue;
	  /* here we have 'p' with a matching provides/obsoletes combination
	   * thus flagging p as a valid update candidate for s
	   */
	}
#endif
      else
        continue;
      queuepush(qs, p);

      if (m && !MAPTST(m, p))		/* mark p for install if not already done */
	addrulesforsolvable(solv, pool->solvables + p, m);
    }
}

/*
 * add rule for update
 *   (A|A1|A2|A3...)  An = update candidates for A
 * 
 * s = (installed) solvable
 * m = 'addedmap', bit set if 'install' rule for solvable exists
 */

static void
addupdaterule(Solver *solv, Solvable *s, Map *m, int allowdowngrade, int allowarchchange, int dontaddrule)
{
  /* system packages get a special upgrade allowed rule */
  Pool *pool = solv->pool;
  Id p, d;
  Rule *r;
  Queue qs;
  Id qsbuf[64];

  queueinit_buffer(&qs, qsbuf, sizeof(qsbuf)/sizeof(*qsbuf));
  findupdatepackages(solv, s, &qs, m, allowdowngrade, allowarchchange);
  p = s - pool->solvables;
  if (dontaddrule)	/* we consider update candidates but dont force them */
    {
      queuefree(&qs);
      return;
    }

  if (qs.count == 0)		       /* no updates found */
    {
#if 0
      printf("new update rule: must keep %s-%s.%s\n", id2str(pool, s->name), id2str(pool, s->evr), id2str(pool, s->arch));
#endif
      addrule(solv, p, 0);		/* request 'install' of s */
      queuefree(&qs);
      return;
    }

  d = pool_queuetowhatprovides(pool, &qs);   /* intern computed provider queue */
  queuefree(&qs);
  r = addrule(solv, p, d);	       /* allow update of s */
#if 0
  printf("new update rule ");
  if (r)
    printrule(solv, r);
#endif
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

  while (solv->propagate_index < solv->decisionq.count)
    {
      /* negative because our watches trigger if literal goes FALSE */
      pkg = -solv->decisionq.elements[solv->propagate_index++];
#if 0
  printf("popagate for decision %d level %d\n", -pkg, level);
  printruleelement(solv, 0, -pkg);
#endif
      for (rp = watches + pkg; *rp; rp = nrp)
	{
	  r = solv->rules + *rp;
#if 0
  printf("  watch triggered ");
  printrule(solv, r);
#endif
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
#if 0
		  if (p > 0)
		    printf("    -> move w%d to %s-%s.%s\n", (pkg == r->w1 ? 1 : 2), id2str(pool, pool->solvables[p].name), id2str(pool, pool->solvables[p].evr), id2str(pool, pool->solvables[p].arch));
		  else
		    printf("    -> move w%d to !%s-%s.%s\n", (pkg == r->w1 ? 1 : 2), id2str(pool, pool->solvables[-p].name), id2str(pool, pool->solvables[-p].evr), id2str(pool, pool->solvables[-p].arch));
#endif
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
#if 0
 	  printf("unit ");
 	  printrule(solv, r);
#endif
	  if (ow > 0)
            decisionmap[ow] = level;
	  else
            decisionmap[-ow] = -level;
	  queuepush(&solv->decisionq, ow);
	  queuepush(&solv->decisionq_why, r - solv->rules);
#if 0
	    {
	      Solvable *s = pool->solvables + (ow > 0 ? ow : -ow);
	      if (ow > 0)
		printf("  -> decided to install %s-%s.%s\n", id2str(pool, s->name), id2str(pool, s->evr), id2str(pool, s->arch));
	      else
		printf("  -> decided to conflict %s-%s.%s\n", id2str(pool, s->name), id2str(pool, s->evr), id2str(pool, s->arch));
	    }
#endif
	}
    }
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
 
  queueinit(&r);

  if (pool->verbose) printf("ANALYZE at %d ----------------------\n", level);
  mapinit(&seen, pool->nsolvables);
  idx = solv->decisionq.count;
  for (;;)
    {
      printrule(solv, c);
      queuepush(&solv->learnt_pool, c - solv->rules);
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
	      queuepush(&rulq, -(j + 1));
#endif
	      continue;			/* initial setting */
	    }
	  MAPSET(&seen, vv);
	  if (l == level)
	    num++;			/* need to do this one as well */
	  else
	    {
	      queuepush(&r, v);
#if 0
  printf("PUSH %d ", v);
  printruleelement(solv, 0, v);
#endif
	      if (l > rlevel)
		rlevel = l;
	    }
	}
#if 0
      printf("num = %d\n", num);
#endif
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
  if (pool->verbose)
    {
      printf("learned rule for level %d (am %d)\n", rlevel, level);
      printruleelement(solv, 0, -v);
      for (i = 0; i < r.count; i++)
        {
          v = r.elements[i];
          printruleelement(solv, 0, v);
        }
    }
  mapfree(&seen);
  queuepush(&solv->learnt_pool, 0);
#if 0
  for (i = learnt_why; solv->learnt_pool.elements[i]; i++)
    {
      printf("learnt_why ");
      printrule(solv, solv->rules + solv->learnt_pool.elements[i]);
    }
#endif
  if (why)
    *why = learnt_why;
  return rlevel;
}


/*
 * reset_solver
 * reset the solver decisions to right after the rpm rules
 */

static void
reset_solver(Solver *solv)
{
  int i;
  Id v;
  Rule *r;

  /* delete all learnt rules */
  solv->nrules = solv->learntrules;
  QUEUEEMPTY(&solv->learnt_why);
  QUEUEEMPTY(&solv->learnt_pool);

  /* redo all direct decision without the disabled rules */
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

  if (solv->pool->verbose)
    printf("decisions done reduced from %d to %d\n", solv->decisionq.count, i);

  solv->decisionq_why.count = i;
  solv->decisionq.count = i;
  solv->recommends_index = -1;
  if (i < solv->propagate_index)
    solv->propagate_index = i;
  /* make direct decisions from enabled unary rules */
  for (i = solv->jobrules, r = solv->rules + solv->jobrules; i < solv->nrules; i++, r++)
    {
      if (!r->w1 || r->w2)
	continue;
#if 0
      printrule(solv, r);
#endif
      v = r->p;
      queuepush(&solv->decisionq, v);
      queuepush(&solv->decisionq_why, r - solv->rules);
      solv->decisionmap[v > 0 ? v : -v] = v > 0 ? 1 : -1;
    }
  if (solv->pool->verbose)
    printf("decisions after adding job and system rules: %d\n", solv->decisionq.count);
  /* recreate watches */
  makewatches(solv);
}


/*
 * analyze_unsolvable_rule
 */

static void
analyze_unsolvable_rule(Solver *solv, Rule *c, int disablerules)
{
  Id why;
  int i;

  why = c - solv->rules;
#if 0
  if (why >= solv->jobrules && why < solv->systemrules)
    printf("JOB ");
  if (why >= solv->systemrules && why < solv->learntrules)
    printf("SYSTEM %d ", why - solv->systemrules);
  if (solv->learntrules && why >= solv->learntrules)
    printf("LEARNED ");
  printrule(solv, c);
#endif
  if (solv->learntrules && why >= solv->learntrules)
    {
      for (i = solv->learnt_why.elements[why - solv->learntrules]; solv->learnt_pool.elements[i]; i++)
	analyze_unsolvable_rule(solv, solv->rules + solv->learnt_pool.elements[i], disablerules);
      return;
    }
  if (why >= solv->jobrules && why < solv->learntrules)
    {
      if (disablerules)
	{
	  /* turn off rule for further analysis */
	  c->w1 = 0;
	}
      /* unify problem */
      if (solv->problems.count)
	{
	  for (i = solv->problems.count - 1; i >= 0; i--)
	    if (solv->problems.elements[i] == 0)
	      break;
	    else if (solv->problems.elements[i] == why)
	      return;
	}
      queuepush(&solv->problems, why);
    }
}


/*
 * analyze_unsolvable
 */

static void
analyze_unsolvable(Solver *solv, Rule *c, int disablerules)
{
  Pool *pool = solv->pool;
  Map seen;		/* global? */
  Id v, vv, *dp, why;
  int l, i, idx;
  Id *decisionmap = solv->decisionmap;

#if 0
  printf("ANALYZE UNSOLVABLE ----------------------\n");
#endif
  mapinit(&seen, pool->nsolvables);
  analyze_unsolvable_rule(solv, c, disablerules);
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
#if 0
	  printf("RPM ");
	  printruleelement(solv, 0, v);
#endif
	  continue;
	}
      c = solv->rules + why;
      analyze_unsolvable_rule(solv, c, disablerules);
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
	  l = solv->decisionmap[vv];
	  if (l < 0)
	    l = -l;
	  MAPSET(&seen, vv);
	}
    }
  mapfree(&seen);
  queuepush(&solv->problems, 0);	/* mark end of this problem */
  if (disablerules)
    reset_solver(solv);
#if 0
  printf("analyze_unsolvables done\n");
#endif
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
#if 0
      printf("reverting decision %d at %d\n", v, solv->decisionmap[vv]);
#endif
      solv->decisionmap[vv] = 0;
      solv->decisionq.count--;
      solv->decisionq_why.count--;
      solv->propagate_index = solv->decisionq.count;
    }
  solv->recommends_index = -1;
}


/*
 * watch2onhighest
 */

static void
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
      queuepush(&solv->decisionq, decision);
      queuepush(&solv->decisionq_why, 0);
    }
  for (;;)
    {
      r = propagate(solv, level);
      if (!r)
	break;
      if (level == 1)
	{
	  analyze_unsolvable(solv, r, disablerules);
	  if (disablerules)
	    return 1;
	  return 0;
	}
      printf("conflict with rule #%d\n", (int)(r - solv->rules));
      l = analyze(solv, level, r, &p, &d, &why);
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
      queuepush(&solv->learnt_why, why);
      if (d)
	{
	  /* at least 2 literals, needs watches */
	  watch2onhighest(solv, r);
	  addwatches(solv, r);
	}
      solv->decisionmap[p > 0 ? p : -p] = p > 0 ? level : -level;
      queuepush(&solv->decisionq, p);
      queuepush(&solv->decisionq_why, r - solv->rules);
      printf("decision: ");
      printruleelement(solv, 0, p);
      printf("new rule: ");
      printrule(solv, r);
    }
  return level;
}

/*-----------------------------------------------------------------*/
/* Main solver interface */


/*
 * solver_create
 * create solver structure
 *
 * pool: all available solvables
 * system: installed Solvables
 *
 *
 * Upon solving, rules are created to flag the Solvables
 * of the 'system' Source as installed.
 */

Solver *
solver_create(Pool *pool, Source *system)
{
  Solver *solv;
  solv = (Solver *)xcalloc(1, sizeof(Solver));
  solv->pool = pool;
  solv->system = system;
  pool->verbose = 1;

  queueinit(&solv->decisionq);
  queueinit(&solv->decisionq_why);
  queueinit(&solv->problems);
  queueinit(&solv->learnt_why);
  queueinit(&solv->learnt_pool);

  mapinit(&solv->recommends, pool->nsolvables);
  mapinit(&solv->suggests, pool->nsolvables);
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
  queuefree(&solv->decisionq);
  queuefree(&solv->decisionq_why);
  queuefree(&solv->learnt_why);
  queuefree(&solv->learnt_pool);
  mapfree(&solv->recommends);
  mapfree(&solv->suggests);
  xfree(solv->decisionmap);
  xfree(solv->rules);
  xfree(solv->watches);
  xfree(solv->weaksystemrules);
  xfree(solv);
}


/*
 * reenablerule
 * 
 * r->w1 was set to 0, now find proper value for w1
 */
  
static void
reenablerule(Solver *solv, Rule *r)
{
  int i;
  Id v, l, good;

  if (!r->w2)			       /* not a rule, but an assertion */
    {
      r->w1 = r->p;
      return;
    }
  if (!r->d)
    {
      if (r->w2 != r->p)
	r->w1 = r->p;
      else
	r->w2 = r->d;		       /* mls: shouldn't this be r->w1 ? */
      return;
    }
  good = 0;
				       /* put it on the first not-false literal */
  for (i = -1; ; i++)
    {
      if (i == -1)
	v = r->p;
      else
        v = solv->pool->whatprovidesdata[r->d + i];
      if (!v)
	{
	  printrule(solv, r);
	  abort();
	}
      if (v == r->w2)
	continue;
      l = solv->decisionmap[v > 0 ? v : -v];
      if (!l || (v < 0 && l < 0) || (v > 0 && l > 0))
	break;
    }
  r->w1 = v;
}


/*-------------------------------------------------------*/

/*
 * run_solver
 * 
 * all rules have been set up, not actually run the solver
 *
 */

static void
run_solver(Solver *solv, int disablerules, int doweak)
{
  Queue dq;
  int systemlevel;
  int level, olevel;
  Rule *r;
  int i, n;
  Solvable *s;
  Pool *pool = solv->pool;
  Id p, *dp;

#if 0
  printf("number of rules: %d\n", solv->nrules);
{
  int i;
  for (i = 0; i < solv->nrules; i++)
    {
      printrule(solv, solv->rules + i);
    }
}
#endif

  /* all new rules are learnt after this point */
  solv->learntrules = solv->nrules;
  /* crate watches lists */
  makewatches(solv);

  if (pool->verbose) printf("initial decisions: %d\n", solv->decisionq.count);

  /* start SAT algorithm */
  level = 1;
  systemlevel = level + 1;
  if (pool->verbose) printf("solving...\n");

  queueinit(&dq);
  for (;;)
    {
      /*
       * propagate
       */
      
      if (level == 1)
	{
	  if (pool->verbose) printf("propagating (%d %d)...\n", solv->propagate_index, solv->decisionq.count);
	  if ((r = propagate(solv, level)) != 0)
	    {
	      analyze_unsolvable(solv, r, disablerules);
	      if (disablerules)
		continue;
	      printf("UNSOLVABLE\n");
	      queuefree(&dq);
	      return;
	    }
	}

      /*
       * system packages
       */
      
      if (level < systemlevel && solv->system->nsolvables)
	{
	  if (!solv->updatesystem)
	    {
	      /* try to keep as many packages as possible */
	      if (pool->verbose) printf("installing system packages\n");
	      for (i = solv->system->start, n = 0; ; i++, n++)
		{
		  if (n == solv->system->nsolvables)
		    break;
		  if (i == solv->system->start + solv->system->nsolvables)
		    i = solv->system->start;
		  s = pool->solvables + i;
		  if (solv->decisionmap[i] != 0)
		    continue;
#if 0
		  printf("system installing %s-%s.%s\n", id2str(pool, s->name), id2str(pool, s->evr), id2str(pool, s->arch));
#endif
		  olevel = level;
		  level = setpropagatelearn(solv, level, i, disablerules);
		  if (level == 0)
		    {
		      printf("UNSOLVABLE\n");
		      queuefree(&dq);
		      return;
		    }
		  if (level <= olevel)
		    n = 0;
		}
	    }
	  if (solv->weaksystemrules)
	    {
	      if (pool->verbose) printf("installing weak system packages\n");
	      for (i = solv->system->start, n = 0; ; i++, n++)
		{
		  if (n == solv->system->nsolvables)
		    break;
		  if (solv->decisionmap[i] > 0 || (solv->decisionmap[i] < 0 && solv->weaksystemrules[i - solv->system->start] == 0))
		    continue;
		  QUEUEEMPTY(&dq);
		  if (solv->decisionmap[i] == 0)
		    queuepush(&dq, i);
		  if (solv->weaksystemrules[i - solv->system->start])
		    {
		      dp = pool->whatprovidesdata + solv->weaksystemrules[i - solv->system->start];
		      while ((p = *dp++) != 0)
			{
			  if (solv->decisionmap[p] > 0)
			    break;
			  if (solv->decisionmap[p] == 0)
			    queuepush(&dq, p);
			}
		      if (p)
			continue;	/* rule is already true */
		    }
		  if (!dq.count)
		    continue;

		  if (dq.count > 1)
		    prune_to_recommended(solv, &dq);
		  if (dq.count > 1)
		    prune_best_version_arch(pool, &dq);
#if 0
		  s = pool->solvables + dq.elements[0];
		  printf("weak system installing %s-%s.%s\n", id2str(pool, s->name), id2str(pool, s->evr), id2str(pool, s->arch));
#endif
		  olevel = level;
		  level = setpropagatelearn(solv, level, dq.elements[0], disablerules);
		  if (level == 0)
		    {
		      printf("UNSOLVABLE\n");
		      queuefree(&dq);
		      return;
		    }
		  if (level <= olevel)
		    {
		      n = 0;
		      break;
		    }
		}
	      if (n != solv->system->nsolvables)
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
	  QUEUEEMPTY(&dq);
	  if (r->d == 0)
	    {
	      /* binary or unary rule */
	      /* need two positive undecided literals */
	      if (r->p < 0 || r->w2 <= 0)
		continue;
	      if (solv->decisionmap[r->p] || solv->decisionmap[r->w2])
		continue;
	      queuepush(&dq, r->p);
	      queuepush(&dq, r->w2);
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
		    queuepush(&dq, r->p);
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
			queuepush(&dq, p);
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
	  prune_to_recommended(solv, &dq);
	  if (dq.count > 1)
	    prune_best_version_arch(pool, &dq);
	  p = dq.elements[dq.count - 1];
	  s = pool->solvables + p;
#if 0
	  printf("installing %s-%s.%s\n", id2str(pool, s->name), id2str(pool, s->evr), id2str(pool, s->arch));
#endif
	  olevel = level;
	  level = setpropagatelearn(solv, level, p, disablerules);
	  if (level == 0)
	    {
	      printf("UNSOLVABLE\n");
	      queuefree(&dq);
	      return;
	    }
	  if (level < systemlevel)
	    break;
	  if (level <= olevel)
	    n = 0;
	} /* for(), decide */

      if (n != solv->nrules)	/* continue if level < systemlevel */
	continue;
      
      if (doweak && !solv->problems.count)
	{
	  int qcount;

	  if (pool->verbose) printf("installing recommended packages\n");
	  QUEUEEMPTY(&dq);
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
		  if ((recp = s->recommends) != 0)
		    {
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
				queuepushunique(&dq, p);
			    }
			}
		    }
		}
	      else
		{
		  Id *supp, sup;
		  s = pool->solvables + i;
		  if (!s->supplements && !s->freshens)
		    continue;
		  if (s->arch == ARCH_SRC || s->arch == ARCH_NOSRC)
		    continue;
		  if (pool->id2arch && (s->arch > pool->lastarch || !pool->id2arch[s->arch]))
		    continue;
		  if ((supp = s->supplements) != 0)
		    {
		      while ((sup = *supp++) != 0)
			if (dep_fulfilled(solv, sup))
			  break;
		      if (!sup)
			continue;
		    }
		  if ((supp = s->freshens) != 0)
		    {
		      while ((sup = *supp++) != 0)
			if (dep_fulfilled(solv, sup))
			  break;
		      if (!sup)
			continue;
		    }
		  queuepushunique(&dq, i);
		}
	    }
	  if (dq.count)
	    {
	      prune_best_version_arch(pool, &dq);
	      p = dq.elements[dq.count - 1];
	      s = pool->solvables + p;
#if 1
	      printf("installing recommended %s-%s.%s\n", id2str(pool, s->name), id2str(pool, s->evr), id2str(pool, s->arch));
#endif
	      level = setpropagatelearn(solv, level, p, 0);
	      continue;
	    }
	}
      break;
    }
  queuefree(&dq);
}

  
/*
 * refine_suggestion
 */
  
static void
refine_suggestion(Solver *solv, Id *problem, Id sug, Queue *refined)
{
  Rule *r;
  int i, j;
  Id v;
  Queue disabled;
  int disabledcnt;

  printf("refine_suggestion start\n");
  queueinit(&disabled);
  QUEUEEMPTY(refined);
  queuepush(refined, sug);

  /* re-enable all rules but rule "sug" of the problem */
  for (i = 0; problem[i]; i++)
    {
      if (problem[i] == sug)
	continue;
      r = solv->rules + problem[i];
#if 0
      printf("enable ");
      printrule(solv, r);
#endif
      reenablerule(solv, r);
    }
  for (;;)
    {
      revert(solv, 1);		/* XXX move to reset_solver? */
      reset_solver(solv);
      QUEUEEMPTY(&solv->problems);
      run_solver(solv, 0, 0);
      if (!solv->problems.count)
	{
	  printf("no more problems!\n");
#if 0
	  printdecisions(solv);
#endif
	  break;		/* great, no more problems */
	}
      disabledcnt = disabled.count;
      for (i = 0; i < solv->problems.elements[i]; i++)
	{
	  /* ignore solutions in refined */
          v = solv->problems.elements[i];
	  for (j = 0; problem[j]; j++)
	    if (problem[j] != sug && problem[j] == v)
	      break;
	  if (problem[j])
	    continue;
	  queuepush(&disabled, v);
	}
      if (disabled.count == disabledcnt)
	{
	  /* no solution found, this was an invalid suggestion! */
	  printf("no solution found!\n");
	  for (i = 0; i < refined->count; i++)
	    reenablerule(solv, solv->rules + refined->elements[i]);
	  refined->count = 0;
	  break;
	}
      if (disabled.count == disabledcnt + 1)
	{
	  /* just one solution, add it to refined list */
	  queuepush(refined, disabled.elements[disabledcnt]);
	}
      else
	{
	  printf("##############################################   more than one solution found.\n");
#if 1
	  for (i = 0; i < solv->problems.elements[i]; i++)
	    {
	      printrule(solv, solv->rules + solv->problems.elements[i]);
	    }
	  printf("##############################################\n");
#endif
	  /* more than one solution */
	  /* for now return */
	}
      for (i = disabledcnt; i < disabled.count; i++)
	{
	  r = solv->rules + disabled.elements[i];;
	  /* disable it */
	  r->w1 = 0;
#if 0
	  printf("disable ");
	  printrule(solv, r);
#endif
	}
    }
  /* enable refined rules again */
  for (i = 0; i < disabled.count; i++)
    reenablerule(solv, solv->rules + disabled.elements[i]);
  /* disable problem rules again so that we are in the same state as before */
  for (i = 0; problem[i]; i++)
    {
      r = solv->rules + problem[i];
      r->w1 = 0;
    }
  printf("refine_suggestion end\n");
}

  
/*
 * printdecisions
 */
  
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

void
printdecisions(Solver *solv)
{
  Pool *pool = solv->pool;
  Id p, *obsoletesmap;
  int i;
  Solvable *s;

  obsoletesmap = (Id *)xcalloc(pool->nsolvables, sizeof(Id));
  for (i = 0; i < solv->decisionq.count; i++)
    {
      Id obs, *obsp;
      Id *pp, n;

      n = solv->decisionq.elements[i];
      if (n < 0)
	continue;
      if (n >= solv->system->start && n < solv->system->start + solv->system->nsolvables)
	continue;
      s = pool->solvables + n;
      if ((obsp = s->obsoletes) != 0)
	while ((obs = *obsp++) != 0)
	  FOR_PROVIDES(p, pp, obs)
	    {
	      if (p >= solv->system->start && p < solv->system->start + solv->system->nsolvables)
		{
	          obsoletesmap[p] = n;
		  obsoletesmap[n]++;
		}
	    }
      FOR_PROVIDES(p, pp, s->name)
	if (s->name == pool->solvables[p].name)
	  {
	    if (p >= solv->system->start && p < solv->system->start + solv->system->nsolvables)
	      {
	        obsoletesmap[p] = n;
	        obsoletesmap[n]++;
	      }
	  }
    }

  if (solv->rc_output)
    printf(">!> Solution #1:\n");

  int installs = 0, uninstalls = 0, upgrades = 0;
  
  /* print solvables to be erased */

  for (i = solv->system->start; i < solv->system->start + solv->system->nsolvables; i++)
    {
      if (solv->decisionmap[i] > 0)
	continue;
      if (obsoletesmap[i])
	continue;
      s = pool->solvables + i;
      if (solv->rc_output == 2)
	printf(">!> remove  %s-%s%s\n", id2str(pool, s->name), id2rc(solv, s->evr), id2str(pool, s->evr));
      else if (solv->rc_output)
	printf(">!> remove  %s-%s.%s\n", id2str(pool, s->name), id2str(pool, s->evr), id2str(pool, s->arch));
      else
	printf("erase   %s-%s.%s\n", id2str(pool, s->name), id2str(pool, s->evr), id2str(pool, s->arch));
      uninstalls++;
    }

  /* print solvables to be installed */

  for (i = 0; i < solv->decisionq.count; i++)
    {
      int j;
      p = solv->decisionq.elements[i];
      if (p < 0)
	continue;
      if (p >= solv->system->start && p < solv->system->start + solv->system->nsolvables)
	continue;
      s = pool->solvables + p;

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
	  for (j = solv->system->start; j < solv->system->start + solv->system->nsolvables; j++)
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
	  for (j = solv->system->start; j < solv->system->start + solv->system->nsolvables; j++)
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
	  Source *source = pool_source(pool, s);
	  if (source && strcmp(source_name(source), "locales"))
	    printf("[%s]", source_name(source));
        }
      printf("\n");
    }

  if (solv->rc_output)
    printf(">!> installs=%d, upgrades=%d, uninstalls=%d\n", installs, upgrades, uninstalls);
  
  xfree(obsoletesmap);
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
  int i;
  Map addedmap;			       /* '1' == have rule for solvable */
  Map noupdaterule;		       /* '1' == don't update (scheduled for removal) */
  Id how, what, p, *pp, d;
  Queue q;
  Rule *r;
  Solvable *s;

  /*
   * create basic rule set of all involved packages
   * as bitmaps
   * 
   */

  mapinit(&addedmap, pool->nsolvables);
  mapinit(&noupdaterule, pool->nsolvables);

  queueinit(&q);

  /*
   * create rules for installed solvables -> keep them installed
   * so called: rpm rules
   * 
   */

  for (i = solv->system->start; i < solv->system->start + solv->system->nsolvables; i++)
    addrulesforsolvable(solv, pool->solvables + i, &addedmap);

  /*
   * create install rules
   * 
   * two passes, as we want to keep the rpm rules distinct from the job rules
   * 
   */

  /*
   * solvable rules
   *  process job rules for solvables
   */
  
  for (i = 0; i < job->count; i += 2)
    {
      how = job->elements[i];
      what = job->elements[i + 1];

      switch(how)
	{
	case SOLVER_INSTALL_SOLVABLE:
	  addrulesforsolvable(solv, pool->solvables + what, &addedmap);
	  break;
	case SOLVER_INSTALL_SOLVABLE_NAME:
	case SOLVER_INSTALL_SOLVABLE_PROVIDES:
	  QUEUEEMPTY(&q);
	  FOR_PROVIDES(p, pp, what)
	    {
				       /* if by name, ensure that the name matches */
	      if (how == SOLVER_INSTALL_SOLVABLE_NAME && pool->solvables[p].name != what)
		continue;
	      addrulesforsolvable(solv, pool->solvables + p, &addedmap);
	    }
	  break;
	case SOLVER_INSTALL_SOLVABLE_UPDATE:
	  /* dont allow downgrade */
	  addupdaterule(solv, pool->solvables + what, &addedmap, 0, 0, 1);
	  break;
	}
    }

  /*
   * if unstalls are disallowed, add update rules for every
   * installed solvables in the hope to circumvent uninstall
   * by upgrading
   * 
   */
  
#if 0
  if (!solv->allowuninstall)
    {
      /* add update rule for every installed package */
      for (i = solv->system->start; i < solv->system->start + solv->system->nsolvables; i++)
        addupdaterule(solv, pool->solvables + i, &addedmap, solv->allowdowngrade, solv->allowarchchange, 1);
    }
#else  /* this is just to add the needed rpm rules to our set */
  for (i = solv->system->start; i < solv->system->start + solv->system->nsolvables; i++)
    addupdaterule(solv, pool->solvables + i, &addedmap, 1, 1, 1);
#endif

  addrulesforsupplements(solv, &addedmap);

  /*
   * first pass done
   * 
   * unify existing rules before going over all job rules
   * 
   */
  
  unifyrules(solv);	/* remove duplicate rpm rules */

  /*
   * at this point the system is always solvable,
   * as an empty system (remove all packages) is a valid solution
   */
  if (pool->verbose) printf("decisions based on rpms: %d\n", solv->decisionq.count);

  /*
   * now add all job rules
   */
  
  solv->jobrules = solv->nrules;

  for (i = 0; i < job->count; i += 2)
    {
      how = job->elements[i];
      what = job->elements[i + 1];
      switch(how)
	{
	case SOLVER_INSTALL_SOLVABLE:                     /* install specific solvable */
	  if (solv->rc_output) {
	    Solvable *s = pool->solvables + what;
	    printf(">!> Installing %s from channel %s\n", id2str(pool, s->name), source_name(pool_source(pool, s)));
	  }
          addrule(solv, what, 0);                         /* install by Id */
	  break;
	case SOLVER_ERASE_SOLVABLE:
          addrule(solv, -what, 0);                        /* remove by Id */
	  MAPSET(&noupdaterule, what);
	  break;
	case SOLVER_INSTALL_SOLVABLE_NAME:                /* install by capability */
	case SOLVER_INSTALL_SOLVABLE_PROVIDES:
	  QUEUEEMPTY(&q);
	  FOR_PROVIDES(p, pp, what)    /* check all providers */
	    {
				       /* if by name, ensure that the name matches */
	      if (how == SOLVER_INSTALL_SOLVABLE_NAME && pool->solvables[p].name != what)
		continue;
	      queuepush(&q, p);
	    }
	  if (!q.count)	{	       /* no provider found -> abort */
	    fprintf(stderr, "Nothing provides '%s'\n", id2str(pool, what));
	    /* XXX make this a problem! */
	    return;
	    abort();
	  }

	  p = queueshift(&q);	       /* get first provider */
	  if (!q.count)
	    d = 0;		       /* single provider ? -> make assertion */
	  else
	    d = pool_queuetowhatprovides(pool, &q);   /* get all providers */
	  addrule(solv, p, d);	       /* add 'requires' rule */
	  break;
	case SOLVER_ERASE_SOLVABLE_NAME:                  /* remove by capability */
	case SOLVER_ERASE_SOLVABLE_PROVIDES:
	  FOR_PROVIDES(p, pp, what)
	    {
				       /* if by name, ensure that the name matches */
	      if (how == SOLVER_ERASE_SOLVABLE_NAME && pool->solvables[p].name != what)
	        continue;

	      addrule(solv, -p, 0);  /* add 'remove' rule */
	      MAPSET(&noupdaterule, p);
	    }
	  break;
	case SOLVER_INSTALL_SOLVABLE_UPDATE:              /* find update for solvable */
	  addupdaterule(solv, pool->solvables + what, &addedmap, 0, 0, 0);
	  break;
	}
    }

  if (pool->verbose) printf("problems so far: %d\n", solv->problems.count);
  
  /*
   * now add policy rules
   * 
   */
  
  solv->systemrules = solv->nrules;

  /*
   * create rules for updating installed solvables
   * 
   * (Again ?)
   * 
   */
  
  if (!solv->allowuninstall)
    {				       /* loop over all installed solvables */
      for (i = solv->system->start; i < solv->system->start + solv->system->nsolvables; i++)
      {
	if (!MAPTST(&noupdaterule, i)) /* if not marked as 'noupdate' */
	  addupdaterule(solv, pool->solvables + i, &addedmap, solv->allowdowngrade, solv->allowarchchange, 0);
        else
	  addrule(solv, 0, 0);		/* place holder */
      }
      /* consistency check: we added a rule for _every_ system solvable */
      if (solv->nrules - solv->systemrules != solv->system->nsolvables)
	abort();
    }

  if (pool->verbose) printf("problems so far: %d\n", solv->problems.count);

  /* create special weak system rules */
  if (solv->system->nsolvables)
    {
      solv->weaksystemrules = xcalloc(solv->system->nsolvables, sizeof(Id));
      for (i = 0; i < solv->system->nsolvables; i++)
	{
	  findupdatepackages(solv, pool->solvables + solv->system->start + i, &q, (Map *)0, 1, 1);
	  if (q.count)
	    solv->weaksystemrules[i] = pool_queuetowhatprovides(pool, &q);
	}
    }

  /* free unneeded memory */
  mapfree(&addedmap);
  mapfree(&noupdaterule);
  queuefree(&q);

  /*
   * solve !
   * 
   */
  
  run_solver(solv, 1, 1);

  /*
   *
   * print solver result
   * 
   */

  if (pool->verbose) printf("-------------------------------------------------------------\n");

  if (solv->problems.count)
    {
      Queue problems;
      Queue solution;
      Id *problem;
      Id why;
      int j;

      if (!pool->verbose)
	return;
      clonequeue(&problems, &solv->problems);
      queueinit(&solution);
      printf("Encountered problems! Here are the solutions:\n");
      problem = problems.elements;
      for (i = 0; i < problems.count; i++)
	{
	  Id v = problems.elements[i];
	  if (v == 0)
	    {
	      printf("====================================\n");
	      problem = problems.elements + i + 1;
	      continue;
	    }
	  refine_suggestion(solv, problem, v, &solution);
	  for (j = 0; j < solution.count; j++)
	    {
	      r = solv->rules + solution.elements[j];
	      why = solution.elements[j];
#if 0
	      printrule(solv, r);
#endif
	      if (why >= solv->jobrules && why < solv->systemrules)
		{
		  /* do a sloppy job of analyzing the job rule */
		  if (r->p > 0)
		    {
		      s = pool->solvables + r->p;
		      printf("- do not install %s-%s.%s\n", id2str(pool, s->name), id2str(pool, s->evr), id2str(pool, s->arch));
		    }
		  else
		    {
		      s = pool->solvables - r->p;
		      printf("- do not erase %s-%s.%s\n", id2str(pool, s->name), id2str(pool, s->evr), id2str(pool, s->arch));
		    }
		}
	      else if (why >= solv->systemrules && why < solv->learntrules)
		{
		  Solvable *sd = 0;
		  s = pool->solvables + solv->system->start + (why - solv->systemrules);
		  if (solv->weaksystemrules && solv->weaksystemrules[why - solv->systemrules])
		    {
		      Id *dp = pool->whatprovidesdata + solv->weaksystemrules[why - solv->systemrules];
		      for (; *dp; dp++)
			if (solv->decisionmap[*dp] > 0)
			  {
			    sd = pool->solvables + *dp;
			    break;
			  }
		    }
		  if (sd)
		    {
		      printf("- allow downgrade of %s-%s.%s to %s-%s.%s\n", id2str(pool, s->name), id2str(pool, s->evr), id2str(pool, s->arch), id2str(pool, sd->name), id2str(pool, sd->evr), id2str(pool, sd->arch));
		    }
		  else
		    {
		      printf("- allow deinstallation of %s-%s.%s\n", id2str(pool, s->name), id2str(pool, s->evr), id2str(pool, s->arch));
		    }
		}
	      else
		{
		  abort();
		}
	    }
	  printf("------------------------------------\n");
	}
      return;
    }

  printdecisions(solv);
}


// EOF
