/*
 * bitmap.c
 * 
 */

#include <stdlib.h>
#include <string.h>

#include "bitmap.h"
#include "util.h"

void
mapinit(Map *m, int n)
{
  m->size = (n + 7) >> 3;
  m->map = xcalloc(m->size, 1);
}

// free space allocated
void
mapfree(Map *m)
{
  m->map = xfree(m->map);
  m->size = 0;
}

// copy t <- s
void
clonemap(Map *t, Map *s)
{
  t->size = s->size;
  t->map = xmalloc(s->size);
  memcpy(t->map, s->map, t->size);
}

// EOF
