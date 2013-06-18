#include <stdio.h>
#include <stdlib.h>
#include <unistd.h>

#include "pool.h"
#include "repo.h"
#include "solver.h"
#include "selection.h"
#include "solverdebug.h"
#include "testcase.h"

static struct resultflags2str {
  Id flag;
  const char *str;
} resultflags2str[] = {
  { TESTCASE_RESULT_TRANSACTION,        "transaction" },
  { TESTCASE_RESULT_PROBLEMS,           "problems" },
  { TESTCASE_RESULT_ORPHANED,           "orphaned" },
  { TESTCASE_RESULT_RECOMMENDED,        "recommended" },
  { TESTCASE_RESULT_UNNEEDED,           "unneeded" },
  { 0, 0 }
};

static void
usage(ex)
{
  fprintf(ex ? stderr : stdout, "Usage: testsolv <testcase>\n");
  exit(ex);
}

int
main(int argc, char **argv)
{
  Pool *pool;
  Queue job;
  Solver *solv;
  char *result = 0;
  int resultflags = 0;
  int debuglevel = 0;
  int writeresult = 0;
  int multijob = 0;
  int c;
  int ex = 0;
  const char *list = 0;
  FILE *fp;

  while ((c = getopt(argc, argv, "vrhl:")) >= 0)
    {
      switch (c)
      {
        case 'v':
          debuglevel++;
          break;
        case 'r':
          writeresult++;
          break;
        case 'h':
	  usage(0);
          break;
        case 'l':
	  list = optarg;
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

      fp = fopen(argv[optind], "r");
      if (!fp)
	{
	  perror(argv[optind]);
	  exit(0);
	}
      while(!feof(fp))
	{
	  queue_init(&job);
	  result = 0;
	  resultflags = 0;
	  solv = testcase_read(pool, fp, argv[optind], &job, &result, &resultflags);
	  if (!solv)
	    {
	      pool_free(pool);
	      exit(1);
	    }

	  if (!multijob && !feof(fp))
	    multijob = 1;

	  if (multijob)
	    printf("test %d:\n", multijob++);
	  if (list)
	    {
	      queue_empty(&job);
	      selection_make(pool, &job, list, SELECTION_NAME|SELECTION_PROVIDES|SELECTION_FILELIST|SELECTION_CANON|SELECTION_DOTARCH|SELECTION_REL|SELECTION_GLOB|SELECTION_FLAT);
	      if (!job.elements)
		printf("No match\n");
	      else
		{
		  Queue q;
		  int i;
		  queue_init(&q);
		  selection_solvables(pool, &job, &q);
		  for (i = 0; i < q.count; i++)
		    printf("  - %s\n", testcase_solvid2str(pool, q.elements[i]));
		  queue_free(&q);
		}
	    }
	  else if (result || writeresult)
	    {
	      char *myresult, *resultdiff;
	      solver_solve(solv, &job);
	      if (!resultflags)
		resultflags = TESTCASE_RESULT_TRANSACTION | TESTCASE_RESULT_PROBLEMS;
	      myresult = testcase_solverresult(solv, resultflags);
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
	      if (solver_solve(solv, &job))
		{
		  int problem, solution, pcnt, scnt;
		  pcnt = solver_problem_count(solv);
		  printf("Found %d problems:\n", pcnt);
		  for (problem = 1; problem <= pcnt; problem++)
		    {
		      printf("Problem %d:\n", problem);
		      solver_printprobleminfo(solv, problem);
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
	  solver_free(solv);
	}
      pool_free(pool);
      fclose(fp);
    }
  exit(ex);
}
