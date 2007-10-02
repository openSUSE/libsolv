/*
 * bitmap.c
 * 
 */

#include <stdlib.h>
#include <string.h>

#include "bitmap.h"

void
mapinit(Map *m, int n)
{
  m->size = (n + 7) >> 3;
  m->map = calloc(m->size, 1);
}

// free space allocated
void
mapfree(Map *m)
{
  free(m->map);
  m->map = 0;
  m->size = 0;
}

// copy t <- s
void
clonemap(Map *t, Map *s)
{
  t->size = s->size;
  t->map = malloc(s->size);
  memcpy(t->map, s->map, t->size);
}

// EOF
