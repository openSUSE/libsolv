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

/* copy constructor target <- source */
void
map_init_clone(Map *target, const Map *source)
{
  target->size = source->size;
  if (source->size)
    {
      target->map = solv_malloc(source->size);
      memcpy(target->map, source->map, source->size);
    }
  else
    target->map = 0;
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

/* bitwise-ands maps t and s, stores the result in t. */
void
map_and(Map *t, const Map *s)
{
  unsigned char *ti, *si, *end;
  ti = t->map;
  si = s->map;
  end = ti + (t->size < s->size ? t->size : s->size);
  while (ti < end)
    *ti++ &= *si++;
}

/* bitwise-ors maps t and s, stores the result in t. */
void
map_or(Map *t, const Map *s)
{
  unsigned char *ti, *si, *end;
  if (t->size < s->size)
    map_grow(t, s->size << 3);
  ti = t->map;
  si = s->map;
  end = ti + (t->size < s->size ? t->size : s->size);
  while (ti < end)
    *ti++ |= *si++;
}

/* remove all set bits in s from t. */
void
map_subtract(Map *t, const Map *s)
{
  unsigned char *ti, *si, *end;
  ti = t->map;
  si = s->map;
  end = ti + (t->size < s->size ? t->size : s->size);
  while (ti < end)
    *ti++ &= ~*si++;
}

void
map_invertall(Map *m)
{
  unsigned char *ti, *end;
  ti = m->map;
  end = ti + m->size;
  while (ti < end)
    *ti++ ^= 0xff;
}

/* EOF */
