/*
 * Copyright (c) 2011, Novell Inc.
 *
 * This program is licensed under the BSD license, read LICENSE.BSD
 * for further information
 */

/*
 * solver_private.h - private functions
 *
 */

#ifndef LIBSOLV_SOLVER_PRIVATE_H
#define LIBSOLV_SOLVER_PRIVATE_H

extern void solver_run_sat(Solver *solv, int disablerules, int doweak);
extern void solver_reset(Solver *solv);

extern int solver_splitprovides(Solver *solv, Id dep, Map *m);

static inline int
solver_dep_fulfilled(Solver *solv, Id dep)
{
  Pool *pool = solv->pool;
  Id p, pp;

  if (ISRELDEP(dep))
    {
      Reldep *rd = GETRELDEP(pool, dep);
      if (rd->flags == REL_COND)
	{
	  if (ISRELDEP(rd->evr))
	    {
	      Reldep *rd2 = GETRELDEP(pool, rd->evr);
	      if (rd2->flags == REL_ELSE)
		{
		  if (solver_dep_fulfilled(solv, rd2->name))
		    return solver_dep_fulfilled(solv, rd->name);
		  return solver_dep_fulfilled(solv, rd2->evr);
		}
	    }
          if (solver_dep_fulfilled(solv, rd->name))
	    return 1;
	  return !solver_dep_fulfilled(solv, rd->evr);
	}
      if (rd->flags == REL_UNLESS)
	{
	  if (ISRELDEP(rd->evr))
	    {
	      Reldep *rd2 = GETRELDEP(pool, rd->evr);
	      if (rd2->flags == REL_ELSE)
		{
		  if (!solver_dep_fulfilled(solv, rd2->name))
		    return solver_dep_fulfilled(solv, rd->name);
		  return solver_dep_fulfilled(solv, rd2->evr);
		}
	    }
          if (!solver_dep_fulfilled(solv, rd->name))
	    return 0;
	  return !solver_dep_fulfilled(solv, rd->evr);
	}
      if (rd->flags == REL_AND)
        {
          if (!solver_dep_fulfilled(solv, rd->name))
            return 0;
          return solver_dep_fulfilled(solv, rd->evr);
        }
      if (rd->flags == REL_OR)
	{
          if (solver_dep_fulfilled(solv, rd->name))
	    return 1;
          return solver_dep_fulfilled(solv, rd->evr);
	}
      if (rd->flags == REL_NAMESPACE && rd->name == NAMESPACE_SPLITPROVIDES)
        return solver_splitprovides(solv, rd->evr, 0);
    }
  FOR_PROVIDES(p, pp, dep)
    {
      if (solv->decisionmap[p] > 0)
        return 1;
    }
  return 0;
}

static inline int
solver_is_supplementing(Solver *solv, Solvable *s)
{
  Id sup, *supp;
  if (!s->supplements)
    return 0;
  supp = s->repo->idarraydata + s->supplements;
  while ((sup = *supp++) != 0)
    if (solver_dep_fulfilled(solv, sup))
      return 1;
  return 0;
}

static inline int
solver_is_enhancing(Solver *solv, Solvable *s)
{
  Id enh, *enhp;
  if (!s->enhances)
    return 0;
  enhp = s->repo->idarraydata + s->enhances;
  while ((enh = *enhp++) != 0)
    if (solver_dep_fulfilled(solv, enh))
      return 1;
  return 0;
}

#endif /* LIBSOLV_SOLVER_PRIVATE_H */
