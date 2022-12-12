/*
 * Copyright (c) 2022, SUSE LLC
 *
 * This program is licensed under the BSD license, read LICENSE.BSD
 * for further information
 */

/*
 * decision.c
 *
 * solver decision introspection code
 */

#include <stdio.h>
#include <stdlib.h>
#include <unistd.h>
#include <string.h>
#include <assert.h>

#include "solver.h"
#include "solver_private.h"
#include "bitmap.h"
#include "pool.h"
#include "util.h"
#include "evr.h"

int
solver_get_decisionlevel(Solver *solv, Id p)
{
  return solv->decisionmap[p];
}

void
solver_get_decisionqueue(Solver *solv, Queue *decisionq)
{
  queue_free(decisionq);
  queue_init_clone(decisionq, &solv->decisionq);
}

int
solver_get_lastdecisionblocklevel(Solver *solv)
{
  Id p;
  if (solv->decisionq.count == 0)
    return 0;
  p = solv->decisionq.elements[solv->decisionq.count - 1];
  if (p < 0)
    p = -p;
  return solv->decisionmap[p] < 0 ? -solv->decisionmap[p] : solv->decisionmap[p];
}

void
solver_get_decisionblock(Solver *solv, int level, Queue *decisionq)
{
  Id p;
  int i;

  queue_empty(decisionq);
  for (i = 0; i < solv->decisionq.count; i++)
    {
      p = solv->decisionq.elements[i];
      if (p < 0)
	p = -p;
      if (solv->decisionmap[p] == level || solv->decisionmap[p] == -level)
        break;
    }
  if (i == solv->decisionq.count)
    return;
  for (i = 0; i < solv->decisionq.count; i++)
    {
      p = solv->decisionq.elements[i];
      if (p < 0)
	p = -p;
      if (solv->decisionmap[p] == level || solv->decisionmap[p] == -level)
        queue_push(decisionq, p);
      else
        break;
    }
}

/* return the reason and some extra info (i.e. a rule id) why
 * a package was installed/conflicted */
int
solver_describe_decision(Solver *solv, Id p, Id *infop)
{
  int i;
  Id pp, why;

  if (infop)
    *infop = 0;
  if (!solv->decisionmap[p])
    return SOLVER_REASON_UNRELATED;
  pp = solv->decisionmap[p] < 0 ? -p : p;
  for (i = 0; i < solv->decisionq.count; i++)
    if (solv->decisionq.elements[i] == pp)
      break;
  if (i == solv->decisionq.count)	/* just in case... */
    return SOLVER_REASON_UNRELATED;
  why = solv->decisionq_why.elements[i];
  if (infop)
    *infop = why >= 0 ? why : -why;
  if (why > 0)
    return SOLVER_REASON_UNIT_RULE;
  i = solv->decisionmap[p] >= 0 ? solv->decisionmap[p] : -solv->decisionmap[p];
  return solv->decisionq_reason.elements[i];
}

/* create pseudo ruleinfo elements why a package was installed if
 * the reason was SOLVER_REASON_WEAKDEP */
int
solver_allweakdepinfos(Solver *solv, Id p, Queue *whyq)
{
  Pool *pool = solv->pool;
  int i;
  int level = solv->decisionmap[p];
  int decisionno;
  Solvable *s;

  queue_empty(whyq);
  if (level < 0)
    return 0;	/* huh? */
  for (decisionno = 0; decisionno < solv->decisionq.count; decisionno++)
    if (solv->decisionq.elements[decisionno] == p)
      break;
  if (decisionno == solv->decisionq.count)
    return 0;	/* huh? */
  i = solv->decisionmap[p] >= 0 ? solv->decisionmap[p] : -solv->decisionmap[p];
  if (solv->decisionq_reason.elements[i] != SOLVER_REASON_WEAKDEP)
    return 0;	/* huh? */

  /* 1) list all packages that recommend us */
  for (i = 1; i < pool->nsolvables; i++)
    {
      Id *recp, rec, pp2, p2;
      if (solv->decisionmap[i] <= 0 || solv->decisionmap[i] >= level)
	continue;
      s = pool->solvables + i;
      if (!s->recommends)
	continue;
      if (!solv->addalreadyrecommended && s->repo == solv->installed)
	continue;
      recp = s->repo->idarraydata + s->recommends;
      while ((rec = *recp++) != 0)
	{
	  int found = 0;
	  FOR_PROVIDES(p2, pp2, rec)
	    {
	      if (p2 == p)
		found = 1;
	      else
		{
		  /* if p2 is already installed, this recommends is ignored */
		  if (solv->decisionmap[p2] > 0 && solv->decisionmap[p2] < level)
		    break;
		}
	    }
	  if (!p2 && found)
	    {
	      queue_push2(whyq, SOLVER_RULE_PKG_RECOMMENDS, i);
	      queue_push2(whyq, 0, rec);
	    }
	}
    }
  /* 2) list all supplements */
  s = pool->solvables + p;
  if (s->supplements && level > 0)
    {
      Id *supp, sup, pp2, p2;
      /* this is a hack. to use solver_dep_fulfilled we temporarily clear
       * everything above our level in the decisionmap */
      for (i = decisionno; i < solv->decisionq.count; i++ )
	{
	  p2 = solv->decisionq.elements[i];
	  if (p2 > 0)
	    solv->decisionmap[p2] = -solv->decisionmap[p2];
	}
      supp = s->repo->idarraydata + s->supplements;
      while ((sup = *supp++) != 0)
	if (solver_dep_fulfilled(solv, sup))
	  {
	    int found = 0;
	    /* let's see if this is an easy supp */
	    FOR_PROVIDES(p2, pp2, sup)
	      {
		if (!solv->addalreadyrecommended && solv->installed)
		  {
		    if (pool->solvables[p2].repo == solv->installed)
		      continue;
		  }
	        if (solv->decisionmap[p2] > 0 && solv->decisionmap[p2] < level)
		  {
		    queue_push2(whyq, SOLVER_RULE_PKG_SUPPLEMENTS, p);
		    queue_push2(whyq, p2, sup);
		    found = 1;
		  }
	      }
	    if (!found)
	      {
		/* hard case, just note dependency with no package */
		queue_push2(whyq, SOLVER_RULE_PKG_SUPPLEMENTS, p);
	        queue_push2(whyq, 0, sup);
	      }
	  }
      for (i = decisionno; i < solv->decisionq.count; i++)
	{
	  p2 = solv->decisionq.elements[i];
	  if (p2 > 0)
	    solv->decisionmap[p2] = -solv->decisionmap[p2];
	}
    }
  return whyq->count / 4;
}

SolverRuleinfo
solver_weakdepinfo(Solver *solv, Id p, Id *fromp, Id *top, Id *depp)
{
  Queue iq;
  queue_init(&iq);
  solver_allweakdepinfos(solv, p, &iq);
  if (fromp)
    *fromp = iq.count ? iq.elements[1] : 0;
  if (top)
    *top = iq.count ? iq.elements[2] : 0;
  if (depp)
    *depp = iq.count ? iq.elements[3] : 0;
  return iq.count ? iq.elements[0] : SOLVER_RULE_UNKNOWN;
}

/* deprecated, use solver_allweakdepinfos instead */
void
solver_describe_weakdep_decision(Solver *solv, Id p, Queue *whyq)
{
  int i, j;
  solver_allweakdepinfos(solv, p, whyq);
  for (i = j = 0; i < whyq->count; i += 4)
    {
      if (whyq->elements[i] == SOLVER_RULE_PKG_RECOMMENDS)
        {
	  whyq->elements[j++] = SOLVER_REASON_RECOMMENDED;
	  whyq->elements[j++] = whyq->elements[i + 1];
	  whyq->elements[j++] = whyq->elements[i + 3];
        }
      else if (whyq->elements[i] == SOLVER_RULE_PKG_SUPPLEMENTS)
	{
	  whyq->elements[j++] = SOLVER_REASON_SUPPLEMENTED;
	  whyq->elements[j++] = whyq->elements[i + 2];
	  whyq->elements[j++] = whyq->elements[i + 3];
	}
    }
  queue_truncate(whyq, j);
}

static int
decisionsort_cmp(const void *va, const void *vb, void *vd)
{
  Solver *solv = vd;
  Pool *pool = solv->pool;
  const Id *a = va, *b = vb;	/* (decision, reason, rid, bits, type, from, to, dep) */
  Solvable *as, *bs;
  if (a[4] != b[4])	/* type */
    return a[4] - b[4];
  if (a[7] != b[7])	/* dep id */
    return a[7] - b[7];
  as = pool->solvables + a[5];
  bs = pool->solvables + b[5];
  if (as->name != bs->name)
    return strcmp(pool_id2str(pool, as->name), pool_id2str(pool, bs->name));
  if (as->evr != bs->evr)
    {
      int r = pool_evrcmp(pool, as->evr, bs->evr, EVRCMP_COMPARE);
      if (r)
	return r;
    }
  as = pool->solvables + a[6];
  bs = pool->solvables + b[6];
  if (as->name != bs->name)
    return strcmp(pool_id2str(pool, as->name), pool_id2str(pool, bs->name));
  if (as->evr != bs->evr)
    {
      int r = pool_evrcmp(pool, as->evr, bs->evr, EVRCMP_COMPARE);
      if (r)
	return r;
    }
  return 0;
}

static void
decisionmerge(Solver *solv, Queue *q)
{
  Pool *pool = solv->pool;
  int i, j;

  for (i = 0; i < q->count; i += 8)
    {
      Id p = q->elements[i] >= 0 ? q->elements[i] : -q->elements[i];
      int reason = q->elements[i + 1];
      int bits = q->elements[i + 3];
      Id name =  pool->solvables[p].name;
      for (j = i + 8; j < q->count; j += 8)
	{
	  int merged;
	  p = q->elements[j] >= 0 ? q->elements[j] : -q->elements[j];
	  if (reason != q->elements[j + 1] || name != pool->solvables[p].name)
	    break;
	  merged = solver_merge_decisioninfo_bits(solv, bits, q->elements[i + 4], q->elements[i + 5], q->elements[i + 6], q->elements[i + 7], q->elements[j + 3], q->elements[j + 4], q->elements[j + 5], q->elements[j + 6], q->elements[j + 7]);
	  if (!merged)
	    break;
	  bits = merged;
	}
      j -= 8;
      for (; i < j; i += 8)
	q->elements[i + 3] = bits;
    }
}

/* move a decison from position "from" to a smaller position "to" */
static inline void
move_decision(Queue *q, int to, int from)
{
  queue_insertn(q, to, 8, 0);
  memmove(q->elements + to, q->elements + from + 8, 8 * sizeof(Id));
  queue_deleten(q, from + 8, 8); 
}

static void
solver_get_proof(Solver *solv, Id id, int flags, Queue *q)
{
  Pool *pool = solv->pool;
  Map seen, seent;	/* seent: was the literal true or false */
  Id rid, truelit;
  int first = 1;
  int why, i, j, k;
  int cnt, doing;

  queue_empty(q);
  if ((flags & SOLVER_DECISIONLIST_TYPEMASK) == SOLVER_DECISIONLIST_PROBLEM)
    why = solv->problems.elements[2 * id - 2];
  else if ((flags & SOLVER_DECISIONLIST_TYPEMASK) == SOLVER_DECISIONLIST_LEARNTRULE && id >= solv->learntrules && id < solv->nrules)
    why = solv->learnt_why.elements[id - solv->learntrules];
  else
    return;
  if (!solv->learnt_pool.elements[why])
    return;

  map_init(&seen, pool->nsolvables);
  map_init(&seent, pool->nsolvables);
  while ((rid = solv->learnt_pool.elements[why++]) != 0)
    {
      Rule *r = solv->rules + rid;
      Id p, pp;
      truelit = 0;
      FOR_RULELITERALS(p, pp, r)
	{
	  Id vv = p > 0 ? p : -p;
	  if (MAPTST(&seen, vv))
	    {
	      if ((p > 0 ? 1 : 0) == (MAPTST(&seent, vv) ? 1 : 0))
		{
		  if (truelit)
		    abort();
		  truelit = p;		/* the one true literal! */
		}
	    }
	  else
	    {
	      /* a new literal. it must be false as the rule is unit */
	      MAPSET(&seen, vv);
	      if (p < 0)
		MAPSET(&seent, vv);
	    }
	}
      if (truelit)
        queue_push(q, truelit);
      else if (!first)
	abort();
      queue_push(q, rid);
      first = 0;
    }

  /* add ruleinfo data to all rules (and also reverse the queue) */
  cnt = q->count;
  for (i = q->count - 1; i >= 0; i -= 2)
    {
      SolverRuleinfo type;
      Id from = 0, to = 0, dep = 0;
      rid = q->elements[i];
      type = solver_ruleinfo(solv, rid, &from, &to, &dep);
      if (type == SOLVER_RULE_CHOICE || type == SOLVER_RULE_RECOMMENDS)
	{
	  /* use pkg ruleinfo for choice/recommends rules */
	  Id rid2 = solver_rule2pkgrule(solv, rid);
	  if (rid2)
	    type = solver_ruleinfo(solv, rid2, &from, &to, &dep);
	}
      queue_insertn(q, q->count, 8, 0);
      q->elements[q->count - 8] = i > 0 ? q->elements[i - 1] : 0;
      q->elements[q->count - 8 + 1] = i > 0 ? SOLVER_REASON_UNIT_RULE : SOLVER_REASON_UNSOLVABLE;
      q->elements[q->count - 8 + 2] = rid;
      q->elements[q->count - 8 + 4] = type;
      q->elements[q->count - 8 + 5] = from;
      q->elements[q->count - 8 + 6] = to;
      q->elements[q->count - 8 + 7] = dep;
    }
  queue_deleten(q, 0, cnt);

  /* switch last two decisions if the unsolvable rule is of type SOLVER_RULE_RPM_SAME_NAME */
  if (q->count >= 16 && q->elements[q->count - 8 + 3] == SOLVER_RULE_RPM_SAME_NAME && q->elements[q->count - 16] > 0)
    {
      Rule *r = solv->rules + q->elements[q->count - 8 + 1];
      /* make sure that the rule is a binary conflict and it matches the installed element */
      if (r->p < 0 && (r->d == 0 || r->d == -1) && r->w2 < 0
	 && (q->elements[q->count - 16] == -r->p || q->elements[q->count - 16] -r->w2))
	{
	  /* looks good! swap decisions and fixup truelit entries */
	  move_decision(q, q->count - 16, q->count - 8);
	  q->elements[q->count - 16] = -q->elements[q->count - 8];
	  q->elements[q->count - 8] = 0;
	}
    }

  /* put learnt rule premises in front */
  MAPZERO(&seen);
  MAPSET(&seen, 1);
  i = 0;
  if ((flags & SOLVER_DECISIONLIST_TYPEMASK) == SOLVER_DECISIONLIST_LEARNTRULE)
    {
      /* insert learnt prereqs at front */
      Rule *r = solv->rules + id;
      Id p, pp;
      i = 0;
      FOR_RULELITERALS(p, pp, r)
	{
	  queue_insertn(q, i, 8, 0);
	  q->elements[i] = -p;
	  q->elements[i + 1] = SOLVER_REASON_PREMISE;
	  q->elements[i + 5] = p >= 0 ? p : -p;
	  MAPSET(&seen, p >= 0 ? p : -p);
	  i += 8;
	}
    }

  if (flags & SOLVER_DECISIONLIST_SORTED)
    {
      /* sort premise block */
      if (i > 8)
	solv_sort(q->elements, i / 8, 8 * sizeof(Id), decisionsort_cmp, solv);

      /* move rules:
       *   if a package is installed, move conflicts right after
       *   if a package is conflicted, move requires right after
       */
      doing = 1;
      while (i < q->count - 8)
	{
	  doing ^= 1;
	  for (j = k = i; j < q->count - 8; j += 8)
	    {
	      Rule *or;
	      Id p, pp;
	      Id truelit = q->elements[j];
	      if ((doing == 0 && truelit < 0) || (doing != 0 && truelit >= 0))
		continue;
	      or = solv->rules + q->elements[j + 2];
	      FOR_RULELITERALS(p, pp, or)
		if (p != truelit && !MAPTST(&seen, p >= 0 ? p : -p))
		  break;
	      if (p)
		continue;	/* not unit yet */
	      if (j > k)
		move_decision(q, k, j);
	      k += 8;
	    }
	  if (k == i)
	    continue;
	  if (i + 8 < k)
	    solv_sort(q->elements + i, (k - i) / 8, 8 * sizeof(Id), decisionsort_cmp, solv);
	  for (; i < k; i += 8)
	    {
	      Id truelit = q->elements[i];
	      MAPSET(&seen, truelit >= 0 ? truelit : -truelit);
	    }
	}
    }

  map_free(&seen);
  map_free(&seent);

  if (!(flags & SOLVER_DECISIONLIST_WITHINFO))
    {
      int j;
      for (i = j = 0; i < q->count; i += 8)
	{
	  q->elements[j++] = q->elements[i];
	  q->elements[j++] = q->elements[i + 1];
	  q->elements[j++] = q->elements[i + 2];
	}
      queue_truncate(q, j);
    }
  else
    {
      /* set decisioninfo bits */
      for (i = 0; i < q->count; i += 8)
	q->elements[i + 3] = solver_calc_decisioninfo_bits(solv, q->elements[i], q->elements[i + 4], q->elements[i + 5], q->elements[i + 6], q->elements[i + 7]);
      if (flags & SOLVER_DECISIONLIST_MERGEDINFO)
	decisionmerge(solv, q);
    }
}

void
solver_get_learnt(Solver *solv, Id id, int flags, Queue *q)
{
  int why = -1;
  Queue todo;

  queue_empty(q);
  queue_init(&todo);
  if ((flags & SOLVER_DECISIONLIST_TYPEMASK) == SOLVER_DECISIONLIST_PROBLEM)
    why = solv->problems.elements[2 * id - 2];
  else if ((flags & SOLVER_DECISIONLIST_TYPEMASK) == SOLVER_DECISIONLIST_LEARNTRULE && id >= solv->learntrules && id < solv->nrules)
    why = solv->learnt_why.elements[id - solv->learntrules];
  else if ((flags & SOLVER_DECISIONLIST_TYPEMASK) == SOLVER_DECISIONLIST_SOLVABLE)
    {
      int i, j, cnt;
      solver_get_decisionlist(solv, id, 0, &todo);
      cnt = todo.count;
      for (i = 0; i < cnt; i += 3)
	{
	  int rid = todo.elements[i + 2];
	  if (rid < solv->learntrules || rid >= solv->nrules)
	    continue;
	  /* insert sorted and unified */
	  for (j = 0; j < q->count; j++)
	    {
	      if (q->elements[j] < rid)
		continue;
	      if (q->elements[j] == rid)
		rid = 0;
	      break;
	    }
	  if (!rid)
	    continue;	/* already in list */
	  queue_insert(q, j, rid);
	  queue_push(&todo, solv->learnt_why.elements[rid - solv->learntrules]);
	}
      queue_deleten(&todo, 0, cnt);
    }
  else
    {
      queue_free(&todo);
      return;
    }
  if (why >= 0)
    queue_push(&todo, why);
  while (todo.count)
    {
      int i, rid;
      why = queue_pop(&todo);
      while ((rid = solv->learnt_pool.elements[why++]) != 0)
	{
	  if (rid < solv->learntrules || rid >= solv->nrules)
	    continue;
	  /* insert sorted and unified */
	  for (i = 0; i < q->count; i++)
	    {
	      if (q->elements[i] < rid)
		continue;
	      if (q->elements[i] == rid)
		rid = 0;
	      break;
	    }
	  if (!rid)
	    continue;	/* already in list */
	  queue_insert(q, i, rid);
	  queue_push(&todo, solv->learnt_why.elements[rid - solv->learntrules]);
	}
    }
  queue_free(&todo);
}

static void
getdecisionlist(Solver *solv, Map *dm, int flags, Queue *decisionlistq)
{
  Pool *pool = solv->pool;
  int i, ii, reason, info;
  Queue iq;

  queue_empty(decisionlistq);
  if ((flags & SOLVER_DECISIONLIST_TYPEMASK) != SOLVER_DECISIONLIST_SOLVABLE)
    return;
  queue_init(&iq);
  for (ii = solv->decisionq.count - 1; ii > 0; ii--)
    {
      Id v = solv->decisionq.elements[ii];
      Id vv = (v > 0 ? v : -v);
      if (!MAPTST(dm, vv))
	continue;
      info = solv->decisionq_why.elements[ii];
      if (info > 0)
	reason = SOLVER_REASON_UNIT_RULE;
      else if (info <= 0)
	{
	  info = -info;
	  reason = solv->decisionmap[vv];
	  reason = solv->decisionq_reason.elements[reason >= 0 ? reason : -reason];
	}
      if (flags & (SOLVER_DECISIONLIST_SORTED | SOLVER_DECISIONLIST_WITHINFO))
	{
	  queue_insertn(decisionlistq, 0, 5, 0);
	  if (reason == SOLVER_REASON_WEAKDEP)
	    {
	      solver_allweakdepinfos(solv, v, &iq);
	      if (iq.count)
		{
		  decisionlistq->elements[1] = iq.elements[0];
		  decisionlistq->elements[2] = iq.elements[1];
		  decisionlistq->elements[3] = iq.elements[2];
		  decisionlistq->elements[4] = iq.elements[3];
		}
	    }
	  else if (info > 0)
	    {
	      Id from, to, dep;
              int type = solver_ruleinfo(solv, info, &from, &to, &dep);
	      if (type == SOLVER_RULE_CHOICE || type == SOLVER_RULE_RECOMMENDS)
		{
		  /* use pkg ruleinfo for choice/recommends rules */
		  Id rid2 = solver_rule2pkgrule(solv, info);
		  if (rid2)
		    type = solver_ruleinfo(solv, rid2, &from, &to, &dep);
		}
	      decisionlistq->elements[1] = type;
	      decisionlistq->elements[2] = from;
	      decisionlistq->elements[3] = to;
	      decisionlistq->elements[4] = dep;
	    }
	}
      queue_unshift(decisionlistq, info);
      queue_unshift(decisionlistq, reason);
      queue_unshift(decisionlistq, v);
      switch (reason)
	{
	case SOLVER_REASON_WEAKDEP:
	  if (v <= 0)
	    break;
	  solver_allweakdepinfos(solv, v, &iq);
	  for (i = 0; i < iq.count; i += 4)
	    {
	      if (iq.elements[i + 1])
		MAPSET(dm, iq.elements[i + 1]);
	      if (iq.elements[i + 2])
		MAPSET(dm, iq.elements[i + 2]);
	      else if (iq.elements[i] == SOLVER_RULE_PKG_SUPPLEMENTS)
		{
		  Id p2, pp2, id = iq.elements[i + 3];
		  FOR_PROVIDES(p2, pp2, id)
		    if (solv->decisionmap[p2] > 0)
		      MAPSET(dm, p2);
		}
	    }
	  break;
	case SOLVER_REASON_RESOLVE_JOB:
	case SOLVER_REASON_UNIT_RULE:
	case SOLVER_REASON_RESOLVE:
	  solver_ruleliterals(solv, info, &iq);
	  for (i = 0; i < iq.count; i++)
	    {
	      Id p2 = iq.elements[i];
	      if (p2 < 0)
		MAPSET(dm, -p2);
	      else if (solv->decisionmap[p2] > 0)
		MAPSET(dm, p2);
	    }
	  break;
	default:
	  break;
	}
    }
  queue_free(&iq);
  if ((flags & (SOLVER_DECISIONLIST_SORTED | SOLVER_DECISIONLIST_WITHINFO)) == SOLVER_DECISIONLIST_SORTED)
    {
      int j;
      for (i = j = 0; i < decisionlistq->count; i += 8)
	{
	  decisionlistq->elements[j++] = decisionlistq->elements[i];
	  decisionlistq->elements[j++] = decisionlistq->elements[i + 1];
	  decisionlistq->elements[j++] = decisionlistq->elements[i + 2];
	}
      queue_truncate(decisionlistq, j);
    }
  else
    {
      /* set decisioninfo bits */
      for (i = 0; i < decisionlistq->count; i += 8)
	decisionlistq->elements[i + 3] = solver_calc_decisioninfo_bits(solv, decisionlistq->elements[i], decisionlistq->elements[i + 4], decisionlistq->elements[i + 5], decisionlistq->elements[i + 6], decisionlistq->elements[i + 7]);
      if (flags & SOLVER_DECISIONLIST_MERGEDINFO)
	decisionmerge(solv, decisionlistq);
    }
}

void
solver_get_decisionlist(Solver *solv, Id id, int flags, Queue *decisionlistq)
{
  Pool *pool = solv->pool;
  Map dm;
  if ((flags & SOLVER_DECISIONLIST_TYPEMASK) != SOLVER_DECISIONLIST_SOLVABLE)
    return solver_get_proof(solv, id, flags, decisionlistq);
  map_init(&dm, pool->nsolvables);
  MAPSET(&dm, id);
  getdecisionlist(solv, &dm, flags, decisionlistq);
  if (!decisionlistq->count)
    {
      queue_push(decisionlistq, -id);
      queue_push2(decisionlistq, SOLVER_REASON_UNRELATED, 0);
      if ((flags & SOLVER_DECISIONLIST_WITHINFO) != 0)
	{
	  queue_push(decisionlistq, solver_calc_decisioninfo_bits(solv, -id, 0, 0, 0, 0));
	  queue_push2(decisionlistq, 0, 0);
	  queue_push2(decisionlistq, 0, 0);
	}
    }
  map_free(&dm);
}

void
solver_get_decisionlist_multiple(Solver *solv, Queue *idq, int flags, Queue *decisionlistq)
{
  Pool *pool = solv->pool;
  int i;
  Map dm;
  queue_empty(decisionlistq);
  if ((flags & SOLVER_DECISIONLIST_TYPEMASK) != SOLVER_DECISIONLIST_SOLVABLE)
    return;
  map_init(&dm, pool->nsolvables);
  for (i = 0; i < idq->count; i++)
    {
      Id p = idq->elements[i];
      if (solv->decisionmap[p] != 0)
	MAPSET(&dm, p);
    }
  getdecisionlist(solv, &dm, flags, decisionlistq);
  for (i = 0; i < idq->count; i++)
    {
      Id p = idq->elements[i];
      if (solv->decisionmap[p] != 0)
	continue;
      queue_push(decisionlistq, -p);
      queue_push2(decisionlistq, SOLVER_REASON_UNRELATED, 0);
      if ((flags & SOLVER_DECISIONLIST_WITHINFO) != 0)
	{
	  queue_push(decisionlistq, solver_calc_decisioninfo_bits(solv, -p, 0, 0, 0, 0));
	  queue_push2(decisionlistq, 0, 0);
	  queue_push2(decisionlistq, 0, 0);
	}
    }
  map_free(&dm);
}


const char *
solver_reason2str(Solver *solv, int reason)
{
  switch(reason)
    {
    case SOLVER_REASON_WEAKDEP:
      return "a weak dependency";
    case SOLVER_REASON_RESOLVE_JOB:
      return "a job rule";
    case SOLVER_REASON_RESOLVE:
      return "a rule";
    case SOLVER_REASON_UNIT_RULE:
      return "an unit rule";
    case SOLVER_REASON_KEEP_INSTALLED:
      return "update/keep installed";
    case SOLVER_REASON_UPDATE_INSTALLED:
      return "update installed";
    case SOLVER_REASON_CLEANDEPS_ERASE:
      return "cleandeps erase";
    case SOLVER_REASON_RESOLVE_ORPHAN:
      return "orphaned package";
    case SOLVER_REASON_UNSOLVABLE:
      return "unsolvable";
    case SOLVER_REASON_PREMISE:
      return "learnt rule premise";
    case SOLVER_REASON_UNRELATED:
      return "it is unrelated";
    default:
      break;
    }
  return "an unknown reason";
}

static const char *
decisionruleinfo2str(Solver *solv, Id decision, int type, Id from, Id to, Id dep)
{
  int bits = solver_calc_decisioninfo_bits(solv, decision, type, from, to, dep);
  return solver_decisioninfo2str(solv, bits, type, from, to, dep);
}

const char *
solver_decisionreason2str(Solver *solv, Id decision, int reason, Id info)
{
  if (reason == SOLVER_REASON_WEAKDEP && decision > 0)
    {
      Id from, to, dep;
      int type = solver_weakdepinfo(solv, decision, &from, &to, &dep);
      if (type)
	return decisionruleinfo2str(solv, decision, type, from, to, dep);
    }
  if ((reason == SOLVER_REASON_RESOLVE_JOB || reason == SOLVER_REASON_UNIT_RULE || reason == SOLVER_REASON_RESOLVE || reason == SOLVER_REASON_UNSOLVABLE) && info > 0)
    {
      Id from, to, dep;
      int type = solver_ruleinfo(solv, info, &from, &to, &dep);
      if (type == SOLVER_RULE_CHOICE || type == SOLVER_RULE_RECOMMENDS)
	{
	  Id rid2 = solver_rule2pkgrule(solv, info);
	  if (rid2)
	    {
              type = solver_ruleinfo(solv, rid2, &from, &to, &dep);
	      if (type)
		return decisionruleinfo2str(solv, decision, type, from, to, dep);
            }
	}
      if (type)
	return decisionruleinfo2str(solv, decision, type, from, to, dep);
    }
  return solver_reason2str(solv, reason);
}

/* decision merge state bits */
#define DMS_INITED		(1 << 0)
#define DMS_IDENTICAL_FROM	(1 << 1)
#define DMS_IDENTICAL_TO	(1 << 2)
#define DMS_MERGED		(1 << 3)
#define DMS_NEGATIVE		(1 << 4)
#define DMS_NOMERGE		(1 << 5)

/* add some bits about the decision and the ruleinfo so we can join decisions */
int
solver_calc_decisioninfo_bits(Solver *solv, Id decision, int type, Id from, Id to, Id dep)
{
  Id decisionpkg = decision >= 0 ? decision : -decision;
  int bits = DMS_INITED | (decision < 0 ? DMS_NEGATIVE : 0);
  if (!decision)
    return bits | DMS_NOMERGE;
  switch (type)
    {
    case SOLVER_RULE_DISTUPGRADE:
    case SOLVER_RULE_INFARCH:
    case SOLVER_RULE_UPDATE:
    case SOLVER_RULE_FEATURE:
    case SOLVER_RULE_BLACK:
    case SOLVER_RULE_STRICT_REPO_PRIORITY:
    case SOLVER_RULE_PKG_NOTHING_PROVIDES_DEP:
    case SOLVER_RULE_PKG_REQUIRES:
    case SOLVER_RULE_PKG_RECOMMENDS:
    case SOLVER_RULE_PKG_SUPPLEMENTS:
      if (decisionpkg == from)
	bits |= DMS_IDENTICAL_FROM;
      break;
    case SOLVER_RULE_PKG_SAME_NAME:
    case SOLVER_RULE_PKG_CONFLICTS:
    case SOLVER_RULE_PKG_OBSOLETES:
    case SOLVER_RULE_PKG_INSTALLED_OBSOLETES:
    case SOLVER_RULE_PKG_IMPLICIT_OBSOLETES:
    case SOLVER_RULE_PKG_CONSTRAINS:
      if (decisionpkg == from)
	bits |= DMS_IDENTICAL_FROM;
      else if (decisionpkg == to)
	bits |= DMS_IDENTICAL_TO;
      break;
    default:
      break;
    }
  return bits;
}

/* try to merge the ruleinfos of two decisions */
int
solver_merge_decisioninfo_bits(Solver *solv, int bits1, int type1, Id from1, Id to1, Id dep1, int bits2, int type2, Id from2, Id to2, Id dep2)
{
  int merged = 0;
  if (type1 != type2 || dep1 != dep2)
    return 0;
  if (!bits1 || !bits2 || ((bits1 | bits2) & DMS_NOMERGE) != 0 || ((bits1 ^ bits2) & DMS_NEGATIVE) != 0)
    return 0;
  merged = (((bits1 ^ (DMS_IDENTICAL_FROM | DMS_IDENTICAL_TO)) | (bits2 ^ (DMS_IDENTICAL_FROM | DMS_IDENTICAL_TO))) ^ (DMS_IDENTICAL_FROM | DMS_IDENTICAL_TO)) | DMS_MERGED;
  if (((bits1 & DMS_MERGED) != 0 && bits1 != merged) || ((bits2 & DMS_MERGED) != 0 && bits2 != merged))
    return 0;
  if (((merged & DMS_IDENTICAL_FROM) == 0 && from1 != from2) || ((merged & DMS_IDENTICAL_TO) == 0 && to1 != to2))
    return 0;
  return merged;
}

void
solver_decisionlist_solvables(Solver *solv, Queue *decisionlistq, int pos, Queue *q)
{
  queue_empty(q);
  for (; pos < decisionlistq->count; pos += 8)
    {
      Id p = decisionlistq->elements[pos];
      queue_push(q, p > 0 ? p : -p);
      if ((decisionlistq->elements[pos + 3] & DMS_MERGED) == 0)
	break;
    }
}

int
solver_decisionlist_merged(Solver *solv, Queue *decisionlistq, int pos)
{
  int cnt = 0;
  for (; pos < decisionlistq->count; pos += 8, cnt++)
    if ((decisionlistq->elements[pos + 3] & DMS_MERGED) == 0)
      break;
  return cnt;
}

/* special version of solver_ruleinfo2str which supports merged decisions */
const char *
solver_decisioninfo2str(Solver *solv, int bits, int type, Id from, Id to, Id dep)
{
  Pool *pool = solv->pool;
  const char *s;
  int multiple = bits & DMS_MERGED;

  /* use it/they variants if DMS_IDENTICAL_FROM is set */
  if ((bits & DMS_IDENTICAL_FROM) != 0)
    {
      switch (type)
	{
	case SOLVER_RULE_DISTUPGRADE:
	  return multiple ? "they do not belong to a distupgrade repository" : "it does not belong to a distupgrade repository";
	case SOLVER_RULE_INFARCH:
	  return multiple ? "they have inferior architecture": "it has inferior architecture";
	case SOLVER_RULE_UPDATE:
	  return multiple ? "they need to stay installed or be updated" : "it needs to stay installed or be updated";
	case SOLVER_RULE_FEATURE:
	  return multiple ? "they need to stay installed or be updated/downgraded" : "it needs to stay installed or be updated/downgraded";
	case SOLVER_RULE_BLACK:
	  return multiple ? "they can only be installed by a direct request" : "it can only be installed by a direct request";
	case SOLVER_RULE_STRICT_REPO_PRIORITY:
	  return multiple ? "they are excluded by strict repo priority" : "it is excluded by strict repo priority";

	case SOLVER_RULE_PKG_NOTHING_PROVIDES_DEP:
	  return pool_tmpjoin(pool, "nothing provides ", pool_dep2str(pool, dep), 0);
	case SOLVER_RULE_PKG_REQUIRES:
	  return pool_tmpjoin(pool, multiple ? "they require " : "it requires ", pool_dep2str(pool, dep), 0);
	case SOLVER_RULE_PKG_RECOMMENDS:
	  return pool_tmpjoin(pool,  multiple ? "they recommend " : "it recommends ", pool_dep2str(pool, dep), 0);
	case SOLVER_RULE_PKG_SUPPLEMENTS:
	  s = pool_tmpjoin(pool, multiple ? "they  supplement " : "it supplements ", pool_dep2str(pool, dep), 0);
	  if (to)
	    s = pool_tmpappend(pool, s, " provided by ", pool_solvid2str(pool, to));
	  return s;
	case SOLVER_RULE_PKG_SAME_NAME:
	  return pool_tmpappend(pool, multiple ? "they have the same name as " : "it has the same name as ", pool_solvid2str(pool, to), 0);
	case SOLVER_RULE_PKG_CONFLICTS:
	  s = pool_tmpappend(pool, multiple ? "they conflict with " : "it conflicts with ", pool_dep2str(pool, dep), 0);
	  if (to)
	    s = pool_tmpappend(pool, s, " provided by ", pool_solvid2str(pool, to));
	  return s;
	case SOLVER_RULE_PKG_OBSOLETES:
	  s = pool_tmpappend(pool, multiple ? "they obsolete " : "it obsoletes ", pool_dep2str(pool, dep), 0);
	  if (to)
	    s = pool_tmpappend(pool, s, " provided by ", pool_solvid2str(pool, to));
	  return s;
	case SOLVER_RULE_PKG_INSTALLED_OBSOLETES:
	  s = pool_tmpjoin(pool, multiple ? "they are installed and obsolete " : "it is installed and obsoletes ", pool_dep2str(pool, dep), 0);
	  if (to)
	    s = pool_tmpappend(pool, s, " provided by ", pool_solvid2str(pool, to));
	  return s;
	case SOLVER_RULE_PKG_IMPLICIT_OBSOLETES:
	  s = pool_tmpjoin(pool, multiple ? "they implicitly obsolete " : "it implicitly obsoletes ", pool_dep2str(pool, dep), 0);
	  if (to)
	    s = pool_tmpappend(pool, s, " provided by ", pool_solvid2str(pool, to));
	  return s;
	case SOLVER_RULE_PKG_CONSTRAINS:
	  s = pool_tmpappend(pool, multiple ? "they have constraint " : "it has constraint ", pool_dep2str(pool, dep), 0);
	  if (to)
	    s = pool_tmpappend(pool, s, " conflicting with ", pool_solvid2str(pool, to));
	  return s;
	default:
	  break;
	}
    }

  /* in some cases we can drop the "to" part if DMS_IDENTICAL_TO is set */
  if ((bits & DMS_IDENTICAL_TO) != 0)
    {
      switch (type)
	{
	case SOLVER_RULE_PKG_SAME_NAME:
	  return pool_tmpappend(pool, multiple ? "they have the same name as " : "it has the same name as ", pool_solvid2str(pool, from), 0);
	case SOLVER_RULE_PKG_CONFLICTS:
	case SOLVER_RULE_PKG_OBSOLETES:
	case SOLVER_RULE_PKG_IMPLICIT_OBSOLETES:
	case SOLVER_RULE_PKG_INSTALLED_OBSOLETES:
	case SOLVER_RULE_PKG_CONSTRAINS:
	  bits &= ~DMS_IDENTICAL_TO;
	  to = 0;
	  break;
	default:
	  break;
	}
    }

  /* fallback to solver_ruleinfo2str if we can */
  if (multiple && (bits & (DMS_IDENTICAL_FROM|DMS_IDENTICAL_TO)) != 0)
      return "unsupported decision merge?";
  return solver_ruleinfo2str(solv, type, from, to, dep);
}
