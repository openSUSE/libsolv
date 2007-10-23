#include <stdio.h>
#include <stdlib.h>
#include <unistd.h>
#include <string.h>

#include "pool.h"
#include "source_solv.h"

static void
printids(Source *source, char *kind, Offset ido)
{
  Pool *pool = source->pool;
  Id id, *ids;
  if (!ido)
    return;
  printf("%s:\n", kind);
  ids = source->idarraydata + ido;
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
      printids(source, "provides", s->provides);
      printids(source, "obsoletes", s->obsoletes);
      printids(source, "conflicts", s->conflicts);
      printids(source, "requires", s->requires);
      printids(source, "recommends", s->recommends);
      printids(source, "suggests", s->suggests);
      printids(source, "supplements", s->supplements);
      printids(source, "enhances", s->enhances);
      printids(source, "freshens", s->freshens);
    }
  exit(0);
}
