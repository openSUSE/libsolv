/*
 * Copyright (c) 2019, SUSE LLC
 *
 * This program is licensed under the BSD license, read LICENSE.BSD
 * for further information
 */

/*
 * conda2solv.c
 *
 * parse a conda repository file
 *
 * reads from stdin
 * writes to stdout
 */

#include <dirent.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/types.h>
#include <unistd.h>

#include "common_write.h"
#include "conda.h"
#include "pool.h"
#include "repo.h"
#include "repo_conda.h"
#include "solv_xfopen.h"
#include "solverdebug.h"

static void usage(int status) {
  fprintf(stderr, "\nUsage:\n"
                  "conda2solv\n"
                  "  reads a 'synthesis' repository from <stdin> and writes a "
                  ".solv file to <stdout>\n"
                  "  -h : print help & exit\n");
  exit(status);
}

int main(int argc, char **argv) {
  Pool *pool;
  Repo *repo;
  int c;
  DIR *d;
  char dir_name[PATH_MAX + 1];
  int n_install = 0;
  char to_install[1024];
  char* cur_offset = to_install;
  struct dirent *dir;

  while ((c = getopt(argc, argv, "hi:d:")) != -1) {
    switch (c) {
    case 'h':
      usage(0);
      break;
    case 'd':
      printf("checking dir: %s\n", optarg);
      d = opendir(optarg);
      strcpy(dir_name, optarg);
      if (dir_name[strlen(dir_name) - 1] != '/')
      {
        strcat(dir_name, "/");
      }
      break;
    case 'i':
      strcpy(cur_offset, optarg);
      cur_offset += strlen(optarg) + 1;
      n_install++;
      break;
    default:
      usage(1);
      break;
    }
  }

  pool = pool_create();
  pool_setdisttype(pool, DISTTYPE_CONDA);
  pool_setdebuglevel(pool, 0);

  if (d)
  {
    char fn[PATH_MAX + 1];
    while ((dir = readdir(d)) != NULL)
    {
      fn[0] = '\0';
      strcat(fn, dir_name);
      strcat(fn, dir->d_name);
      printf("%s\n", fn);
      char *dot = strrchr(fn, '.');
      if (dot && !strcmp(dot, ".json"))
      {
        repo = repo_create(pool, dir->d_name);
        if (repo_add_conda(repo, fopen(fn, "r"), 0))
        {
          fprintf(stderr, "conda2solv: %s\n", pool_errstr(pool));
          exit(1);
        }
        repo_internalize(repo);
      }

    }
    closedir(d);
    
    pool_createwhatprovides(pool);
    Solver *solvy = solver_create(pool);
    solver_set_flag(solvy, SOLVER_FLAG_ALLOW_DOWNGRADE, 1);

    Queue q;
    queue_init(&q);

    char* cur_install = to_install;
    for (int i = 0; i < n_install; ++i)
    {
      Id inst_id = pool_conda_matchspec(pool, cur_install);
      cur_install += strlen(cur_install) + 1;
      queue_push2(&q, SOLVER_INSTALL | REL_CONDA, inst_id);
      printf("Adding rule for %s\n", pool_dep2str(pool, inst_id));
    }

    solver_solve(solvy, &q);

    Transaction *transy = solver_create_transaction(solvy);

    transaction_print(transy);

    int cnt = solver_problem_count(solvy);
    Queue problem_queue;
    queue_init(&problem_queue);

    printf("No problems: %i", cnt);
    for (int i = 1; i <= cnt; i++) {
      queue_push(&problem_queue, i);
      printf("Problem %s", solver_problem2str(solvy, i));
    }

    pool_free(pool);
    exit(0);
  }
}