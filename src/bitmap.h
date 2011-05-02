/*
 * Copyright (c) 2007-2011, Novell Inc.
 *
 * This program is licensed under the BSD license, read LICENSE.BSD
 * for further information
 */

/*
 * bitmap.h
 *
 */

#ifndef SATSOLVER_BITMAP_H
#define SATSOLVER_BITMAP_H

#include <string.h>

typedef struct _Map {
  unsigned char *map;
  int size;
} Map;

#define MAPZERO(m) (memset((m)->map, 0, (m)->size))
/* set bit */
#define MAPSET(m, n) ((m)->map[(n) >> 3] |= 1 << ((n) & 7))
/* clear bit */
#define MAPCLR(m, n) ((m)->map[(n) >> 3] &= ~(1 << ((n) & 7)))
/* test bit */
#define MAPTST(m, n) ((m)->map[(n) >> 3] & (1 << ((n) & 7)))

extern void map_init(Map *m, int n);
extern void map_init_clone(Map *t, Map *s);
extern void map_grow(Map *m, int n);
extern void map_free(Map *m);

static inline void map_empty(Map *m)
{
  MAPZERO(m);
}
static inline void map_set(Map *m, int n)
{
  MAPSET(m, n);
}
static inline void map_clr(Map *m, int n)
{
  MAPCLR(m, n);
}
static inline int map_tst(Map *m, int n)
{
  return MAPTST(m, n);
}

#endif /* SATSOLVER_BITMAP_H */
