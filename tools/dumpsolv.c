#include <stdio.h>
#include <stdlib.h>
#include <unistd.h>
#include <string.h>

#include "pool.h"
#include "source_solv.h"

static void
printids(Pool *pool, char *kind, Id *ids)
{
  Id id;
  if (!ids)
    return;
  printf("%s:\n", kind);
  while((id = *ids++) != 0)
    printf("  %s\n", dep2str(pool, id));
}

int main(int argc, char **argv)
{
  Source *source;
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
  source = pool_addsource_solv(pool, stdin, "");
  printf("source contains %d solvables\n", source->nsolvables);
  for (i = source->start; i < source->start + source->nsolvables; i++)
    {
      s = pool->solvables + i;
      printf("\n");
      printf("solvable %d:\n", i);
      printf("name: %s %s %s\n", id2str(pool, s->name), id2str(pool, s->evr), id2str(pool, s->arch));
      printids(pool, "provides", s->provides);
      printids(pool, "obsoletes", s->obsoletes);
      printids(pool, "conflicts", s->conflicts);
      printids(pool, "requires", s->requires);
      printids(pool, "recommends", s->recommends);
      printids(pool, "suggests", s->suggests);
      printids(pool, "supplements", s->supplements);
      printids(pool, "enhances", s->enhances);
      printids(pool, "freshens", s->freshens);
    }
  exit(0);
}
