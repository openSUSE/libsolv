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
extern int solver_dep_possible_slow(Solver *solv, Id dep, Map *m);
extern int solver_dep_fulfilled_cplx(Solver *solv, Reldep *rd);
extern int solver_is_supplementing_alreadyinstalled(Solver *solv, Solvable *s);
extern void solver_intersect_obsoleted(Solver *solv, Id p, Queue *q, int qstart, Map *m);

extern void solver_createcleandepsmap(Solver *solv, Map *cleandepsmap, int unneeded);
extern int solver_check_cleandeps_mistakes(Solver *solv);


#define ISSIMPLEDEP(pool, dep) (!ISRELDEP(dep) || GETRELDEP(pool, dep)->flags < 8)

static inline int
solver_dep_possible(Solver *solv, Id dep, Map *m)
{
  Pool *pool = solv->pool;
  Id p, pp;

  if (!ISSIMPLEDEP(pool, dep))
    return solver_dep_possible_slow(solv, dep, m);
  FOR_PROVIDES(p, pp, dep)
    {  
      if (MAPTST(m, p))
        return 1;
    }
  return 0;
}

static inline int
solver_dep_fulfilled(Solver *solv, Id dep)
{
  Pool *pool = solv->pool;
  Id p, pp;

  if (ISRELDEP(dep))
    {
      Reldep *rd = GETRELDEP(pool, dep);
      if (rd->flags == REL_COND || rd->flags == REL_UNLESS || rd->flags == REL_AND || rd->flags == REL_OR)
	return solver_dep_fulfilled_cplx(solv, rd);
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
