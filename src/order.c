/*
 * Copyright (c) 2007-2015, SUSE LLC
 *
 * This program is licensed under the BSD license, read LICENSE.BSD
 * for further information
 */

/*
 * order.c
 *
 * Transaction ordering
 */

#include <stdio.h>
#include <stdlib.h>
#include <unistd.h>
#include <string.h>
#include <assert.h>

#include "transaction.h"
#include "bitmap.h"
#include "pool.h"
#include "repo.h"
#include "util.h"

struct _TransactionElement {
  Id p;		/* solvable id */
  Id edges;	/* pointer into edges data */
  Id mark;
};

struct _TransactionOrderdata {
  struct _TransactionElement *tes;
  int ntes;
  Id *invedgedata;
  int ninvedgedata;
  Queue *cycles;
};

#define TYPE_BROKEN	(1<<0)
#define TYPE_CON    	(1<<1)

#define TYPE_REQ_P    	(1<<2)
#define TYPE_PREREQ_P 	(1<<3)

#define TYPE_REC	(1<<4)

#define TYPE_REQ    	(1<<5)
#define TYPE_PREREQ 	(1<<6)

#define TYPE_CYCLETAIL  (1<<16)
#define TYPE_CYCLEHEAD  (1<<17)

#define EDGEDATA_BLOCK	127

void
transaction_clone_orderdata(Transaction *trans, Transaction *srctrans)
{
  struct _TransactionOrderdata *od = srctrans->orderdata;
  if (!od)
    return;
  trans->orderdata = solv_calloc(1, sizeof(*trans->orderdata));
  trans->orderdata->tes = solv_memdup2(od->tes, od->ntes, sizeof(*od->tes));
  trans->orderdata->ntes = od->ntes;
  trans->orderdata->invedgedata = solv_memdup2(od->invedgedata, od->ninvedgedata, sizeof(Id));
  trans->orderdata->ninvedgedata = od->ninvedgedata;
  if (od->cycles)
    {
      trans->orderdata->cycles = solv_calloc(1, sizeof(Queue));
      queue_init_clone(trans->orderdata->cycles, od->cycles);
    }
}

void
transaction_free_orderdata(Transaction *trans)
{
  if (trans->orderdata)
    {
      struct _TransactionOrderdata *od = trans->orderdata;
      od->tes = solv_free(od->tes);
      od->invedgedata = solv_free(od->invedgedata);
      if (od->cycles)
	{
	  queue_free(od->cycles);
	  od->cycles = solv_free(od->cycles);
	}
      trans->orderdata = solv_free(trans->orderdata);
    }
}

struct orderdata {
  Transaction *trans;
  struct _TransactionElement *tes;
  int ntes;
  Id *edgedata;
  int nedgedata;
  Id *invedgedata;

  Queue cycles;
  Queue cyclesdata;
  int ncycles;
};

static int
addteedge(struct orderdata *od, int from, int to, int type)
{
  int i;
  struct _TransactionElement *te;

  if (from == to)
    return 0;

  /* printf("edge %d(%s) -> %d(%s) type %x\n", from, pool_solvid2str(pool, od->tes[from].p), to, pool_solvid2str(pool, od->tes[to].p), type); */

  te = od->tes + from;
  for (i = te->edges; od->edgedata[i]; i += 2)
    if (od->edgedata[i] == to)
      break;
  /* test of brokenness */
  if (type == TYPE_BROKEN)
    return od->edgedata[i] && (od->edgedata[i + 1] & TYPE_BROKEN) != 0 ? 1 : 0;
  if (od->edgedata[i])
    {
      od->edgedata[i + 1] |= type;
      return 0;
    }
  if (i + 1 == od->nedgedata)
    {
      /* printf("tail add %d\n", i - te->edges); */
      if (!i)
	te->edges = ++i;
      od->edgedata = solv_extend(od->edgedata, od->nedgedata, 3, sizeof(Id), EDGEDATA_BLOCK);
    }
  else
    {
      /* printf("extend %d\n", i - te->edges); */
      od->edgedata = solv_extend(od->edgedata, od->nedgedata, 3 + (i - te->edges), sizeof(Id), EDGEDATA_BLOCK);
      if (i > te->edges)
	memcpy(od->edgedata + od->nedgedata, od->edgedata + te->edges, sizeof(Id) * (i - te->edges));
      i = od->nedgedata + (i - te->edges);
      te->edges = od->nedgedata;
    }
  od->edgedata[i] = to;
  od->edgedata[i + 1] = type;
  od->edgedata[i + 2] = 0;	/* end marker */
  od->nedgedata = i + 3;
  return 0;
}

static int
addedge(struct orderdata *od, Id from, Id to, int type)
{
  Transaction *trans = od->trans;
  Pool *pool = trans->pool;
  Solvable *s;
  struct _TransactionElement *te;
  int i;

  /* printf("addedge %d %d type %d\n", from, to, type); */
  s = pool->solvables + from;
  if (s->repo == pool->installed && trans->transaction_installed[from - pool->installed->start])
    {
      /* obsolete, map to install */
      if (trans->transaction_installed[from - pool->installed->start] > 0)
	from = trans->transaction_installed[from - pool->installed->start];
      else
	{
	  int ret = 0;
	  Queue ti;
	  Id tibuf[5];

	  queue_init_buffer(&ti, tibuf, sizeof(tibuf)/sizeof(*tibuf));
	  transaction_all_obs_pkgs(trans, from, &ti);
	  for (i = 0; i < ti.count; i++)
	    ret |= addedge(od, ti.elements[i], to, type);
	  queue_free(&ti);
	  return ret;
	}
    }
  s = pool->solvables + to;
  if (s->repo == pool->installed && trans->transaction_installed[to - pool->installed->start])
    {
      /* obsolete, map to install */
      if (trans->transaction_installed[to - pool->installed->start] > 0)
	to = trans->transaction_installed[to - pool->installed->start];
      else
	{
	  int ret = 0;
	  Queue ti;
	  Id tibuf[5];

	  queue_init_buffer(&ti, tibuf, sizeof(tibuf)/sizeof(*tibuf));
	  transaction_all_obs_pkgs(trans, to, &ti);
	  for (i = 0; i < ti.count; i++)
	    ret |= addedge(od, from, ti.elements[i], type);
	  queue_free(&ti);
	  return ret;
	}
    }

  /* map from/to to te numbers */
  for (i = 1, te = od->tes + i; i < od->ntes; i++, te++)
    if (te->p == to)
      break;
  if (i == od->ntes)
    return 0;
  to = i;

  for (i = 1, te = od->tes + i; i < od->ntes; i++, te++)
    if (te->p == from)
      break;
  if (i == od->ntes)
    return 0;

  return addteedge(od, i, to, type);
}

static inline int
havescripts(Pool *pool, Id solvid)
{
  Solvable *s = pool->solvables + solvid;
  const char *dep;
  if (s->requires)
    {
      Id req, *reqp;
      int inpre = 0;
      reqp = s->repo->idarraydata + s->requires;
      while ((req = *reqp++) != 0)
	{
          if (req == SOLVABLE_PREREQMARKER)
	    {
	      inpre = 1;
	      continue;
	    }
	  if (!inpre)
	    continue;
	  dep = pool_id2str(pool, req);
	  if (*dep == '/' && strcmp(dep, "/sbin/ldconfig") != 0)
	    return 1;
	}
    }
  return 0;
}

static void
addsolvableedges(struct orderdata *od, Solvable *s)
{
  Transaction *trans = od->trans;
  Pool *pool = trans->pool;
  Id req, *reqp, con, *conp, rec, *recp;
  Id p, p2, pp2;
  int i, j, pre, numins;
  Repo *installed = pool->installed;
  Solvable *s2;
  Queue depq;
  int provbyinst;

#if 0
  printf("addsolvableedges %s\n", pool_solvable2str(pool, s));
#endif
  p = s - pool->solvables;
  queue_init(&depq);
  if (s->requires)
    {
      reqp = s->repo->idarraydata + s->requires;
      pre = TYPE_REQ;
      while ((req = *reqp++) != 0)
	{
	  if (req == SOLVABLE_PREREQMARKER)
	    {
	      pre = TYPE_PREREQ;
	      continue;
	    }
	  queue_empty(&depq);
	  numins = 0;	/* number of packages to be installed providing it */
	  provbyinst = 0;	/* provided by kept package */
	  FOR_PROVIDES(p2, pp2, req)
	    {
	      s2 = pool->solvables + p2;
	      if (p2 == p)
		{
		  depq.count = 0;	/* self provides */
		  break;
		}
	      if (s2->repo == installed && !MAPTST(&trans->transactsmap, p2))
		{
		  provbyinst = 1;
		  continue;
		}
	      if (s2->repo != installed && !MAPTST(&trans->transactsmap, p2))
		continue;		/* package stays uninstalled */

	      if (s->repo == installed)
		{
		  /* s gets uninstalled */
		  queue_pushunique(&depq, p2);
		  if (s2->repo != installed)
		    numins++;
		}
	      else
		{
		  if (s2->repo == installed)
		    continue;	/* s2 gets uninstalled */
		  queue_pushunique(&depq, p2);
		}
	    }
	  if (provbyinst)
	    {
	      /* prune to harmless ->inst edges */
	      for (i = j = 0; i < depq.count; i++)
		if (pool->solvables[depq.elements[i]].repo != installed)
		  depq.elements[j++] = depq.elements[i];
	      depq.count = j;
	    }

	  if (numins && depq.count)
	    {
	      if (s->repo == installed)
		{
		  for (i = 0; i < depq.count; i++)
		    {
		      if (pool->solvables[depq.elements[i]].repo == installed)
			{
			  for (j = 0; j < depq.count; j++)
			    {
			      if (pool->solvables[depq.elements[j]].repo != installed)
				{
				  if (trans->transaction_installed[depq.elements[i] - pool->installed->start] == depq.elements[j])
				    continue;	/* no self edge */
#if 0
				  printf("add interrreq uninst->inst edge (%s -> %s -> %s)\n", pool_solvid2str(pool, depq.elements[i]), pool_dep2str(pool, req), pool_solvid2str(pool, depq.elements[j]));
#endif
				  addedge(od, depq.elements[i], depq.elements[j], pre == TYPE_PREREQ ? TYPE_PREREQ_P : TYPE_REQ_P);
				}
			    }
			}
		    }
		}
	      /* no mixed types, remove all deps on uninstalls */
	      for (i = j = 0; i < depq.count; i++)
		if (pool->solvables[depq.elements[i]].repo != installed)
		  depq.elements[j++] = depq.elements[i];
	      depq.count = j;
	    }
          for (i = 0; i < depq.count; i++)
	    {
	      p2 = depq.elements[i];
	      if (pool->solvables[p2].repo != installed)
		{
		  /* all elements of depq are installs, thus have different TEs */
		  if (pool->solvables[p].repo != installed)
		    {
#if 0
		      printf("add inst->inst edge (%s -> %s -> %s)\n", pool_solvid2str(pool, p), pool_dep2str(pool, req), pool_solvid2str(pool, p2));
#endif
		      addedge(od, p, p2, pre);
		    }
		  else
		    {
#if 0
		      printf("add uninst->inst edge (%s -> %s -> %s)\n", pool_solvid2str(pool, p), pool_dep2str(pool, req), pool_solvid2str(pool, p2));
#endif
		      addedge(od, p, p2, pre == TYPE_PREREQ ? TYPE_PREREQ_P : TYPE_REQ_P);
		    }
		}
	      else
		{
		  if (s->repo != installed)
		    continue;	/* no inst->uninst edges, please! */

		  /* uninst -> uninst edge. Those make trouble. Only add if we must */
		  if (trans->transaction_installed[p - installed->start] && !havescripts(pool, p))
		    {
		      /* p is obsoleted by another package and has no scripts */
		      /* we assume that the obsoletor is good enough to replace p */
		      continue;
		    }
#if 0
		  printf("add uninst->uninst edge (%s -> %s -> %s)\n", pool_solvid2str(pool, p), pool_dep2str(pool, req), pool_solvid2str(pool, p2));
#endif
	          addedge(od, p2, p, pre == TYPE_PREREQ ? TYPE_PREREQ_P : TYPE_REQ_P);
		}
	    }
	}
    }
  if (s->conflicts)
    {
      conp = s->repo->idarraydata + s->conflicts;
      while ((con = *conp++) != 0)
	{
	  FOR_PROVIDES(p2, pp2, con)
	    {
	      if (p2 == p)
		continue;
	      s2 = pool->solvables + p2;
	      if (!s2->repo)
		continue;
	      if (s->repo == installed)
		{
		  if (s2->repo != installed && MAPTST(&trans->transactsmap, p2))
		    {
		      /* deinstall p before installing p2 */
#if 0
		      printf("add conflict uninst->inst edge (%s -> %s -> %s)\n", pool_solvid2str(pool, p2), pool_dep2str(pool, con), pool_solvid2str(pool, p));
#endif
		      addedge(od, p2, p, TYPE_CON);
		    }
		}
	      else
		{
		  if (s2->repo == installed && MAPTST(&trans->transactsmap, p2))
		    {
		      /* deinstall p2 before installing p */
#if 0
		      printf("add conflict uninst->inst edge (%s -> %s -> %s)\n", pool_solvid2str(pool, p), pool_dep2str(pool, con), pool_solvid2str(pool, p2));
#endif
		      addedge(od, p, p2, TYPE_CON);
		    }
		}

	    }
	}
    }
  if (s->recommends && s->repo != installed)
    {
      recp = s->repo->idarraydata + s->recommends;
      while ((rec = *recp++) != 0)
	{
	  queue_empty(&depq);
	  FOR_PROVIDES(p2, pp2, rec)
	    {
	      s2 = pool->solvables + p2;
	      if (p2 == p)
		{
		  depq.count = 0;	/* self provides */
		  break;
		}
	      if (s2->repo == installed && !MAPTST(&trans->transactsmap, p2))
		continue;
	      if (s2->repo != installed && !MAPTST(&trans->transactsmap, p2))
		continue;		/* package stays uninstalled */
	      if (s2->repo != installed)
	        queue_pushunique(&depq, p2);
	    }
          for (i = 0; i < depq.count; i++)
	    {
	      p2 = depq.elements[i];
	      if (pool->solvables[p2].repo != installed)
		{
#if 0
		  printf("add recommends inst->inst edge (%s -> %s -> %s)\n", pool_solvid2str(pool, p), pool_dep2str(pool, rec), pool_solvid2str(pool, p2));
#endif
		  addedge(od, p, p2, TYPE_REC);
		}
	    }
	}
    }
  if (s->repo == installed && solvable_lookup_idarray(s, SOLVABLE_TRIGGERS, &depq) && depq.count)
    {
      /* we're getting deinstalled/updated. Try to do this before our
       * triggers are hit */
      for (i = 0; i < depq.count; i++)
	{
	  Id tri = depq.elements[i];
	  FOR_PROVIDES(p2, pp2, tri)
	    {
	      if (p2 == p)
		continue;
	      s2 = pool->solvables + p2;
	      if (!s2->repo)
		continue;
	      if (s2->name == s->name)
		continue;	/* obsoleted anyway */
	      if (s2->repo != installed && MAPTST(&trans->transactsmap, p2))
		{
		  /* deinstall/update p before installing p2 */
#if 0
		  printf("add trigger uninst->inst edge (%s -> %s -> %s)\n", pool_solvid2str(pool, p2), pool_dep2str(pool, tri), pool_solvid2str(pool, p));
#endif
		  addedge(od, p2, p, TYPE_CON);
		}
	    }
	}
    }
  queue_free(&depq);
}


/* break an edge in a cycle */
static void
breakcycle(struct orderdata *od, Id *cycle)
{
  Pool *pool = od->trans->pool;
  Id ddegmin, ddegmax, ddeg;
  int k, l;
  struct _TransactionElement *te;

  l = 0;
  ddegmin = ddegmax = 0;
  for (k = 0; cycle[k + 1]; k += 2)
    {
      ddeg = od->edgedata[cycle[k + 1] + 1];
      if (ddeg > ddegmax)
	ddegmax = ddeg;
      if (!k || ddeg < ddegmin)
	{
	  l = k;
	  ddegmin = ddeg;
	  continue;
	}
      if (ddeg == ddegmin)
	{
	  if (havescripts(pool, od->tes[cycle[l]].p) && !havescripts(pool, od->tes[cycle[k]].p))
	    {
	      /* prefer k, as l comes from a package with contains scriptlets */
	      l = k;
	      continue;
	    }
	  /* same edge value, check for prereq */
	}
    }

  /* record brkoen cycle starting with the tail */
  queue_push(&od->cycles, od->cyclesdata.count);		/* offset into data */
  queue_push(&od->cycles, k / 2);				/* cycle elements */
  queue_push(&od->cycles, od->edgedata[cycle[l + 1] + 1]);	/* broken edge */
  queue_push(&od->cycles, (ddegmax << 16) | ddegmin);		/* max/min values */
  od->ncycles++;
  for (k = l;;)
    {
      k += 2;
      if (!cycle[k + 1])
	k = 0;
      queue_push(&od->cyclesdata, cycle[k]);
      if (k == l)
	break;
    }
  queue_push(&od->cyclesdata, 0);	/* mark end */

  /* break that edge */
  od->edgedata[cycle[l + 1] + 1] |= TYPE_BROKEN;

#if 1
  if (ddegmin < TYPE_REQ)
    return;
#endif

  /* cycle recorded, print it */
  if (ddegmin >= TYPE_REQ && (ddegmax & TYPE_PREREQ) != 0)
    POOL_DEBUG(SOLV_DEBUG_STATS, "CRITICAL ");
  POOL_DEBUG(SOLV_DEBUG_STATS, "cycle: --> ");
  for (k = 0; cycle[k + 1]; k += 2)
    {
      te = od->tes + cycle[k];
      if ((od->edgedata[cycle[k + 1] + 1] & TYPE_BROKEN) != 0)
        POOL_DEBUG(SOLV_DEBUG_STATS, "%s ##%x##> ", pool_solvid2str(pool, te->p), od->edgedata[cycle[k + 1] + 1]);
      else
        POOL_DEBUG(SOLV_DEBUG_STATS, "%s --%x--> ", pool_solvid2str(pool, te->p), od->edgedata[cycle[k + 1] + 1]);
    }
  POOL_DEBUG(SOLV_DEBUG_STATS, "\n");
}

static inline void
dump_tes(struct orderdata *od)
{
  Pool *pool = od->trans->pool;
  int i, j;
  Queue obsq;
  struct _TransactionElement *te, *te2;

  queue_init(&obsq);
  for (i = 1, te = od->tes + i; i < od->ntes; i++, te++)
    {
      Solvable *s = pool->solvables + te->p;
      POOL_DEBUG(SOLV_DEBUG_RESULT, "TE %4d: %c%s\n", i, s->repo == pool->installed ? '-' : '+', pool_solvable2str(pool, s));
      if (s->repo != pool->installed)
        {
	  queue_empty(&obsq);
	  transaction_all_obs_pkgs(od->trans, te->p, &obsq);
	  for (j = 0; j < obsq.count; j++)
	    POOL_DEBUG(SOLV_DEBUG_RESULT, "         -%s\n", pool_solvid2str(pool, obsq.elements[j]));
	}
      for (j = te->edges; od->edgedata[j]; j += 2)
	{
	  te2 = od->tes + od->edgedata[j];
	  if ((od->edgedata[j + 1] & TYPE_BROKEN) == 0)
	    POOL_DEBUG(SOLV_DEBUG_RESULT, "       --%x--> TE %4d: %s\n", od->edgedata[j + 1], od->edgedata[j], pool_solvid2str(pool, te2->p));
	  else
	    POOL_DEBUG(SOLV_DEBUG_RESULT, "       ##%x##> TE %4d: %s\n", od->edgedata[j + 1], od->edgedata[j], pool_solvid2str(pool, te2->p));
	}
    }
}

static void
reachable(struct orderdata *od, Id i)
{
  struct _TransactionElement *te = od->tes + i;
  int j, k;

  if (te->mark != 0)
    return;
  te->mark = 1;
  for (j = te->edges; (k = od->edgedata[j]) != 0; j += 2)
    {
      if ((od->edgedata[j + 1] & TYPE_BROKEN) != 0)
	continue;
      if (!od->tes[k].mark)
        reachable(od, k);
      if (od->tes[k].mark == 2)
	{
	  te->mark = 2;
	  return;
	}
    }
  te->mark = -1;
}

static void
addcycleedges(struct orderdata *od, Id *cycle, Queue *todo)
{
#if 0
  Transaction *trans = od->trans;
  Pool *pool = trans->pool;
#endif
  struct _TransactionElement *te;
  int i, j, k, tail;
  int head;

#if 0
  printf("addcycleedges\n");
  for (i = 0; (j = cycle[i]) != 0; i++)
    printf("cycle %s\n", pool_solvid2str(pool, od->tes[j].p));
#endif

  /* first add all the tail cycle edges */

  /* see what we can reach from the cycle */
  queue_empty(todo);
  for (i = 1, te = od->tes + i; i < od->ntes; i++, te++)
    te->mark = 0;
  for (i = 0; (j = cycle[i]) != 0; i++)
    {
      od->tes[j].mark = -1;
      queue_push(todo, j);
    }
  while (todo->count)
    {
      i = queue_pop(todo);
      te = od->tes + i;
      if (te->mark > 0)
	continue;
      te->mark = te->mark < 0 ? 2 : 1;
      for (j = te->edges; (k = od->edgedata[j]) != 0; j += 2)
	{
	  if ((od->edgedata[j + 1] & TYPE_BROKEN) != 0)
	    continue;
	  if (od->tes[k].mark > 0)
	    continue;	/* no need to visit again */
	  queue_push(todo, k);
	}
    }
  /* now all cycle TEs are marked with 2, all TEs reachable
   * from the cycle are marked with 1 */
  tail = cycle[0];
  od->tes[tail].mark = 1;	/* no need to add edges */

  for (i = 1, te = od->tes + i; i < od->ntes; i++, te++)
    {
      if (te->mark)
	continue;	/* reachable from cycle */
      for (j = te->edges; (k = od->edgedata[j]) != 0; j += 2)
	{
	  if ((od->edgedata[j + 1] & TYPE_BROKEN) != 0)
	    continue;
	  if (od->tes[k].mark != 2)
	    continue;
	  /* We found an edge to the cycle. Add an extra edge to the tail */
	  /* the TE was not reachable, so we're not creating a new cycle! */
#if 0
	  printf("adding TO TAIL cycle edge %d->%d %s->%s!\n", i, tail, pool_solvid2str(pool, od->tes[i].p), pool_solvid2str(pool, od->tes[tail].p));
#endif
	  j -= te->edges;	/* in case we move */
	  addteedge(od, i, tail, TYPE_CYCLETAIL);
	  j += te->edges;
	  break;	/* one edge is enough */
	}
    }

  /* now add all head cycle edges */

  /* reset marks */
  for (i = 1, te = od->tes + i; i < od->ntes; i++, te++)
    te->mark = 0;
  head = 0;
  for (i = 0; (j = cycle[i]) != 0; i++)
    {
      head = j;
      od->tes[j].mark = 2;
    }
  /* first the head to save some time */
  te = od->tes + head;
  for (j = te->edges; (k = od->edgedata[j]) != 0; j += 2)
    {
      if ((od->edgedata[j + 1] & TYPE_BROKEN) != 0)
	continue;
      if (!od->tes[k].mark)
	reachable(od, k);
      if (od->tes[k].mark == -1)
	od->tes[k].mark = -2;	/* no need for another edge */
    }
  for (i = 0; cycle[i] != 0; i++)
    {
      if (cycle[i] == head)
	break;
      te = od->tes + cycle[i];
      for (j = te->edges; (k = od->edgedata[j]) != 0; j += 2)
	{
	  if ((od->edgedata[j + 1] & TYPE_BROKEN) != 0)
	    continue;
	  /* see if we can reach a cycle TE from k */
	  if (!od->tes[k].mark)
	    reachable(od, k);
	  if (od->tes[k].mark == -1)
	    {
#if 0
	      printf("adding FROM HEAD cycle edge %d->%d %s->%s [%s]!\n", head, k, pool_solvid2str(pool, od->tes[head].p), pool_solvid2str(pool, od->tes[k].p), pool_solvid2str(pool, od->tes[cycle[i]].p));
#endif
	      addteedge(od, head, k, TYPE_CYCLEHEAD);
	      od->tes[k].mark = -2;	/* no need to add that one again */
	    }
	}
    }
}

void
transaction_order(Transaction *trans, int flags)
{
  Pool *pool = trans->pool;
  Queue *tr = &trans->steps;
  Repo *installed = pool->installed;
  Id p;
  Solvable *s;
  int i, j, k, numte, numedge;
  struct orderdata od;
  struct _TransactionElement *te;
  Queue todo, obsq, samerepoq, uninstq;
  int cycstart, cycel;
  Id *cycle;
  int oldcount;
  int start, now;
  Repo *lastrepo;
  int lastmedia;
  Id *temedianr;

  start = now = solv_timems(0);
  POOL_DEBUG(SOLV_DEBUG_STATS, "ordering transaction\n");
  /* free old data if present */
  if (trans->orderdata)
    {
      struct _TransactionOrderdata *od = trans->orderdata;
      od->tes = solv_free(od->tes);
      od->invedgedata = solv_free(od->invedgedata);
      trans->orderdata = solv_free(trans->orderdata);
    }

  /* create a transaction element for every active component */
  numte = 0;
  for (i = 0; i < tr->count; i++)
    {
      p = tr->elements[i];
      s = pool->solvables + p;
      if (installed && s->repo == installed && trans->transaction_installed[p - installed->start])
	continue;
      numte++;
    }
  POOL_DEBUG(SOLV_DEBUG_STATS, "transaction elements: %d\n", numte);
  if (!numte)
    return;	/* nothing to do... */

  numte++;	/* leave first one zero */
  memset(&od, 0, sizeof(od));
  od.trans = trans;
  od.ntes = numte;
  od.tes = solv_calloc(numte, sizeof(*od.tes));
  od.edgedata = solv_extend(0, 0, 1, sizeof(Id), EDGEDATA_BLOCK);
  od.edgedata[0] = 0;
  od.nedgedata = 1;
  queue_init(&od.cycles);

  /* initialize TEs */
  for (i = 0, te = od.tes + 1; i < tr->count; i++)
    {
      p = tr->elements[i];
      s = pool->solvables + p;
      if (installed && s->repo == installed && trans->transaction_installed[p - installed->start])
	continue;
      te->p = p;
      te++;
    }

  /* create dependency graph */
  for (i = 0; i < tr->count; i++)
    addsolvableedges(&od, pool->solvables + tr->elements[i]);

  /* count edges */
  numedge = 0;
  for (i = 1, te = od.tes + i; i < numte; i++, te++)
    for (j = te->edges; od.edgedata[j]; j += 2)
      numedge++;
  POOL_DEBUG(SOLV_DEBUG_STATS, "edges: %d, edge space: %d\n", numedge, od.nedgedata / 2);
  POOL_DEBUG(SOLV_DEBUG_STATS, "edge creation took %d ms\n", solv_timems(now));

#if 0
  dump_tes(&od);
#endif

  now = solv_timems(0);
  /* kill all cycles */
  queue_init(&todo);
  for (i = numte - 1; i > 0; i--)
    queue_push(&todo, i);

  while (todo.count)
    {
      i = queue_pop(&todo);
      /* printf("- look at TE %d\n", i); */
      if (i < 0)
	{
	  i = -i;
	  od.tes[i].mark = 2;	/* done with that one */
	  continue;
	}
      te = od.tes + i;
      if (te->mark == 2)
	continue;		/* already finished before */
      if (te->mark == 0)
	{
	  int edgestovisit = 0;
	  /* new node, visit edges */
	  for (j = te->edges; (k = od.edgedata[j]) != 0; j += 2)
	    {
	      if ((od.edgedata[j + 1] & TYPE_BROKEN) != 0)
		continue;
	      if (od.tes[k].mark == 2)
		continue;	/* no need to visit again */
	      if (!edgestovisit++)
	        queue_push(&todo, -i);	/* end of edges marker */
	      queue_push(&todo, k);
	    }
	  if (!edgestovisit)
	    te->mark = 2;	/* no edges, done with that one */
	  else
	    te->mark = 1;	/* under investigation */
	  continue;
	}
      /* oh no, we found a cycle */
      /* find start of cycle node (<0) */
      for (j = todo.count - 1; j >= 0; j--)
	if (todo.elements[j] == -i)
	  break;
      assert(j >= 0);
      cycstart = j;
      /* build te/edge chain */
      k = cycstart;
      for (j = k; j < todo.count; j++)
	if (todo.elements[j] < 0)
	  todo.elements[k++] = -todo.elements[j];
      cycel = k - cycstart;
      assert(cycel > 1);
      /* make room for edges, two extra element for cycle loop + terminating 0 */
      while (todo.count < cycstart + 2 * cycel + 2)
	queue_push(&todo, 0);
      cycle = todo.elements + cycstart;
      cycle[cycel] = i;		/* close the loop */
      cycle[2 * cycel + 1] = 0;	/* terminator */
      for (k = cycel; k > 0; k--)
	{
	  cycle[k * 2] = cycle[k];
	  te = od.tes + cycle[k - 1];
	  assert(te->mark == 1);
	  te->mark = 0;	/* reset investigation marker */
	  /* printf("searching for edge from %d to %d\n", cycle[k - 1], cycle[k]); */
	  for (j = te->edges; od.edgedata[j]; j += 2)
	    if (od.edgedata[j] == cycle[k])
	      break;
	  assert(od.edgedata[j]);
	  cycle[k * 2 - 1] = j;
	}
      /* now cycle looks like this: */
      /* te1 edge te2 edge te3 ... teN edge te1 0 */
      breakcycle(&od, cycle);
      /* restart with start of cycle */
      todo.count = cycstart + 1;
    }
  POOL_DEBUG(SOLV_DEBUG_STATS, "cycles broken: %d\n", od.ncycles);
  POOL_DEBUG(SOLV_DEBUG_STATS, "cycle breaking took %d ms\n", solv_timems(now));

  now = solv_timems(0);
  /* now go through all broken cycles and create cycle edges to help
     the ordering */
   for (i = od.cycles.count - 4; i >= 0; i -= 4)
     {
       if (od.cycles.elements[i + 2] >= TYPE_REQ)
         addcycleedges(&od, od.cyclesdata.elements + od.cycles.elements[i], &todo);
     }
   for (i = od.cycles.count - 4; i >= 0; i -= 4)
     {
       if (od.cycles.elements[i + 2] < TYPE_REQ)
         addcycleedges(&od, od.cyclesdata.elements + od.cycles.elements[i], &todo);
     }
  POOL_DEBUG(SOLV_DEBUG_STATS, "cycle edge creation took %d ms\n", solv_timems(now));

#if 0
  dump_tes(&od);
#endif
  /* all edges are finally set up and there are no cycles, now the easy part.
   * Create an ordered transaction */
  now = solv_timems(0);
  /* first invert all edges */
  for (i = 1, te = od.tes + i; i < numte; i++, te++)
    te->mark = 1;	/* term 0 */
  for (i = 1, te = od.tes + i; i < numte; i++, te++)
    {
      for (j = te->edges; od.edgedata[j]; j += 2)
        {
	  if ((od.edgedata[j + 1] & TYPE_BROKEN) != 0)
	    continue;
	  od.tes[od.edgedata[j]].mark++;
	}
    }
  j = 1;
  for (i = 1, te = od.tes + i; i < numte; i++, te++)
    {
      te->mark += j;
      j = te->mark;
    }
  POOL_DEBUG(SOLV_DEBUG_STATS, "invedge space: %d\n", j + 1);
  od.invedgedata = solv_calloc(j + 1, sizeof(Id));
  for (i = 1, te = od.tes + i; i < numte; i++, te++)
    {
      for (j = te->edges; od.edgedata[j]; j += 2)
        {
	  if ((od.edgedata[j + 1] & TYPE_BROKEN) != 0)
	    continue;
	  od.invedgedata[--od.tes[od.edgedata[j]].mark] = i;
	}
    }
  for (i = 1, te = od.tes + i; i < numte; i++, te++)
    te->edges = te->mark;	/* edges now points into invedgedata */
  od.edgedata = solv_free(od.edgedata);
  od.nedgedata = j + 1;

  /* now the final ordering */
  for (i = 1, te = od.tes + i; i < numte; i++, te++)
    te->mark = 0;
  for (i = 1, te = od.tes + i; i < numte; i++, te++)
    for (j = te->edges; od.invedgedata[j]; j++)
      od.tes[od.invedgedata[j]].mark++;

  queue_init(&samerepoq);
  queue_init(&uninstq);
  queue_empty(&todo);
  for (i = 1, te = od.tes + i; i < numte; i++, te++)
    if (te->mark == 0)
      {
	if (installed && pool->solvables[te->p].repo == installed)
          queue_push(&uninstq, i);
	else
          queue_push(&todo, i);
      }
  assert(todo.count > 0 || uninstq.count > 0);
  oldcount = tr->count;
  queue_empty(tr);

  queue_init(&obsq);

  lastrepo = 0;
  lastmedia = 0;
  temedianr = solv_calloc(numte, sizeof(Id));
  for (i = 1; i < numte; i++)
    {
      Solvable *s = pool->solvables + od.tes[i].p;
      if (installed && s->repo == installed)
	j = 1;
      else
        j = solvable_lookup_num(s, SOLVABLE_MEDIANR, 1);
      temedianr[i] = j;
    }
  for (;;)
    {
      /* select an TE i */
      if (uninstq.count)
	i = queue_shift(&uninstq);
      else if (samerepoq.count)
	i = queue_shift(&samerepoq);
      else if (todo.count)
	{
	  /* find next repo/media */
	  for (j = 0; j < todo.count; j++)
	    {
	      if (!j || temedianr[todo.elements[j]] < lastmedia)
		{
		  i = j;
		  lastmedia = temedianr[todo.elements[j]];
		}
	    }
	  lastrepo = pool->solvables[od.tes[todo.elements[i]].p].repo;

	  /* move all matching TEs to samerepoq */
	  for (i = j = 0; j < todo.count; j++)
	    {
	      int k = todo.elements[j];
	      if (temedianr[k] == lastmedia && pool->solvables[od.tes[k].p].repo == lastrepo)
		queue_push(&samerepoq, k);
	      else
		todo.elements[i++] = k;
	    }
	  todo.count = i;

	  assert(samerepoq.count);
	  i = queue_shift(&samerepoq);
	}
      else
	break;

      te = od.tes + i;
      queue_push(tr, te->p);
#if 0
printf("do %s [%d]\n", pool_solvid2str(pool, te->p), temedianr[i]);
#endif
      s = pool->solvables + te->p;
      for (j = te->edges; od.invedgedata[j]; j++)
	{
	  struct _TransactionElement *te2 = od.tes + od.invedgedata[j];
	  assert(te2->mark > 0);
	  if (--te2->mark == 0)
	    {
	      Solvable *s = pool->solvables + te2->p;
#if 0
printf("free %s [%d]\n", pool_solvid2str(pool, te2->p), temedianr[od.invedgedata[j]]);
#endif
	      if (installed && s->repo == installed)
	        queue_push(&uninstq, od.invedgedata[j]);
	      else if (s->repo == lastrepo && temedianr[od.invedgedata[j]] == lastmedia)
	        queue_push(&samerepoq, od.invedgedata[j]);
	      else
	        queue_push(&todo, od.invedgedata[j]);
	    }
	}
    }
  solv_free(temedianr);
  queue_free(&todo);
  queue_free(&samerepoq);
  queue_free(&uninstq);
  queue_free(&obsq);
  for (i = 1, te = od.tes + i; i < numte; i++, te++)
    assert(te->mark == 0);

  /* add back obsoleted packages */
  transaction_add_obsoleted(trans);
  assert(tr->count == oldcount);

  POOL_DEBUG(SOLV_DEBUG_STATS, "creating new transaction took %d ms\n", solv_timems(now));
  POOL_DEBUG(SOLV_DEBUG_STATS, "transaction ordering took %d ms\n", solv_timems(start));

  if ((flags & (SOLVER_TRANSACTION_KEEP_ORDERDATA | SOLVER_TRANSACTION_KEEP_ORDERCYCLES)) != 0)
    {
      struct _TransactionOrderdata *tod;
      trans->orderdata = tod = solv_calloc(1, sizeof(*trans->orderdata));
      if ((flags & SOLVER_TRANSACTION_KEEP_ORDERCYCLES) != 0)
	{
	  Queue *cycles = tod->cycles = solv_calloc(1, sizeof(Queue));
	  queue_init_clone(cycles, &od.cyclesdata);
	  /* map from tes to packages */
	  for (i = 0; i < cycles->count; i++)
	    if (cycles->elements[i])
	      cycles->elements[i] = od.tes[cycles->elements[i]].p;
	  queue_insertn(cycles, cycles->count, od.cycles.count, od.cycles.elements);
	  queue_push(cycles, od.cycles.count / 4);
	}
      if ((flags & SOLVER_TRANSACTION_KEEP_ORDERDATA) != 0)
	{
	  tod->tes = od.tes;
	  tod->ntes = numte;
	  tod->invedgedata = od.invedgedata;
	  tod->ninvedgedata = od.nedgedata;
	  od.tes = 0;
	  od.invedgedata = 0;
	}
    }
  solv_free(od.tes);
  solv_free(od.invedgedata);
  queue_free(&od.cycles);
  queue_free(&od.cyclesdata);
}


int
transaction_order_add_choices(Transaction *trans, Id chosen, Queue *choices)
{
  int i, j;
  struct _TransactionOrderdata *od = trans->orderdata;
  struct _TransactionElement *te;

  if (!od)
     return choices->count;
  if (!chosen)
    {
      /* initialization step */
      for (i = 1, te = od->tes + i; i < od->ntes; i++, te++)
	te->mark = 0;
      for (i = 1, te = od->tes + i; i < od->ntes; i++, te++)
	{
	  for (j = te->edges; od->invedgedata[j]; j++)
	    od->tes[od->invedgedata[j]].mark++;
	}
      for (i = 1, te = od->tes + i; i < od->ntes; i++, te++)
	if (!te->mark)
	  queue_push(choices, te->p);
      return choices->count;
    }
  for (i = 1, te = od->tes + i; i < od->ntes; i++, te++)
    if (te->p == chosen)
      break;
  if (i == od->ntes)
    return choices->count;
  if (te->mark > 0)
    {
      /* hey! out-of-order installation! */
      te->mark = -1;
    }
  for (j = te->edges; od->invedgedata[j]; j++)
    {
      te = od->tes + od->invedgedata[j];
      assert(te->mark > 0 || te->mark == -1);
      if (te->mark > 0 && --te->mark == 0)
	queue_push(choices, te->p);
    }
  return choices->count;
}

void
transaction_add_obsoleted(Transaction *trans)
{
  Pool *pool = trans->pool;
  Repo *installed = pool->installed;
  Id p;
  Solvable *s;
  int i, j, k, max;
  Map done;
  Queue obsq, *steps;

  if (!installed || !trans->steps.count)
    return;
  /* calculate upper bound */
  max = 0;
  FOR_REPO_SOLVABLES(installed, p, s)
    if (MAPTST(&trans->transactsmap, p))
      max++;
  if (!max)
    return;
  /* make room */
  steps = &trans->steps;
  queue_insertn(steps, 0, max, 0);

  /* now add em */
  map_init(&done, installed->end - installed->start);
  queue_init(&obsq);
  for (j = 0, i = max; i < steps->count; i++)
    {
      p = trans->steps.elements[i];
      if (pool->solvables[p].repo == installed)
	{
	  if (!trans->transaction_installed[p - pool->installed->start])
	    trans->steps.elements[j++] = p;
	  continue;
	}
      trans->steps.elements[j++] = p;
      queue_empty(&obsq);
      transaction_all_obs_pkgs(trans, p, &obsq);
      for (k = 0; k < obsq.count; k++)
	{
	  p = obsq.elements[k];
	  assert(p >= installed->start && p < installed->end);
	  if (!MAPTST(&trans->transactsmap, p))	/* just in case */
	    continue;
	  if (MAPTST(&done, p - installed->start))
	    continue;
	  MAPSET(&done, p - installed->start);
	  trans->steps.elements[j++] = p;
	}
    }

  /* free unneeded space */
  queue_truncate(steps, j);
  map_free(&done);
  queue_free(&obsq);
}

static void
transaction_check_pkg(Transaction *trans, Id tepkg, Id pkg, Map *ins, Map *seen, int onlyprereq, Id noconfpkg, int depth)
{
  Pool *pool = trans->pool;
  Id p, pp;
  Solvable *s;
  int good;

  if (MAPTST(seen, pkg))
    return;
  MAPSET(seen, pkg);
  s = pool->solvables + pkg;
#if 0
  printf("- %*s%c%s\n", depth * 2, "", s->repo == pool->installed ? '-' : '+', pool_solvable2str(pool, s));
#endif
  if (s->requires)
    {
      Id req, *reqp;
      int inpre = 0;
      reqp = s->repo->idarraydata + s->requires;
      while ((req = *reqp++) != 0)
	{
          if (req == SOLVABLE_PREREQMARKER)
	    {
	      inpre = 1;
	      continue;
	    }
	  if (onlyprereq && !inpre)
	    continue;
	  if (!strncmp(pool_id2str(pool, req), "rpmlib(", 7))
	    continue;
	  good = 0;
	  /* first check kept packages, then freshly installed, then not yet uninstalled */
	  FOR_PROVIDES(p, pp, req)
	    {
	      if (!MAPTST(ins, p))
		continue;
	      if (MAPTST(&trans->transactsmap, p))
		continue;
	      good++;
	      transaction_check_pkg(trans, tepkg, p, ins, seen, 0, noconfpkg, depth + 1);
	    }
	  if (!good)
	    {
	      FOR_PROVIDES(p, pp, req)
		{
		  if (!MAPTST(ins, p))
		    continue;
		  if (pool->solvables[p].repo == pool->installed)
		    continue;
		  good++;
		  transaction_check_pkg(trans, tepkg, p, ins, seen, 0, noconfpkg, depth + 1);
		}
	    }
	  if (!good)
	    {
	      FOR_PROVIDES(p, pp, req)
		{
		  if (!MAPTST(ins, p))
		    continue;
		  good++;
		  transaction_check_pkg(trans, tepkg, p, ins, seen, 0, noconfpkg, depth + 1);
		}
	    }
	  if (!good)
	    {
	      POOL_DEBUG(SOLV_DEBUG_RESULT, "  %c%s: nothing provides %s needed by %c%s\n", pool->solvables[tepkg].repo == pool->installed ? '-' : '+', pool_solvid2str(pool, tepkg), pool_dep2str(pool, req), s->repo == pool->installed ? '-' : '+', pool_solvable2str(pool, s));
	    }
	}
    }
}

void
transaction_check_order(Transaction *trans)
{
  Pool *pool = trans->pool;
  Solvable *s;
  Id p, lastins;
  Map ins, seen;
  int i;

  POOL_DEBUG(SOLV_DEBUG_RESULT, "\nchecking transaction order...\n");
  map_init(&ins, pool->nsolvables);
  map_init(&seen, pool->nsolvables);
  if (pool->installed)
    {
      FOR_REPO_SOLVABLES(pool->installed, p, s)
        MAPSET(&ins, p);
    }
  lastins = 0;
  for (i = 0; i < trans->steps.count; i++)
    {
      p = trans->steps.elements[i];
      s = pool->solvables + p;
      if (s->repo != pool->installed)
	lastins = p;
      if (s->repo != pool->installed)
	MAPSET(&ins, p);
      if (havescripts(pool, p))
	{
	  MAPZERO(&seen);
	  transaction_check_pkg(trans, p, p, &ins, &seen, 1, lastins, 0);
	}
      if (s->repo == pool->installed)
	MAPCLR(&ins, p);
    }
  map_free(&seen);
  map_free(&ins);
  POOL_DEBUG(SOLV_DEBUG_RESULT, "transaction order check done.\n");
}

void
transaction_order_get_cycleids(Transaction *trans, Queue *q, int minseverity)
{
  struct _TransactionOrderdata *od = trans->orderdata;
  Queue *cq;
  int i, cid, ncycles;

  queue_empty(q);
  if (!od || !od->cycles || !od->cycles->count)
    return;
  cq = od->cycles;
  ncycles = cq->elements[cq->count - 1];
  i = cq->count - 1 - ncycles * 4;
  for (cid = 1; cid <= ncycles; cid++, i += 4)
    {
      if (minseverity)
	{
	  int cmin = cq->elements[i + 3] & 0xffff;
	  int cmax = (cq->elements[i + 3] >> 16) & 0xffff;
	  if (minseverity >= SOLVER_ORDERCYCLE_NORMAL && cmin < TYPE_REQ)
	    continue;
	  if (minseverity >= SOLVER_ORDERCYCLE_CRITICAL && (cmax & TYPE_PREREQ) == 0)
	    continue;
	}
      queue_push(q, cid);
    }
}

int
transaction_order_get_cycle(Transaction *trans, Id cid, Queue *q)
{
  struct _TransactionOrderdata *od = trans->orderdata;
  Queue *cq;
  int cmin, cmax, severity;
  int ncycles;

  queue_empty(q);
  if (!od || !od->cycles || !od->cycles->count)
    return SOLVER_ORDERCYCLE_HARMLESS;
  cq = od->cycles;
  ncycles = cq->elements[cq->count - 1];
  if (cid < 1 || cid > ncycles)
    return SOLVER_ORDERCYCLE_HARMLESS;
  cid =  cq->count - 1 - 4 * (ncycles - cid + 1);
  cmin = cq->elements[cid + 3] & 0xffff;
  cmax = (cq->elements[cid + 3] >> 16) & 0xffff;
  if (cmin < TYPE_REQ)
    severity = SOLVER_ORDERCYCLE_HARMLESS;
  else if ((cmax & TYPE_PREREQ) == 0)
    severity = SOLVER_ORDERCYCLE_NORMAL;
  else
    severity = SOLVER_ORDERCYCLE_CRITICAL;
  if (q)
    queue_insertn(q, 0, cq->elements[cid + 1], cq->elements + cq->elements[cid]);
  return severity;
}

