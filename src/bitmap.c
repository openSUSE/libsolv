/*
 * Copyright (c) 2007, Novell Inc.
 *
 * This program is licensed under the BSD license, read LICENSE.BSD
 * for further information
 */

/*
 * bitmap.c
 * 
 */

#include <stdlib.h>
#include <string.h>

#include "bitmap.h"
#include "util.h"

void
map_init(Map *m, int n)
{
  m->size = (n + 7) >> 3;
  m->map = sat_calloc(m->size, 1);
}

// free space allocated
void
map_free(Map *m)
{
  m->map = sat_free(m->map);
  m->size = 0;
}

// copy t <- s
void
map_init_clone(Map *t, Map *s)
{
  t->size = s->size;
  t->map = sat_malloc(s->size);
  memcpy(t->map, s->map, t->size);
}

// EOF
