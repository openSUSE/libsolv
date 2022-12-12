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
    solver_get_decisionlist(solv, i, SOLVER_DECISIONLIST_SOLVABLE, &dq);
  else
    {
      int selflags = SELECTION_NAME | SELECTION_CANON;
      selection_make(pool, &dq, showwhypkgstr, selflags);
      selection_solvables(pool, &dq, &iq);
      if (!iq.count)
	printf("No package matches %s\n", showwhypkgstr);
      queue_empty(&dq);
      solver_get_decisionlist_multiple(solv, &iq, SOLVER_DECISIONLIST_SOLVABLE, &dq);
    }
  for (ii = 0; ii < dq.count; ii += 3)
    {
      Id v = dq.elements[ii];
      int reason = dq.elements[ii + 1];
      int info = dq.elements[ii + 2];

      printf("%s %s:\n", v < 0 ? "conflicted" : "installed", testcase_solvid2str(pool, v >= 0 ? v : -v));
      /* special case some reasons where we want to show multiple rule infos or extra info */
      if (reason == SOLVER_REASON_WEAKDEP || reason == SOLVER_REASON_UNIT_RULE || reason == SOLVER_REASON_RESOLVE)
	{
	  queue_empty(&iq);
	  if (reason == SOLVER_REASON_WEAKDEP)
	    solver_allweakdepinfos(solv, v, &iq);
	  else if (info > 0)
	    solver_allruleinfos(solv, info, &iq);
	  if (iq.count)
	    {
	      for (i = 0; i < iq.count; i += 4)
		{
		  int bits;
		  if (iq.elements[i] == SOLVER_RULE_LEARNT)
		    {
		      printf("  a learnt rule:\n");
		      solver_ruleliterals(solv, info, &rq);
		      for (i = 0; i < rq.count; i++)
			{
			  Id p2 = rq.elements[i];
			  printf("    %c %s\n", p2 > 0 ? '+' : '-', testcase_solvid2str(pool, p2 > 0 ? p2 : -p2));
			}
		      continue;
		    }
		  bits = solver_calc_decisioninfo_bits(solv, v, iq.elements[i], iq.elements[i + 1], iq.elements[i + 2], iq.elements[i + 3]);
		  printf("  %s\n", solver_decisioninfo2str(solv, bits, iq.elements[i], iq.elements[i + 1], iq.elements[i + 2], iq.elements[i + 3]));
		}
	      continue;
	    }
	}
      printf("  %s\n", solver_decisionreason2str(solv, v, reason, info));
    }
  queue_free(&iq);
  queue_free(&rq);
  queue_free(&dq);
}

void
doshowproof(Solver *solv, Id id, int flags, Queue *lq)
{
  Pool *pool = solv->pool;
  Queue q, qp;
  int i, j;

  queue_init(&q);
  queue_init(&qp);
  solver_get_decisionlist(solv, id, flags | SOLVER_DECISIONLIST_SORTED | SOLVER_DECISIONLIST_WITHINFO | SOLVER_DECISIONLIST_MERGEDINFO, &q);
  for (i = 0; i < q.count; i += 8)
    {
      Id v = q.elements[i];
      int reason = q.elements[i + 1], bits = q.elements[i + 3], type = q.elements[i + 4];
      Id from = q.elements[i + 5], to = q.elements[i + 6], dep = q.elements[i + 7];

      if (reason != SOLVER_REASON_UNSOLVABLE && type == SOLVER_RULE_PKG_SAME_NAME)
	continue;	/* do not show "obvious" decisions */

      solver_decisionlist_solvables(solv, &q, i, &qp);
      if (reason == SOLVER_REASON_UNSOLVABLE)
        printf("unsolvable: ");
      else
        printf("%s %s: ", v < 0 ? "conflicted" : "installed", pool_solvidset2str(pool, &qp));
      i += solver_decisionlist_merged(solv, &q, i) * 8;
      if (type == 0)
	{
	  printf("%s\n", solver_reason2str(solv, reason));
	  continue;
	}
      if (type == SOLVER_RULE_LEARNT && lq)
	{
	  for (j = 0; j < lq->count; j++)
	    if (lq->elements[j] == q.elements[i + 2])
	      break;
	  if (j < lq->count)
	    {
	      printf("learnt rule #%d\n", j + 1);
	      continue;
	    }
	}
      printf("%s\n", solver_decisioninfo2str(solv, bits, type, from, to, dep));
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
		  solver_get_learnt(solv, problem, SOLVER_DECISIONLIST_PROBLEM, &lq);
		  for (i = 0; i < lq.count; i++)
		    {
		      printf("Learnt rule #%d:\n", i + 1);
		      doshowproof(solv, lq.elements[i], SOLVER_DECISIONLIST_LEARNTRULE, &lq);
		      printf("\n");
		    }
		  printf("Proof #%d:\n", problem);
		  doshowproof(solv, problem, SOLVER_DECISIONLIST_PROBLEM, &lq);
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
