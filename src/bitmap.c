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

/* constructor */
void
map_init(Map *m, int n)
{
  m->size = (n + 7) >> 3;
  m->map = m->size ? solv_calloc(m->size, 1) : 0;
}

/* destructor */
void
map_free(Map *m)
{
  m->map = solv_free(m->map);
  m->size = 0;
}

/* copy constructor t <- s */
void
map_init_clone(Map *t, Map *s)
{
  t->size = s->size;
  if (s->size)
    {
      t->map = solv_malloc(s->size);
      memcpy(t->map, s->map, s->size);
    }
  else
    t->map = 0;
}

/* grow a map */
void
map_grow(Map *m, int n)
{
  n = (n + 7) >> 3;
  if (m->size < n)
    {
      m->map = solv_realloc(m->map, n);
      memset(m->map + m->size, 0, n - m->size);
      m->size = n;
    }
}

/* bitwise-ands same-sized maps t and s, stores the result in t. */
void
map_and(Map *t, Map *s)
{
    unsigned char *ti, *si, *end;
    ti = t->map;
    si = s->map;
    end = ti + t->size;
    while (ti < end)
	*ti++ &= *si++;
}

/* like map_and but negates value in s first, i.e. t & ~s */
void
map_and_not(Map *t, Map *s)
{
    unsigned char *ti, *si, *end;
    ti = t->map;
    si = s->map;
    end = ti + t->size;
    while (ti < end)
	*ti++ &= ~*si++;
}

/* EOF */
