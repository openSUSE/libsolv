#include <stdio.h>
#include <stdlib.h>
#include <unistd.h>

#include "pool.h"
#include "repo.h"
#include "solver.h"
#include "solverdebug.h"
#include "testcase.h"

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
  int c;
  int ex = 0;

  while ((c = getopt(argc, argv, "vh")) >= 0)
    {
      switch (c)
      {
        case 'v':
          debuglevel++;
          break;
        case 'h':
	  usage(0);
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
      queue_init(&job);
      solv = testcase_read(pool, 0, argv[optind], &job, &result, &resultflags);
      if (!solv)
	{
	  pool_free(pool);
	  exit(1);
	}

      if (result)
	{
	  char *myresult, *resultdiff;
	  solver_solve(solv, &job);
	  myresult = testcase_solverresult(solv, resultflags);
	  resultdiff = testcase_resultdiff(result, myresult);
	  if (resultdiff)
	    {
	      printf("Results differ:\n%s", resultdiff);
	      ex = 1;
	      solv_free(resultdiff);
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
      pool_free(pool);
    }
  exit(ex);
}
