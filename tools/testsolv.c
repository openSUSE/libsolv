#include <stdio.h>
#include <stdlib.h>
#include <unistd.h>
#include <getopt.h>

#include "pool.h"
#include "repo.h"
#include "solver.h"
#include "selection.h"
#include "solverdebug.h"
#include "testcase.h"
#include "evr.h"

static struct resultflags2str {
  Id flag;
  const char *str;
} resultflags2str[] = {
  { TESTCASE_RESULT_TRANSACTION,        "transaction" },
  { TESTCASE_RESULT_PROBLEMS,           "problems" },
  { TESTCASE_RESULT_ORPHANED,           "orphaned" },
  { TESTCASE_RESULT_RECOMMENDED,        "recommended" },
  { TESTCASE_RESULT_UNNEEDED,           "unneeded" },
  { TESTCASE_RESULT_ALTERNATIVES,       "alternatives" },
  { TESTCASE_RESULT_RULES,              "rules" },
  { TESTCASE_RESULT_GENID,              "genid" },
  { TESTCASE_RESULT_REASON,             "reason" },
  { TESTCASE_RESULT_CLEANDEPS,          "cleandeps" },
  { TESTCASE_RESULT_JOBS,               "jobs" },
  { 0, 0 }
};

static void
usage(int ex)
{
  fprintf(ex ? stderr : stdout, "Usage: testsolv <testcase>\n");
  exit(ex);
}

struct reportsolutiondata {
  int count;
  char *result;
};

static int
reportsolutioncb(Solver *solv, void *cbdata)
{
  struct reportsolutiondata *sd = cbdata;
  char *res;

  sd->count++;
  res = testcase_solverresult(solv, TESTCASE_RESULT_TRANSACTION);
  if (*res)
    {
      char prefix[64];
      char *p2, *p = res;
      sprintf(prefix, "callback%d:", sd->count);
      while ((p2 = strchr(p, '\n')) != 0)
	{
	  char c = p2[1];
	  p2[1] = 0;
	  sd->result = solv_dupappend(sd->result, prefix, p);
	  p2[1] = c;
	  p = p2 + 1;
	}
    }
  solv_free(res);
  return 0;
}

static void
free_considered(Pool *pool)
{
  if (pool->considered)
    {
      map_free(pool->considered);
      pool->considered = solv_free(pool->considered);
    }
}

void
showwhy(Solver *solv, const char *showwhypkgstr)
{
  Pool *pool = solv->pool;
  Queue dq, rq, iq;
  int ii, i;

  queue_init(&dq);
  queue_init(&rq);
  queue_init(&iq);

  i = testcase_str2solvid(pool, showwhypkgstr);
  if (i)
    solver_get_decisionlist(solv, i, &dq);
  else
    {
      int selflags = SELECTION_NAME | SELECTION_CANON;
      selection_make(pool, &dq, showwhypkgstr, selflags);
      selection_solvables(pool, &dq, &iq);
      if (!iq.count)
	printf("No package matches %s\n", showwhypkgstr);
      queue_empty(&dq);
      solver_get_decisionlist_multiple(solv, &iq, &dq);
    }
  for (ii = 0; ii < dq.count; ii += 3)
    {
      Id v = dq.elements[ii];
      int reason = dq.elements[ii + 1];
      int info = dq.elements[ii + 2];
      Id vv = (v > 0 ? v : -v);

      printf("%s %s because\n", v > 0 ? "installed" : "conflicted", testcase_solvid2str(pool, vv));
      switch(reason)
	{
	case SOLVER_REASON_WEAKDEP:
	  solver_allweakdepinfos(solv, vv, &iq);
	  if (!iq.count)
	    printf("  of some weak dependency\n");
	  for (i = 0; i < iq.count; i += 4)
	     printf("  %s\n", solver_ruleinfo2str(solv, iq.elements[i], iq.elements[i + 1], iq.elements[i + 2], iq.elements[i + 3]));
	  break;
	case SOLVER_REASON_UNIT_RULE:
	case SOLVER_REASON_RESOLVE:
	  solver_allruleinfos(solv, info, &iq);
	  if (!iq.count)
	    printf("  of some rule\n");
	  for (i = 0; i < iq.count; i += 4)
	    {
	      if (iq.elements[i] == SOLVER_RULE_LEARNT)
		{
		  solver_ruleliterals(solv, info, &rq);
		  printf("  of a learnt rule:\n");
		  for (i = 0; i < rq.count; i++)
		    {
		      Id p2 = rq.elements[i];
		      printf("    %c %s\n", p2 > 0 ? '+' : '-', testcase_solvid2str(pool, p2 > 0 ? p2 : -p2));
		    }
		  continue;
		}
	      printf("  %s\n", solver_ruleinfo2str(solv, iq.elements[i], iq.elements[i + 1], iq.elements[i + 2], iq.elements[i + 3]));
	    }
	  break;
	case SOLVER_REASON_KEEP_INSTALLED:
	  printf("  we want to keep it installed\n");
	  break;
	case SOLVER_REASON_RESOLVE_JOB:
	  i = solver_rule2jobidx(solv, info);
	  printf("  a job was to %s\n", pool_job2str(pool, solv->job.elements[i], solv->job.elements[i + 1], 0));
	  break;
	case SOLVER_REASON_UPDATE_INSTALLED:
	  printf("  we want to update/keep it\n");
	  break;
	case SOLVER_REASON_CLEANDEPS_ERASE:
	  printf("  we want to cleandeps erase it\n");
	  break;
	case SOLVER_REASON_RESOLVE_ORPHAN:
	  printf("  it is orphaned\n");
	  break;
	case SOLVER_REASON_UNRELATED:
	  printf("  it was unrelated\n");
	  break;
	default:
	  printf("  of some reason\n");
	  break;
	}
    }
  queue_free(&iq);
  queue_free(&rq);
  queue_free(&dq);
}

static int
multipkg_evrcmp(Pool *pool, Id a, Id b)
{
  Solvable *as = pool->solvables + a, *bs = pool->solvables + b;
  return as->evr != bs->evr ? pool_evrcmp(pool, as->evr, bs->evr, EVRCMP_COMPARE) : 0;
}

static int
multipkg_sortcmp(const void *va, const void *vb, void *vd)
{
  Pool *pool = vd;
  Solvable *as = pool->solvables + *(Id *)va, *bs = pool->solvables + *(Id *)vb;
  if (as->name != bs->name)
    {
      int r = strcmp(pool_id2str(pool, as->name), pool_id2str(pool, bs->name));
      if (r)
	return r;
      return as->name - bs->name;
    }
  if (as->evr != bs->evr)
    {
      int r = pool_evrcmp(pool, as->evr, bs->evr, EVRCMP_COMPARE);
      if (r)
        return r;
    }
  return *(Id *)va - *(Id *)vb;
}

const char *
striprelease(Pool *pool, Id evr, Id otherevr)
{
  const char *evrstr = pool_id2str(pool, evr);
  const char *r = strchr(evrstr, '-');
  char *evrstr2;
  int cmp;
  if (!r)
    return evrstr;
  evrstr2 = pool_tmpjoin(pool, evrstr, 0, 0);
  evrstr2[r - evrstr] = 0;
  cmp = pool_evrcmp_str(pool, evrstr2, pool_id2str(pool, otherevr), pool->disttype != DISTTYPE_DEB ? EVRCMP_MATCH_RELEASE : EVRCMP_COMPARE);
  return cmp == 1 ? evrstr2 : evrstr;
}

const char *
multipkg(Pool *pool, Queue *q)
{
  Queue pq;
  Queue pr;
  char *s = 0;
  int i, j, k, kstart;
  Id name = 0;

  if (!q->count)
    return "no package";
  if (q->count == 1)
    return pool_solvid2str(pool, q->elements[0]);
  queue_init_clone(&pq, q);
  queue_init(&pr);
  solv_sort(pq.elements, pq.count, sizeof(Id), multipkg_sortcmp, pool);

  for (i = 0; i < pq.count; i++)
    {
      Id p = pq.elements[i];
      if (s)
	s = pool_tmpappend(pool, s, ", ", 0);

      if (i == 0 || pool->solvables[p].name != name)
	{
	  Id p2, pp2;
	  name = pool->solvables[p].name;
	  queue_empty(&pr);
	  FOR_PROVIDES(p2, pp2, name)
	    if (pool->solvables[p].name == name)
	      queue_push(&pr, p2);
	  if (pr.count > 1)
	    solv_sort(pr.elements, pr.count, sizeof(Id), multipkg_sortcmp, pool);
	}

      for (k = 0; k < pr.count; k++)
	if (pr.elements[k] == p)
	  break;
      if (k == pr.count)
	{
	  /* not in provides, list as singularity */
	  s = pool_tmpappend(pool, s, pool_solvid2str(pool, pq.elements[i]), 0);
	  continue;
	}
      if (k && multipkg_evrcmp(pool, pr.elements[k], pr.elements[k - 1]) == 0)
	{
	  /* unclear start, list as single package */
	  s = pool_tmpappend(pool, s, pool_solvid2str(pool, pq.elements[i]), 0);
	  continue;
	}
      kstart = k;
      for (j = i + 1, k = k + 1; j < pq.count; j++, k++)
        if (k == pr.count || pq.elements[j] != pr.elements[k])
	  break;
      while (j > i + 1 && k && k < pr.count && multipkg_evrcmp(pool, pr.elements[k], pr.elements[k - 1]) == 0)
	{
	  j--;
	  k--;
	}
      if (k == 0 || j == i + 1)
	{
	  s = pool_tmpappend(pool, s, pool_solvid2str(pool, pq.elements[i]), 0);
	  continue;
	}
      /* create an interval */
      s = pool_tmpappend(pool, s, pool_id2str(pool, name), 0);
      if (kstart > 0)
        s = pool_tmpappend(pool, s, " >= ", striprelease(pool, pool->solvables[pr.elements[kstart]].evr, pool->solvables[pr.elements[kstart - 1]].evr));
      if (k < pr.count)
        s = pool_tmpappend(pool, s, " < ", striprelease(pool, pool->solvables[pr.elements[k]].evr, pool->solvables[pr.elements[k - 1]].evr));
      i = j - 1;
    }
  queue_free(&pq);
  queue_free(&pr);
  return s;
}

void
doshowproof(Solver *solv, Id problem, int islearnt, Queue *lq)
{
  Pool *pool = solv->pool;
  Queue q, qp;
  int i, j, k;
  int comb;

  queue_init(&q);
  queue_init(&qp);
  solver_get_proof(solv, problem, islearnt, &q);
  for (i = 0; i < q.count; i += 6)
    {
      Id truelit = q.elements[i];
      Id type = q.elements[i + 2];
      Id from = q.elements[i + 3];
      Id to = q.elements[i + 4];
      Id dep = q.elements[i + 5];
      Id name;
      const char *action = truelit < 0 ? "conflicted" : "installed";
      if (truelit && (type == SOLVER_RULE_PKG_NOTHING_PROVIDES_DEP || type == SOLVER_RULE_PKG_CONFLICTS || type == SOLVER_RULE_PKG_REQUIRES || type == SOLVER_RULE_DISTUPGRADE || type == SOLVER_RULE_INFARCH || type == SOLVER_RULE_PKG_OBSOLETES || type == SOLVER_RULE_PKG_INSTALLED_OBSOLETES || type == SOLVER_RULE_PKG_IMPLICIT_OBSOLETES))
	{
	  comb = 0;
	  name = 0;
	  for (j = i + 6; j < q.count - 6; j += 6)
	    {
	      if (truelit > 0 && q.elements[j] <= 0)
		break;
	      if (truelit < 0 && q.elements[j] >= 0)
		break;
	      if (type != q.elements[j + 2])
		break;
	      if (dep != q.elements[j + 5])
		break;
	      if (!comb)
		{
		  if (from == q.elements[j + 3] && to == (truelit > 0 ? truelit : -truelit))
		    {
		      comb = 1;
		      name = to ? pool->solvables[to].name : 0;
		    }
		  else if (to == q.elements[j + 4] && from == (truelit > 0 ? truelit : -truelit))
		    {
		      comb = 2;
		      name = from ? pool->solvables[from].name : 0;
		    }
		  else
		    break;
		}
	      if (comb == 1 && (from != q.elements[j + 3] || pool->solvables[q.elements[j + 4]].name != name))
		break;
	      if (comb == 2 && (to != q.elements[j + 4] || pool->solvables[q.elements[j + 3]].name != name))
		break;
	      if (comb == 1 && q.elements[j + 4] != (q.elements[j] > 0 ? q.elements[j] : -q.elements[j]))
		break;
	      if (comb == 2 && q.elements[j + 3] != (q.elements[j] > 0 ? q.elements[j] : -q.elements[j]))
		break;
	    }
	  if (comb)
	    {
	      queue_empty(&qp);
	      for (k = i; k < j; k += 6)
		queue_push(&qp, q.elements[k] > 0 ? q.elements[k] : -q.elements[k]);
	      switch (type)
		{
		case SOLVER_RULE_DISTUPGRADE:
		  printf("%s %s: do not belong to a distupgrade repository\n", action, multipkg(pool, &qp));
		  break;
		case SOLVER_RULE_INFARCH:
		  printf("%s %s: have inferior architecture\n", action, multipkg(pool, &qp));
		  break;
		case SOLVER_RULE_PKG_NOTHING_PROVIDES_DEP:
		  printf("%s %s: nothing provides %s\n", action, multipkg(pool, &qp), pool_dep2str(pool, dep));
		  break;
		case SOLVER_RULE_PKG_CONFLICTS:
		  if (comb == 1)
		    printf("%s %s: %s conflicts with %s\n", action, multipkg(pool, &qp), pool_solvid2str(pool, from), pool_dep2str(pool, dep));
		  else
		    printf("%s %s: the packages conflict with %s provided by %s\n", action, multipkg(pool, &qp), pool_dep2str(pool, dep), pool_solvid2str(pool, to));
		  break;
		case SOLVER_RULE_PKG_OBSOLETES:
		  if (comb == 1)
		    printf("%s %s: %s obsoletes %s\n", action, multipkg(pool, &qp), pool_solvid2str(pool, from), pool_dep2str(pool, dep));
		  else
		    printf("%s %s: the packages obsolete %s provided by %s\n", action, multipkg(pool, &qp), pool_dep2str(pool, dep), pool_solvid2str(pool, to));
		  break;
		case SOLVER_RULE_PKG_IMPLICIT_OBSOLETES:
		  if (comb == 1)
		    printf("%s %s: %s implicitly obsoletes %s\n", action, multipkg(pool, &qp), pool_solvid2str(pool, from), pool_dep2str(pool, dep));
		  else
		    printf("%s %s: the packages implicitly obsolete %s provided by %s\n", action, multipkg(pool, &qp), pool_dep2str(pool, dep), pool_solvid2str(pool, to));
		  break;
		case SOLVER_RULE_PKG_INSTALLED_OBSOLETES:
		  if (comb == 1)
		    printf("%s %s: installed %s obsoletes %s\n", action, multipkg(pool, &qp), pool_solvid2str(pool, from), pool_dep2str(pool, dep));
		  else
		    printf("%s %s: the installed packages obsolete %s provided by %s\n", action, multipkg(pool, &qp), pool_dep2str(pool, dep), pool_solvid2str(pool, to));
		  break;
		case SOLVER_RULE_PKG_REQUIRES:
		  printf("%s %s: the packages require %s\n", action, multipkg(pool, &qp), pool_dep2str(pool, dep));
		  break;
		}
	      i = j - 6;
	      continue;
	    }
	}
      if (i + 6 < q.count && type == SOLVER_RULE_PKG_SAME_NAME)
	continue;	/* obvious */
      if (truelit != 0)
	{
	  if (lq && type == SOLVER_RULE_LEARNT)
	    {
	      for (j = 0; j < lq->count; j++)
		if (lq->elements[j] == q.elements[i + 1])
		  break;
	      if (j < lq->count)
		{
		  printf("%s %s: learnt rule #%d\n", action, pool_solvid2str(pool, truelit >= 0 ? truelit : -truelit), j + 1);
		  continue;
		}
	    }
	  if (islearnt && type == 0)
	    {
	      printf("%s %s: learnt rule premise\n", action, pool_solvid2str(pool, truelit >= 0 ? truelit : -truelit));
	      continue;
	    }
          printf("%s %s: %s\n", action, pool_solvid2str(pool, truelit >= 0 ? truelit : -truelit), solver_ruleinfo2str(solv, type, from, to, dep));
	}
      else
        printf("unsolvable: %s\n", solver_ruleinfo2str(solv, type, from, to, dep));
    }
  queue_free(&qp);
  queue_free(&q);
}

int
main(int argc, char **argv)
{
  Pool *pool;
  Queue job;
  Queue solq;
  Solver *solv, *reusesolv = 0;
  char *result = 0;
  char *showwhypkgstr = 0;
  int resultflags = 0;
  int debuglevel = 0;
  int writeresult = 0;
  char *writetestcase = 0;
  int multijob = 0;
  int rescallback = 0;
  int showproof = 0;
  int c;
  int ex = 0;
  const char *list = 0;
  int list_with_deps = 0;
  FILE *fp;
  const char *p;

  queue_init(&solq);
  while ((c = getopt(argc, argv, "vmrhL:l:s:T:W:P")) >= 0)
    {
      switch (c)
      {
        case 'v':
          debuglevel++;
          break;
        case 'r':
          writeresult++;
          break;
        case 'm':
          rescallback = 1;
          break;
        case 'h':
	  usage(0);
          break;
        case 'l':
	  list = optarg;
	  list_with_deps = 0;
          break;
        case 'L':
	  list = optarg;
	  list_with_deps = 1;
          break;
        case 'W':
	  showwhypkgstr = optarg;
          break;
        case 's':
	  if ((p = strchr(optarg, ':')))
	    queue_push2(&solq, atoi(optarg), atoi(p + 1));
	  else
	    queue_push2(&solq, 1, atoi(optarg));
          break;
        case 'T':
	  writetestcase = optarg;
          break;
        case 'P':
	  showproof = 1;
          break;
        default:
	  usage(1);
          break;
      }
    }
  if (optind == argc)
    usage(1);
  for (; optind < argc; optind++)
    {
      pool = pool_create();
      pool_setdebuglevel(pool, debuglevel);
      /* report all errors */
      pool_setdebugmask(pool, pool->debugmask | SOLV_ERROR);

      fp = fopen(argv[optind], "r");
      if (!fp)
	{
	  perror(argv[optind]);
	  exit(0);
	}
      while (!feof(fp))
	{
	  queue_init(&job);
	  result = 0;
	  resultflags = 0;
	  solv = testcase_read(pool, fp, argv[optind], &job, &result, &resultflags);
	  if (!solv)
	    {
	      free_considered(pool);
	      pool_free(pool);
	      queue_free(&job);
	      exit(resultflags == 77 ? 77 : 1);
	    }
	  if (reusesolv)
	    {
	      solver_free(solv);
	      solv = reusesolv;
	      reusesolv = 0;
	    }
	  if (!multijob && !feof(fp))
	    multijob = 1;

	  if (multijob)
	    printf("test %d:\n", multijob++);
	  if (list)
	    {
	      Id p = 0;
	      int selflags = SELECTION_NAME|SELECTION_PROVIDES|SELECTION_CANON|SELECTION_DOTARCH|SELECTION_REL|SELECTION_GLOB|SELECTION_FLAT;
	      if (*list == '/')
		selflags |= SELECTION_FILELIST;
	      queue_empty(&job);
	      if (list_with_deps)
	        p = testcase_str2solvid(pool, list);
	      if (p)
		queue_push2(&job, SOLVER_SOLVABLE, p);
	      else
	        selection_make(pool, &job, list, selflags);
	      if (!job.elements)
		printf("No match\n");
	      else
		{
		  Queue q;
		  int i;
		  queue_init(&q);
		  selection_solvables(pool, &job, &q);
		  for (i = 0; i < q.count; i++)
		    {
		      printf("  - %s\n", testcase_solvid2str(pool, q.elements[i]));
		      if (list_with_deps)
			{
			  int j, k;
			  const char *vendor;
			  static Id deps[] = {
			    SOLVABLE_PROVIDES, SOLVABLE_REQUIRES, SOLVABLE_CONFLICTS, SOLVABLE_OBSOLETES,
			    SOLVABLE_RECOMMENDS, SOLVABLE_SUGGESTS, SOLVABLE_SUPPLEMENTS, SOLVABLE_ENHANCES,
			    SOLVABLE_PREREQ_IGNOREINST,
			    0
			  };
			  vendor = pool_lookup_str(pool, q.elements[i], SOLVABLE_VENDOR);
			  if (vendor)
			    printf("    %s: %s\n", pool_id2str(pool, SOLVABLE_VENDOR), vendor);
			  for (j = 0; deps[j]; j++)
			    {
			      Queue dq;
			      queue_init(&dq);
			      pool_lookup_idarray(pool, q.elements[i], deps[j], &dq);
			      if (dq.count)
			        printf("    %s:\n", pool_id2str(pool, deps[j]));
			      for (k = 0; k < dq.count; k++)
			        printf("      %s\n", pool_dep2str(pool, dq.elements[k]));
			      queue_free(&dq);
			    }
			}
		    }
		  queue_free(&q);
		}
	    }
	  else if (showwhypkgstr)
	    {
	      solver_solve(solv, &job);
	      showwhy(solv, showwhypkgstr);
	    }
	  else if (showproof)
	    {
	      int pcnt = solver_solve(solv, &job);
	      int problem;
	      if (!pcnt)
		printf("nothing to proof\n");
	      for (problem = 1; problem <= pcnt; problem++)
		{
		  Queue lq;
		  int i;
		  queue_init(&lq);
		  solver_get_learnt(solv, problem, 0, &lq);
		  for (i = 0; i < lq.count; i++)
		    {
		      printf("Learnt rule #%d:\n", i + 1);
		      doshowproof(solv, lq.elements[i], 1, &lq);
		      printf("\n");
		    }
		  printf("Proof #%d:\n", problem);
		  doshowproof(solv, problem, 0, &lq);
		  queue_free(&lq);
		  if (problem < pcnt)
		    printf("\n");
		}
	    }
	  else if (result || writeresult)
	    {
	      char *myresult, *resultdiff;
	      struct reportsolutiondata reportsolutiondata;
	      memset(&reportsolutiondata, 0, sizeof(reportsolutiondata));
	      if (rescallback)
		{
		  solv->solution_callback = reportsolutioncb;
		  solv->solution_callback_data = &reportsolutiondata;
		}
	      solver_solve(solv, &job);
	      solv->solution_callback = 0;
	      solv->solution_callback_data = 0;
	      if ((resultflags & ~TESTCASE_RESULT_REUSE_SOLVER) == 0)
		resultflags |= TESTCASE_RESULT_TRANSACTION | TESTCASE_RESULT_PROBLEMS;
	      myresult = testcase_solverresult(solv, resultflags);
	      if (rescallback && reportsolutiondata.result)
		{
		  reportsolutiondata.result = solv_dupjoin(reportsolutiondata.result, myresult, 0);
		  solv_free(myresult);
		  myresult = reportsolutiondata.result;
		}
	      if (writeresult)
		{
		  if (*myresult)
		    {
		      if (writeresult > 1)
			{
			  const char *p;
			  int i;
			  
			  printf("result ");
			  p = "%s";
			  for (i = 0; resultflags2str[i].str; i++)
			    if ((resultflags & resultflags2str[i].flag) != 0)
			      {
			        printf(p, resultflags2str[i].str);
			        p = ",%s";
			      }
			  printf(" <inline>\n");
			  p = myresult;
			  while (*p)
			    {
			      const char *p2 = strchr(p, '\n');
			      p2 = p2 ? p2 + 1 : p + strlen(p);
			      printf("#>%.*s", (int)(p2 - p), p);
			      p = p2;
			    }
			}
		      else
			printf("%s", myresult);
		    }
		}
	      else
		{
		  resultdiff = testcase_resultdiff(result, myresult);
		  if (resultdiff)
		    {
		      printf("Results differ:\n%s", resultdiff);
		      ex = 1;
		      solv_free(resultdiff);
		    }
		}
	      solv_free(result);
	      solv_free(myresult);
	    }
	  else
	    {
	      int pcnt = solver_solve(solv, &job);
	      if (writetestcase)
		{
		  if (!testcase_write(solv, writetestcase, resultflags, 0, 0))
		    {
		      fprintf(stderr, "Could not write testcase: %s\n", pool_errstr(pool));
		      exit(1);
		    }
		}
	      if (pcnt && solq.count)
		{
		  int i, taken = 0;
		  for (i = 0; i < solq.count; i += 2)
		    {
		      if (solq.elements[i] > 0 && solq.elements[i] <= pcnt)
			if (solq.elements[i + 1] > 0 && solq.elements[i + 1] <=  solver_solution_count(solv, solq.elements[i]))
			  {
			    printf("problem %d: taking solution %d\n", solq.elements[i], solq.elements[i + 1]);
			    solver_take_solution(solv, solq.elements[i], solq.elements[i + 1], &job);
			    taken = 1;
			  }
		    }
		  if (taken)
		    pcnt = solver_solve(solv, &job);
		}
	      if (pcnt)
		{
		  int problem, solution, scnt;
		  printf("Found %d problems:\n", pcnt);
		  for (problem = 1; problem <= pcnt; problem++)
		    {
		      printf("Problem %d:\n", problem);
#if 1
		      solver_printprobleminfo(solv, problem);
#else
		      {
			Queue pq;
			int j;
			queue_init(&pq);
			solver_findallproblemrules(solv, problem, &pq);
			for (j = 0; j < pq.count; j++)
			  solver_printproblemruleinfo(solv, pq.elements[j]);
			queue_free(&pq);
		      }
#endif
		      printf("\n");
		      scnt = solver_solution_count(solv, problem);
		      for (solution = 1; solution <= scnt; solution++)
			{
			  printf("Solution %d:\n", solution);
			  solver_printsolution(solv, problem, solution);
			  printf("\n");
			}
		    }
		}
	      else
		{
		  Transaction *trans = solver_create_transaction(solv);
		  printf("Transaction summary:\n\n");
		  transaction_print(trans);
		  transaction_free(trans);
		}
	    }
	  queue_free(&job);
	  if ((resultflags & TESTCASE_RESULT_REUSE_SOLVER) != 0 && !feof(fp))
	    reusesolv = solv;
	  else
	    solver_free(solv);
	}
      if (reusesolv)
	solver_free(reusesolv);
      free_considered(pool);
      pool_free(pool);
      fclose(fp);
    }
  queue_free(&solq);
  exit(ex);
}
