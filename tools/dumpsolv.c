/*
 * Copyright (c) 2007, Novell Inc.
 *
 * This program is licensed under the BSD license, read LICENSE.BSD
 * for further information
 */

#include <stdio.h>
#include <stdlib.h>
#include <unistd.h>
#include <string.h>

#include "pool.h"
#include "repo_solv.h"

static void
printids(Repo *repo, char *kind, Offset ido)
{
  Pool *pool = repo->pool;
  Id id, *ids;
  if (!ido)
    return;
  printf("%s:\n", kind);
  ids = repo->idarraydata + ido;
  while((id = *ids++) != 0)
    printf("  %s\n", dep2str(pool, id));
}

int main(int argc, char **argv)
{
  Repo *repo;
  Pool *pool;
  int i;
  Solvable *s;

  if (argc != 1)
    {
      if (freopen(argv[1], "r", stdin) == 0)
	{
	  perror(argv[1]);
	  exit(1);
	}
    }
  pool = pool_create();
  repo = pool_addrepo_solv(pool, stdin, "");
  printf("repo contains %d solvables\n", repo->nsolvables);
  for (i = repo->start; i < repo->start + repo->nsolvables; i++)
    {
      s = pool->solvables + i;
      printf("\n");
      printf("solvable %d:\n", i);
      printf("name: %s %s %s\n", id2str(pool, s->name), id2str(pool, s->evr), id2str(pool, s->arch));
      if (s->vendor)
        printf("vendor: %s\n", id2str(pool, s->vendor));
      printids(repo, "provides", s->provides);
      printids(repo, "obsoletes", s->obsoletes);
      printids(repo, "conflicts", s->conflicts);
      printids(repo, "requires", s->requires);
      printids(repo, "recommends", s->recommends);
      printids(repo, "suggests", s->suggests);
      printids(repo, "supplements", s->supplements);
      printids(repo, "enhances", s->enhances);
      printids(repo, "freshens", s->freshens);
    }
  exit(0);
}
